#include "LogosWebBridge.h"

#include "LogosWebCallRouter.h"
#include "LogosWebPayload.h"

#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QQmlEngine>
#include <QRemoteObjectDynamicReplica>
#include <QRemoteObjectNode>
#include <QRemoteObjectPendingCall>
#include <QRemoteObjectPendingCallWatcher>
#include <QTimer>

Q_LOGGING_CATEGORY(lcWebBridge, "logos.web.bridge")

namespace {

QString argsToJson(const QVariantList& args)
{
    return QString::fromUtf8(
        QJsonDocument(QJsonArray::fromVariantList(args)).toJson(QJsonDocument::Compact));
}

} // namespace

LogosWebBridge::LogosWebBridge(QRemoteObjectNode* node, QObject* parent)
    : QObject(parent)
    , m_node(node)
{
    // REGISTERED BY NAME, and it has to be by name. A dynamic replica builds
    // its metaobject from the wire, so the return type of a slot that answers
    // with a value is the STRING "QRemoteObjectPendingCall"; QML resolves that
    // through QMetaType's name table, which Q_DECLARE_METATYPE alone does not
    // populate. Without this, `logos.watch(backend.sum(2, 3), …)` fails in QML
    // with "Unknown method return type" and the view never learns why.
    //
    // A typed replica needs none of it — repc registers the reply types it
    // generates — which is why the desktop bridge has no line like this.
    static const int registered =
        qRegisterMetaType<QRemoteObjectPendingCall>("QRemoteObjectPendingCall");
    Q_UNUSED(registered);
}

LogosWebBridge::~LogosWebBridge() = default;

// ── view modules ────────────────────────────────────────────────────────────

QRemoteObjectDynamicReplica* LogosWebBridge::replicaFor(const QString& moduleName)
{
    auto it = m_replicas.constFind(moduleName);
    if (it != m_replicas.cend() && it.value())
        return it.value();

    if (!m_node) {
        qCWarning(lcWebBridge) << "module(" << moduleName
                               << "): no node — the page has handed over no port";
        return nullptr;
    }

    // DYNAMIC, because a page cannot dlopen a typed replica factory. Acquiring
    // before the source exists is the normal case and not an error: the node's
    // reconnect timer is already on it.
    QRemoteObjectDynamicReplica* replica = m_node->acquireDynamic(moduleName);
    if (!replica)
        return nullptr;

    replica->setParent(this);
    m_replicas.insert(moduleName, replica);

    QPointer<LogosWebBridge> self(this);
    const QString name = moduleName;
    connect(replica, &QRemoteObjectReplica::stateChanged, this,
            [self, name](QRemoteObjectReplica::State newState,
                         QRemoteObjectReplica::State /*old*/) {
                if (!self)
                    return;
                emit self->viewModuleReadyChanged(name, newState == QRemoteObjectReplica::Valid);
            });
    return replica;
}

QObject* LogosWebBridge::module(const QString& moduleName)
{
    QRemoteObjectDynamicReplica* replica = replicaFor(moduleName);
    if (!replica)
        return nullptr;

    // NULL UNTIL THE SOURCE'S META HAS ARRIVED, and this is the one place the
    // Web container's bridge cannot behave like the desktop's.
    //
    // A dynamic replica BUILDS ITS METAOBJECT from the wire. Qt's QML engine
    // builds a property cache for an object the first time JS touches it and
    // keeps it; hand a replica over early and QML caches the generic
    // QRemoteObjectReplica metaobject — nine properties, none of the module's —
    // and no later arrival of the real one dislodges it. The module's
    // properties then read `undefined` forever and its slots are "not a
    // function", which is a far worse failure than a view that waits.
    //
    // So a view takes its backend when the bridge says it is there:
    //
    //     property var backend: null
    //     Component.onCompleted: {
    //         logos.viewModuleReadyChanged.connect(function (name, ready) {
    //             if (name === "counter" && ready) root.backend = logos.module(name)
    //         })
    //         root.backend = logos.module("counter")   // starts the acquire
    //     }
    //
    // The early call is not wasted: it is what puts the replica on the node in
    // the first place, and the null it returns is the placeholder state a first
    // paint has to have anyway. On the desktop, where the replica is TYPED and
    // its metaobject is compiled in, LogosQmlBridge::module() hands the object
    // back immediately — same QML, one fewer round trip.
    if (replica->state() != QRemoteObjectReplica::Valid)
        return nullptr;

    QQmlEngine::setObjectOwnership(replica, QQmlEngine::CppOwnership);
    return replica;
}

