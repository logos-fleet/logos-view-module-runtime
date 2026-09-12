#include "LogosWebRuntime.h"

#include "LogosMessagePortTransport.h"
#include "LogosWebBridge.h"

#include <QLoggingCategory>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QRemoteObjectNode>
#include <QUrl>

#include <memory>

Q_LOGGING_CATEGORY(lcWebRuntime, "logos.web.runtime")

namespace {

// THE BASE URL A MODULE'S DOCUMENT IS GIVEN. It has to be a real scheme Qt's
// QML engine will resolve relative imports against, and it must not be one the
// document could use to read something it was not given: `qrc:` is the image's
// own resources and a module has nothing in there, so a relative reference
// resolves to a resource that does not exist rather than to a file, a URL or
// another module's document. ADR 0004 names the same choice for the same
// reason.
QUrl documentUrlFor(const QString& moduleName)
{
    return QUrl(QStringLiteral("qrc:/logos/modules/%1/Main.qml").arg(moduleName));
}

// A VIEW AND THE CONTEXT IT WAS CREATED IN, in this order and not by parenting
// one to the other: a QML object is torn down against its context, so the
// context has to outlive it by exactly that much. Both places that destroy a
// view — removal and the runtime's own teardown — go through here.
void destroyView(QObject* view, QQmlContext* context)
{
    delete view;
    delete context;
}

} // namespace

LogosWebRuntime::LogosWebRuntime(QQmlEngine* engine, QObject* parent)
    : QObject(parent)
    , m_engine(engine)
    , m_node(new QRemoteObjectNode(this))
    , m_bridge(new LogosWebBridge(m_node, this))
{
    if (m_engine) {
        // `logos` on the ROOT context, so every module's view sees the same
        // bridge without the runtime app having to wire each one. Ownership
        // stays in C++: the engine must not collect the bridge a view is bound
        // to when that view goes away.
        QQmlEngine::setObjectOwnership(m_bridge, QQmlEngine::CppOwnership);
        m_engine->rootContext()->setContextProperty(QStringLiteral("logos"), m_bridge);
    }
}

// TEARDOWN ORDER, BECAUSE QObject'S DEFAULT IS THE WRONG ONE HERE.
//
// ~QObject deletes children in the order they were ADDED, and the constructor
// above adds the node before the bridge — so the default destructor deletes the
// QtRO node first and the bridge's replicas second. A replica is the node's
// client: it holds the node's private and unregisters itself on the way out, so
// destroying the node first is a use-after-free. It does not fail every time
// (the freed pages are usually still readable), which is exactly why it showed
// up as an INTERMITTENT SIGSEGV — around one run in six of this repo's own
// WebRuntimeTests, in whichever test function happened to be last.
//
// The order below is the dependency order, innermost first: a module's view
// holds bindings onto a replica, a replica belongs to the node, and the node
// owes nothing to anyone.
LogosWebRuntime::~LogosWebRuntime()
{
    for (auto it = m_views.begin(); it != m_views.end(); ++it)
        destroyView(it->view, it->context);
    m_views.clear();
    m_order.clear();

    // Both are children of this object, so deleting them here also removes
    // them from the child list — ~QObject will not see them again.
    delete m_bridge;
    m_bridge = nullptr;
    delete m_node;
    m_node = nullptr;
}

bool LogosWebRuntime::connectToBackend(const QString& portName)
{
    if (portName.isEmpty())
        return false;
    if (m_portName == portName)
        return true;

    LogosMessagePortTransport::registerTransport();
    m_portName = portName;
    return m_node->connectToNode(LogosMessagePortTransport::url(portName));
}

LogosWebBridge* LogosWebRuntime::bridge() const
{
    return m_bridge;
}

QRemoteObjectNode* LogosWebRuntime::node() const
{
    return m_node;
}

QObject* LogosWebRuntime::installModuleView(const QString& moduleName, const QString& qml)
{
    // EVERY FAILURE LEAVES THE SAME THREE TRACES — lastError(), the signal and a
    // warning — because a page that gets nullptr back has no other way to tell
    // "this module is broken" from "this module has not arrived yet".
    auto fail = [this, &moduleName](const QString& error) -> QObject* {
        m_lastError = error;
        qCWarning(lcWebRuntime) << "module view" << moduleName << "failed:" << m_lastError;
        emit moduleViewFailed(moduleName, m_lastError);
        return nullptr;
    };

    if (moduleName.isEmpty())
        return fail(QStringLiteral("a module view needs a module name"));
    if (!m_engine)
        return fail(QStringLiteral("the runtime has no QML engine"));

    QQmlComponent component(m_engine);
    component.setData(qml.toUtf8(), documentUrlFor(moduleName));
    if (component.isError())
        return fail(component.errorString().trimmed());

    // A CONTEXT PER MODULE, child of the root. It carries the one thing a view
    // can legitimately want to know about itself — its own name — and it is
    // also the unit this class destroys on removal, so a module's bindings go
    // when the module does. Held by scope until the view exists, so a document
    // that compiles but creates nothing leaves none behind.
    std::unique_ptr<QQmlContext> context(new QQmlContext(m_engine->rootContext()));
    context->setContextProperty(QStringLiteral("logosModuleName"), moduleName);

    QObject* view = component.create(context.get());
    if (!view) {
        const QString error = component.errorString().trimmed();
        return fail(error.isEmpty() ? QStringLiteral("the module's QML created no object")
                                    : error);
    }

    // Replace rather than stack: the page re-fetching a module's document is an
    // upgrade or a reload, never a second copy of the same module on screen.
    removeModuleView(moduleName);

    // Adopted in this order, so that a runtime destroying its children tears
    // each view down before the context it was created in — the same ordering
    // destroyView() spells out.
    view->setParent(this);
    QQmlEngine::setObjectOwnership(view, QQmlEngine::CppOwnership);
    context->setParent(this);

    m_views.insert(moduleName, InstalledView{ view, context.release() });
    m_order.append(moduleName);
    m_lastError.clear();

    emit moduleViewInstalled(moduleName, view);
    emit installedModulesChanged();
    return view;
}

bool LogosWebRuntime::removeModuleView(const QString& moduleName)
{
    auto it = m_views.find(moduleName);
    if (it == m_views.end())
        return false;

    QObject* view = it->view;
    QQmlContext* context = it->context;
    m_views.erase(it);
    m_order.removeAll(moduleName);
    destroyView(view, context);

    emit installedModulesChanged();
    return true;
}

QObject* LogosWebRuntime::moduleView(const QString& moduleName) const
{
    auto it = m_views.constFind(moduleName);
    return it == m_views.cend() ? nullptr : it->view.data();
}

QStringList LogosWebRuntime::installedModules() const
{
    return m_order;
}

QString LogosWebRuntime::lastError() const
{
    return m_lastError;
}
