// THE WEB CONTAINER'S QML RUNTIME — the application.
//
// One Qt-for-WebAssembly image, shipped and signed with the app (ADR 0004),
// into which every Downloaded module's QML is loaded at INSTALL TIME. The page
// hands it two things and never a Qt type:
//
//     const channel = new MessageChannel();
//     worker.postMessage({ logosPort: channel.port2 }, [channel.port2]);
//     Module.logosAdoptMessagePort('backend', channel.port1);   // the wire
//     Module.logosInstallModuleView('counter', await (await fetch(url)).text());
//
// The second call is the whole of "install": a module's QML arrives as TEXT
// because `file://` is dead in both platforms' webviews and Qt's network layer
// refuses the custom schemes that replace it, so the page fetches the document
// and this compiles it. A second module costs a document — there is no second
// runtime to download, which is the slice's fourth criterion.
//
// NOTHING HERE RUNS DURING THE BUILD: a Qt-wasm image needs a canvas, a page
// and a peer. What the derivation around this checks is the IMAGE — that the
// link produced one, that the design system's QML types are still in it, and
// that every page-facing embind export survived (nothing in C++ references
// them, so a linker is free to drop them). The runtime's BEHAVIOUR is checked
// on the desktop, against a real QRemoteObjectHost, in
// tests/test_web_runtime.cpp.
#include "LogosWebRuntime.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#ifdef __EMSCRIPTEN__
#include <emscripten/bind.h>
#include <string>
#endif

namespace {

// The one runtime of this image. A raw pointer with no owner because the page's
// embind entry points are free functions and the image lives as long as the
// page does; there is nothing to free it with and nothing that would want to.
LogosWebRuntime* g_runtime = nullptr;

// The name the loader page adopts the backend's port under. One port, and it
// stays one however many modules' sources arrive over it.
const char* kBackendPort = "backend";

} // namespace

#ifdef __EMSCRIPTEN__

namespace {

bool logosInstallModuleView(std::string name, std::string qml)
{
    if (!g_runtime)
        return false;
    return g_runtime->installModuleView(QString::fromStdString(name),
                                        QString::fromStdString(qml)) != nullptr;
}

bool logosRemoveModuleView(std::string name)
{
    return g_runtime && g_runtime->removeModuleView(QString::fromStdString(name));
}

// For a page that publishes its port under a name other than `backend` — a
// second Worker, or a host that names its channels per module. The default is
// already connected by the time this image answers anything.
bool logosConnectBackend(std::string portName)
{
    return g_runtime && g_runtime->connectToBackend(QString::fromStdString(portName));
}

// Why the last install failed, for a page that wants to say so rather than show
// a blank rectangle.
std::string logosRuntimeLastError()
{
    return g_runtime ? g_runtime->lastError().toStdString() : std::string("no runtime");
}

} // namespace

EMSCRIPTEN_BINDINGS(logos_qml_runtime)
{
    emscripten::function("logosInstallModuleView", &logosInstallModuleView);
    emscripten::function("logosRemoveModuleView", &logosRemoveModuleView);
    emscripten::function("logosConnectBackend", &logosConnectBackend);
    emscripten::function("logosRuntimeLastError", &logosRuntimeLastError);
}

#endif // __EMSCRIPTEN__

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);

    QQmlApplicationEngine engine;

    // Built before the scene, so `logos` is in the root context by the time any
    // document — the runtime's own shell included — is compiled.
    g_runtime = new LogosWebRuntime(&engine);
    engine.rootContext()->setContextProperty(QStringLiteral("runtime"), g_runtime);

    // CONNECTED BEFORE THE PAGE HAS HANDED ANYTHING OVER, and that is the
    // ordinary order: this image is up long before the Worker has instantiated
    // its own. Connecting to a name nothing has published yet is not an error —
    // the node's reconnect timer takes the port the moment it appears.
    g_runtime->connectToBackend(QLatin1StringView(kBackendPort));

    engine.loadFromModule("LogosQmlRuntime", "Main");
    if (engine.rootObjects().isEmpty())
        return 1;
    return app.exec();
}
