// The Web container's QML runtime, reduced to what this build has to prove:
// that Qt Quick, the Logos design system and the MessagePort transport link
// into ONE static wasm image, that the design system's QML types survive that
// link, and that the emscripten port's JS glue survives it too.
//
// Nothing here runs during the build — a Qt-wasm image needs a canvas and a
// page to hand it a port — so what the derivation around this checks is the
// IMAGE: that the link produced one, and that both halves of the page-facing
// API (`logosAdoptMessagePort` in, `logosMessagePortDeliver` out) are in it.
// The transport's behaviour is checked on the desktop, against a real
// QRemoteObjectHost, in tests/test_messageport_transport.cpp.
#include "LogosMessagePortTransport.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QRemoteObjectNode>

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);

    LogosMessagePortTransport::registerTransport();

    // The name the loader page adopts the backend's port under. One node, and
    // it stays one however many modules' sources arrive over it.
    QRemoteObjectNode node;
    node.connectToNode(LogosMessagePortTransport::url(QStringLiteral("backend")));

    QQmlApplicationEngine engine;
    engine.loadFromModule("LogosMessagePortSmoke", "Main");
    if (engine.rootObjects().isEmpty())
        return 1;
    return app.exec();
}
