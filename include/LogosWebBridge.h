#pragma once

#include <QHash>
#include <QJSValue>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>

class QRemoteObjectNode;
class QRemoteObjectDynamicReplica;

// ── LogosWebBridge ──────────────────────────────────────────────────────────
//
// `logos`, as a module's QML sees it INSIDE THE WEB CONTAINER.
//
// The same three calls the desktop bridge offers a view — module(),
// callModuleAsync(), watch() — over a completely different set of parts. On the
// desktop LogosQmlBridge holds a LogosAPI, a token store and a local socket per
// view module; here there is one QtRO node over one MessagePort (ADR 0004) and
// nothing else, because inside a Qt-for-WebAssembly image LogosAPI and the
// native SDK stack do not exist.
//
//     property var backend: logos.module("counter")   // typed-ish replica
//     Text  { text: backend.value }                   // a property change repaints
//     Button{ onClicked: backend.increment() }        // a slot drives the backend
//
//     logos.callModuleAsync("keystore", "unlock", [pin], function (payload) {
//         console.log(JSON.parse(payload).value)      // a JSON STRING, as on desktop
//     })
//
// TWO DIFFERENCES FROM THE DESKTOP BRIDGE, both forced and both visible here:
//
//   - module() hands back a DYNAMIC replica. A desktop host loads the view
//     module's generated factory plugin to get a typed one; a page cannot
//     dlopen, so the types come off the wire instead. Properties, slots and
//     signals are all reached the same way from QML either way.
//   - callModule() — the synchronous form — is REFUSED. It is not a policy
//     choice: the answer has to cross a MessagePort, a MessagePort delivers
//     through the event loop, and a page that spins its event loop waiting has
//     stopped rendering. It answers with the same error payload shape as
//     everything else, naming callModuleAsync.
//
// The bridge does not own the node and does not connect it — LogosWebRuntime
// does both, because what a node is connected to is the page's business.
class LogosWebBridge : public QObject
{
    Q_OBJECT
public:
    // `node` must outlive the bridge. Null is legal and every call then answers
    // with an error payload, which is what a runtime with no port yet is.
    explicit LogosWebBridge(QRemoteObjectNode* node, QObject* parent = nullptr);
    ~LogosWebBridge() override;

    // ── view modules: a replica, not a call ─────────────────────────────

    // The replica for a module's backend — and NULL until that backend is
    // there. Calling it early is how the acquire starts, so a view calls it
    // from Component.onCompleted and takes the answer again on
    // viewModuleReadyChanged. The long version of why is on the implementation;
    // the short version is that Qt's QML engine caches a property cache for an
    // object the first time it sees it, and a dynamic replica does not have its
    // real metaobject yet. Cached, so two views of one module share one
    // replica.
    Q_INVOKABLE QObject* module(const QString& moduleName);

    // Also LogosQmlBridge's name, for the same reason the signal below shares
    // one: a module's QML asks the same question of both containers.
    Q_INVOKABLE bool isViewModuleReady(const QString& moduleName) const;

    // Re-emit viewModuleReadyChanged for every replica already Valid. Replicas
    // outlive an engine; a view rebuilt against a fresh one would otherwise
    // wait forever for an edge that already happened. Same contract as
    // LogosQmlBridge::replayViewModuleState().
    void replayModuleState();

    // ── backend modules: a call over the host's router ──────────────────

    // Held, not refused, while the router is not there yet — the ordinary state
    // during startup, since the runtime comes up before the Worker that hosts
    // the router. `timeoutMs > 0` bounds that wait as well as the call itself;
    // the callback then gets an error payload. Pass 0 to wait forever.
    //
    // The callback fires EXACTLY ONCE, with a JSON STRING.
    Q_INVOKABLE void callModuleAsync(const QString& module,
                                     const QString& method,
                                     const QVariantList& args,
                                     QJSValue callback,
                                     int timeoutMs = 30000);

    // Always an error payload naming callModuleAsync. See the class note.
    Q_INVOKABLE QString callModule(const QString& module,
                                   const QString& method,
                                   const QVariantList& args = QVariantList());

    // Resolve the QRemoteObjectPendingCall a replica slot with a return value
    // hands back, so a view never imports QtRemoteObjects. Identical contract
    // to LogosQmlBridge::watch(), including the "not a pending call? then it is
    // already the value" branch.
    Q_INVOKABLE void watch(const QVariant& pendingCall,
                           QJSValue onSuccess,
                           QJSValue onError = QJSValue());

    // Diagnostics: request ids asked and not yet answered. Exposed so a test —
    // and a page's console — can tell "waiting for the backend" apart from
    // "answered and the view ignored it".
    Q_INVOKABLE QStringList pendingCallIds() const;

Q_SIGNALS:
    // THE SAME NAME LogosQmlBridge USES, and that is the point: a module's QML
    // takes its backend on this edge in BOTH containers, so one document runs
    // on the desktop and inside the Web container without knowing which it is
    // in. Two spellings of one edge would make the container an author's
    // problem, which is exactly what ADR 0004 is trying to avoid.
    void viewModuleReadyChanged(const QString& moduleName, bool ready);

private Q_SLOTS:
    // A SLOT, and it has to be: the router is a DYNAMIC replica, so its
    // callCompleted signal exists only in a runtime metaobject and can only be
    // connected to through the string-based SIGNAL()/SLOT() form.
    void onCallCompleted(const QString& requestId, const QString& payloadJson);

private:
    QRemoteObjectDynamicReplica* replicaFor(const QString& moduleName);
    void ensureRouter();
    void onRouterInitialized();
    void deliver(const QString& requestId, const QString& payloadJson);
    void dispatch(const QString& requestId, const QString& module,
                  const QString& method, const QString& argsJson);
    QJSValue toJsValue(const QVariant& v) const;

    struct PendingCall {
        QJSValue callback;
        QString module;
        QString method;
        QString argsJson;
        bool dispatched = false;
    };

    QPointer<QRemoteObjectNode> m_node;
    QHash<QString, QRemoteObjectDynamicReplica*> m_replicas;
    QRemoteObjectDynamicReplica* m_router = nullptr;
    bool m_routerConnected = false;
    QHash<QString, PendingCall> m_pending;
    quint64 m_nextRequestId = 0;
};