bool LogosWebBridge::isViewModuleReady(const QString& moduleName) const
{
    auto it = m_replicas.constFind(moduleName);
    if (it == m_replicas.cend() || !it.value())
        return false;
    return it.value()->state() == QRemoteObjectReplica::Valid;
}

void LogosWebBridge::replayViewModuleState()
{
    for (auto it = m_replicas.cbegin(); it != m_replicas.cend(); ++it) {
        if (it.value() && it.value()->state() == QRemoteObjectReplica::Valid)
            emit viewModuleReadyChanged(it.key(), true);
    }
}

// ── backend modules ─────────────────────────────────────────────────────────

void LogosWebBridge::ensureRouter()
{
    if (m_router || !m_node)
        return;

    m_router = m_node->acquireDynamic(LogosWebCallRouter::sourceName());
    if (!m_router)
        return;
    m_router->setParent(this);

    // A DYNAMIC replica has no metaobject until the source's meta arrives, so
    // `callCompleted(QString,QString)` cannot be connected to before then —
    // SIGNAL() would resolve to nothing and fail silently. Qt's own dynamic
    // replica documentation names initialized() as the moment this becomes
    // possible, and it is also the moment the queued calls below can go out.
    connect(m_router, &QRemoteObjectDynamicReplica::initialized,
            this, &LogosWebBridge::onRouterInitialized);
    if (m_router->isInitialized())
        onRouterInitialized();
}

void LogosWebBridge::onRouterInitialized()
{
    if (!m_routerConnected) {
        m_routerConnected = connect(m_router, SIGNAL(callCompleted(QString, QString)),
                                    this, SLOT(onCallCompleted(QString, QString)));
        if (!m_routerConnected) {
            qCWarning(lcWebBridge)
                << "the backend's" << LogosWebCallRouter::sourceName()
                << "source has no callCompleted(QString,QString): nothing can be"
                   " answered over it";
            return;
        }
    }

    // Everything asked while the Worker was still coming up.
    const QStringList waiting = m_pending.keys();
    for (const QString& id : waiting) {
        auto it = m_pending.find(id);
        if (it == m_pending.end() || it->dispatched)
            continue;
        it->dispatched = true;
        dispatch(id, it->module, it->method, it->argsJson);
    }
}

void LogosWebBridge::dispatch(const QString& requestId, const QString& module,
                              const QString& method, const QString& argsJson)
{
    QMetaObject::invokeMethod(m_router, "call",
                              Q_ARG(QString, requestId),
                              Q_ARG(QString, module),
                              Q_ARG(QString, method),
                              Q_ARG(QString, argsJson));
}

void LogosWebBridge::callModuleAsync(const QString& module,
                                     const QString& method,
                                     const QVariantList& args,
                                     QJSValue callback,
                                     int timeoutMs)
{
    const QString requestId = QString::number(++m_nextRequestId);

    PendingCall pending;
    pending.callback = callback;
    pending.module = module;
    pending.method = method;
    pending.argsJson = argsToJson(args);
    m_pending.insert(requestId, pending);

    if (timeoutMs > 0) {
        QPointer<LogosWebBridge> self(this);
        QTimer::singleShot(timeoutMs, this, [self, requestId, module, method]() {
            if (!self)
                return;
            self->deliver(requestId,
                          logosWebErrorPayload(QStringLiteral("Call timed out"), module, method,
                                               QStringLiteral("the backend did not answer in time")));
        });
    }

    if (!m_node) {
        deliver(requestId,
                logosWebErrorPayload(QStringLiteral("No backend"), module, method,
                                     QStringLiteral("the page has handed this runtime no port")));
        return;
    }

    ensureRouter();
    if (!m_router) {
        deliver(requestId,
                logosWebErrorPayload(QStringLiteral("No backend router"), module, method));
        return;
    }

    auto it = m_pending.find(requestId);
    if (it == m_pending.end() || it->dispatched)
        return;   // already answered, or already sent by ensureRouter() above:
                  // acquiring a router whose source is already known initialises
                  // the replica there and then, and onRouterInitialized() drains
                  // everything pending — this one included.

    // HELD, NOT REFUSED, until the router's source meta arrives. A view calls
    // from Component.onCompleted, which is routinely before the Worker has
    // instantiated its image — the same race the desktop bridge holds calls
    // through, for the same reason.
    if (!m_router->isInitialized())
        return;

    it->dispatched = true;
    dispatch(requestId, module, method, it->argsJson);
}

