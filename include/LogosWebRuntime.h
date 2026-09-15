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
//     ~26 MB of runtime, once           a module's QML, per module
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

    // WHERE ONE ITEM OF A MODULE'S VIEW IS, AND WHAT IT HOLDS -- as JSON, for a
    // page that has to drive the scene it cannot see.
    //
    // A `web` variant's UI is pixels in a canvas: a host that wants to press a
    // module's field or read what it now says has no DOM node to touch and no
    // text node to read (logos-workspace#174). Qt's own accessibility tree
    // answers half of it -- it carries rects, and names for buttons -- but it
    // publishes a text editor with no name at all, so the one control a typed
    // flow is about is the one thing that cannot be asked for by name.
    //
    // `handle` is the item's `objectName`, which is the handle every Logos view
    // already carries for UI automation (the desktop inspector finds items the
    // same way, and nothing on the QML side has to know about this). Answers:
    //
    //     {"found":true,"x":24,"y":271,"width":444,"height":46,"text":"main"}
    //     {"found":false}
    //
    // x and y are the item's top-left in the WINDOW's coordinates -- summed up
    // the parent chain, so a scrolled Flickable's offset is already in them,
    // which is what a press dispatched at the canvas needs. `text` is the
    // item's `text` property when it has one, which for a field is what the
    // keys put in it.
    //
    // READ-ONLY, deliberately. A page may ask where a control is and what it
    // says; it may not set anything -- the far side of this is a document, and
    // driving a view has to go through the events a finger and a keyboard would
    // send, or it proves nothing about the module (ADR 0005).
    Q_INVOKABLE QString describeItem(const QString& moduleName, const QString& handle) const;

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
