#pragma once

#include "LogosMessagePort.h"

#ifdef __EMSCRIPTEN__

#include <emscripten/val.h>

// ── LogosEmscriptenMessagePort ──────────────────────────────────────────────
//
// A LogosMessagePort backed by a real HTML MessagePort, for the half of this
// repo that runs inside a Qt-for-WebAssembly image (ADR 0004).
//
// THE PAGE HANDS THE PORT OVER; nothing here goes looking for one. The loader
// page creates the channel, posts one end into the module's Worker and gives
// the other to the runtime:
//
//     const channel = new MessageChannel();
//     worker.postMessage({ logosPort: channel.port2 }, [channel.port2]);
//     Module.logosAdoptMessagePort('backend', channel.port1);
//
// after which the C++ side is ordinary QtRO:
//
//     LogosMessagePortTransport::registerTransport();
//     node.connectToNode(LogosMessagePortTransport::url("backend"));
//
// `logosAdoptMessagePort` is an embind export of exactly that: it wraps the JS
// object and publishes it under the given name, so a page never touches a Qt
// type and the runtime never touches a JS one.
//
// WHAT A MESSAGEPORT CANNOT TELL YOU is that the peer is gone: there is no
// close event, and a Worker that trapped leaves its port looking perfectly
// healthy. Death is reported the way the loader page already reports it for the
// Wasm host — as a control message, which the page turns into
// close() — and never inferred here.
class LogosEmscriptenMessagePort : public LogosMessagePort
{
    Q_OBJECT
public:
    // Wraps `port`, which must have postMessage()/onmessage and may have
    // start() — a MessagePort, a Worker and the page's own global all qualify.
    // The wrapper does NOT take the JS object's lifetime: JS owns it.
    explicit LogosEmscriptenMessagePort(emscripten::val port, QObject* parent = nullptr);
    ~LogosEmscriptenMessagePort() override;

    void post(const QByteArray& message) override;
    void start() override;
    bool isOpen() const override;
    void close() override;

    // Called from JS through embind. Not for C++ callers.
    static void deliverFromJs(int handle, emscripten::val data);

private:
    emscripten::val m_port;
    int m_handle = 0;
    bool m_open = true;
    bool m_started = false;
};

#endif // __EMSCRIPTEN__
