#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class LogosWebBridge;
class QQmlContext;
class QQmlEngine;
class QRemoteObjectNode;

// ── LogosWebRuntime ─────────────────────────────────────────────────────────
//
// THE BUNDLED QML RUNTIME of ADR 0004, as a C++ object: one Qt-for-WebAssembly
// image, shipped and signed with the app, into which every Downloaded module's
// QML is loaded AT INSTALL TIME. Not one runtime per module — that is the whole
// point, and the reason this class holds a list rather than a document:
//
//     ~21 MB of runtime, once           a module's QML, per module
//     ┌──────────────────────────┐      ┌─────────┐ ┌─────────┐
//     │ Qt Quick + design system │  ◄── │ counter │ │ wallet  │  … text, fetched
//     │ + MessagePort QtRO       │      └─────────┘ └─────────┘
//     └──────────────────────────┘
//
// A module's QML arrives as TEXT. `file://` is dead in both platforms' webviews
// and Qt's network layer refuses the custom schemes that replace it (ADR 0004),
// so the page fetches the document itself and hands the string over; this
// compiles it against the one engine, with a qrc base URL, and hands back the
// object. That is the whole of "install".
//
// WHAT THIS OWNS is the wire and the engine's view of it: it registers the
// messageport scheme, connects one node to the port the page published, and
// puts one LogosWebBridge in the engine's root context as `logos`. A module's
// QML therefore says exactly what it says on the desktop, and nothing in it
// knows it is in a browser.
//
// WHAT IT DOES NOT OWN is the scene. installModuleView() creates the object and
// announces it; where a view goes on screen is the runtime app's Main.qml's
// business, which is why this class needs Qt Qml and not Qt Quick — and why it
// is testable on the desktop without a window.
class LogosWebRuntime : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList installedModules READ installedModules NOTIFY installedModulesChanged)
public:
    // `engine` must outlive the runtime. The bridge is put in its root context
    // as `logos` here, so a module view installed later needs no wiring.
    explicit LogosWebRuntime(QQmlEngine* engine, QObject* parent = nullptr);
    ~LogosWebRuntime() override;

    // Register the `messageport:` scheme and connect this runtime's node to the
    // port the page published under `portName` (`Module.logosAdoptMessagePort`).
    // Connecting to a name nothing has published yet is NOT an error: the node
    // retries until the Worker comes up. Idempotent; a second call with the same
    // name does nothing.
    Q_INVOKABLE bool connectToBackend(const QString& portName);

    LogosWebBridge* bridge() const;
    QRemoteObjectNode* node() const;

    // Compile a module's QML and create it. Returns the view, or nullptr with
    // moduleViewFailed() emitted and lastError() set — a module whose QML does
    // not compile must be REPORTED, since the alternative is a blank rectangle
    // that looks exactly like a module that has not loaded yet.
    //
    // Installing a module that is already installed REPLACES its view (the
    // page re-fetched an upgraded document), and the old one is destroyed.
    Q_INVOKABLE QObject* installModuleView(const QString& moduleName, const QString& qml);

    // Drop a module's view and its context. The replica it reached its backend
    // through is the bridge's and is left alone: the backend is still there and
    // the module may be installed again.
    Q_INVOKABLE bool removeModuleView(const QString& moduleName);

    Q_INVOKABLE QObject* moduleView(const QString& moduleName) const;

    QStringList installedModules() const;

    // The last compile error, for a page that wants to print it. Cleared by a
    // successful install.
    Q_INVOKABLE QString lastError() const;

Q_SIGNALS:
    void moduleViewInstalled(const QString& moduleName, QObject* view);
    void moduleViewFailed(const QString& moduleName, const QString& error);
    void installedModulesChanged();

private:
    struct InstalledView {
        QPointer<QObject> view;
        QQmlContext* context = nullptr;
    };

    QPointer<QQmlEngine> m_engine;
    QRemoteObjectNode* m_node = nullptr;
    LogosWebBridge* m_bridge = nullptr;
    QString m_portName;
    QHash<QString, InstalledView> m_views;
    QStringList m_order;
    QString m_lastError;
};
