#include "LogosWebRuntime.h"

#include "LogosMessagePortTransport.h"
#include "LogosWebBridge.h"

#include <QLoggingCategory>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QRemoteObjectNode>
#include <QUrl>

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

LogosWebRuntime::~LogosWebRuntime() = default;

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
    if (moduleName.isEmpty()) {
        m_lastError = QStringLiteral("a module view needs a module name");
        emit moduleViewFailed(moduleName, m_lastError);
        return nullptr;
    }
    if (!m_engine) {
        m_lastError = QStringLiteral("the runtime has no QML engine");
        emit moduleViewFailed(moduleName, m_lastError);
        return nullptr;
    }

    // A CONTEXT PER MODULE, child of the root. It carries the one thing a view
    // can legitimately want to know about itself — its own name — and it is
    // also the unit this class destroys on removal, so a module's bindings go
    // when the module does.
    auto* context = new QQmlContext(m_engine->rootContext(), this);
    context->setContextProperty(QStringLiteral("logosModuleName"), moduleName);

    QQmlComponent component(m_engine);
    component.setData(qml.toUtf8(), documentUrlFor(moduleName));

    if (component.isError()) {
        m_lastError = component.errorString().trimmed();
        qCWarning(lcWebRuntime) << "module view" << moduleName << "failed to compile:"
                                << m_lastError;
        delete context;
        emit moduleViewFailed(moduleName, m_lastError);
        return nullptr;
    }

    QObject* view = component.create(context);
    if (!view) {
        m_lastError = component.errorString().trimmed();
        if (m_lastError.isEmpty())
            m_lastError = QStringLiteral("the module's QML created no object");
        delete context;
        emit moduleViewFailed(moduleName, m_lastError);
        return nullptr;
    }

    // Replace rather than stack: the page re-fetching a module's document is an
    // upgrade or a reload, never a second copy of the same module on screen.
    removeModuleView(moduleName);

    view->setParent(this);
    QQmlEngine::setObjectOwnership(view, QQmlEngine::CppOwnership);

    m_views.insert(moduleName, InstalledView{ view, context });
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
    // In this order, and not by parenting one to the other: a QML object is
    // torn down against the context it was created in, so the context has to
    // outlive it by exactly that much.
    delete view;
    delete context;

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