void LogosWebBridge::onCallCompleted(const QString& requestId, const QString& payloadJson)
{
    deliver(requestId, payloadJson);
}

void LogosWebBridge::deliver(const QString& requestId, const QString& payloadJson)
{
    // ERASE FIRST. A reply that races the timeout must not invoke the view's
    // callback twice, and taking the entry out is what makes the second
    // delivery a no-op by construction rather than by a flag someone forgets.
    auto it = m_pending.find(requestId);
    if (it == m_pending.end())
        return;
    QJSValue callback = it->callback;
    m_pending.erase(it);

    if (callback.isCallable())
        callback.call(QJSValueList() << QJSValue(payloadJson));
}

QStringList LogosWebBridge::pendingCallIds() const
{
    return m_pending.keys();
}

QString LogosWebBridge::callModule(const QString& module,
                                   const QString& method,
                                   const QVariantList& /*args*/)
{
    // NOT A POLICY. The answer has to cross a MessagePort, which delivers
    // through the event loop; a page that blocked its event loop waiting for
    // one has stopped rendering and stopped reading the port it is waiting on,
    // which is a deadlock rather than a slow call.
    return logosWebErrorPayload(
        QStringLiteral("Synchronous calls are not available in the Web container"),
        module, method,
        QStringLiteral("use logos.callModuleAsync: a MessagePort answers through "
                       "the event loop, so blocking for a reply blocks the reply"));
}

// ── watch ───────────────────────────────────────────────────────────────────

QJSValue LogosWebBridge::toJsValue(const QVariant& v) const
{
    if (auto* engine = qjsEngine(this))
        return engine->toScriptValue(v);
    return QJSValue(v.toString());
}

void LogosWebBridge::watch(const QVariant& pendingCall,
                           QJSValue onSuccess,
                           QJSValue onError)
{
    // canConvert, not a metatype comparison: a typed reply is a SUBCLASS of
    // QRemoteObjectPendingCall with its own metatype. See LogosQmlBridge::watch.
    if (!pendingCall.canConvert<QRemoteObjectPendingCall>()) {
        if (!pendingCall.isValid()) {
            qCWarning(lcWebBridge) << "watch: called with an invalid value — the backend"
                                      " slot probably does not exist, or the module is"
                                      " not available yet";
            if (onError.isCallable())
                onError.call(QJSValueList() << QJSValue(QStringLiteral("call failed")));
            return;
        }
        if (onSuccess.isCallable())
            onSuccess.call(QJSValueList() << toJsValue(pendingCall));
        return;
    }

    auto call = pendingCall.value<QRemoteObjectPendingCall>();
    if (call.isFinished()) {
        if (onSuccess.isCallable())
            onSuccess.call(QJSValueList() << toJsValue(call.returnValue()));
        return;
    }

    auto* watcher = new QRemoteObjectPendingCallWatcher(call, this);
    connect(watcher, &QRemoteObjectPendingCallWatcher::finished, this,
            [this, onSuccess, onError, watcher]() mutable {
                const QVariant rv = watcher->returnValue();
                if (rv.isValid() && onSuccess.isCallable())
                    onSuccess.call(QJSValueList() << toJsValue(rv));
                else if (!rv.isValid() && onError.isCallable())
                    onError.call(QJSValueList() << QJSValue(QStringLiteral("call failed")));
                watcher->deleteLater();
            });
}
