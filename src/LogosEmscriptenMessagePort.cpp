#include "LogosEmscriptenMessagePort.h"

#ifdef __EMSCRIPTEN__

#include "LogosMessagePortTransport.h"

#include <QHash>
#include <QPointer>

#include <emscripten/bind.h>
#include <emscripten/em_js.h>
#include <emscripten/val.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

// AN INTEGER HANDLE, NOT A POINTER. JS holds this across the call boundary, and
// a page that keeps a stale one must get "no such port" rather than a write
// through a freed C++ object.
QHash<int, QPointer<LogosEmscriptenMessagePort>>& ports()
{
    static QHash<int, QPointer<LogosEmscriptenMessagePort>> instances;
    return instances;
}

int nextHandle()
{
    static int handle = 0;
    return ++handle;
}

} // namespace

// Installs the JS side of the channel. EM_JS rather than a val-built closure
// because embind cannot make a JS function out of a C++ lambda, and the body is
// small enough to read: everything it does is name the two ends of the hop.
//
// Emscripten compiles this body into the image's JS library, so a syntax error
// here fails the LINK rather than the first message.
EM_JS(void, logos_messageport_attach, (int handle, emscripten::EM_VAL portHandle), {
    var port = Emval.toValue(portHandle);
    port.onmessage = function (event) {
        var data = event.data;
        Module["logosMessagePortDeliver"](handle,
            data instanceof Uint8Array ? data : new Uint8Array(data));
    };
    // A port obtained from a MessageChannel has its queue disabled until this
    // is called; one obtained from an onmessage event handler does not have the
    // method at all.
    if (typeof port.start === "function")
        port.start();
});

LogosEmscriptenMessagePort::LogosEmscriptenMessagePort(emscripten::val port, QObject* parent)
    : LogosMessagePort(parent)
    , m_port(port)
    , m_handle(nextHandle())
{
    ports().insert(m_handle, this);
}

LogosEmscriptenMessagePort::~LogosEmscriptenMessagePort()
{
    ports().remove(m_handle);
}

void LogosEmscriptenMessagePort::start()
{
    if (m_started || !m_open)
        return;
    m_started = true;
    logos_messageport_attach(m_handle, m_port.as_handle());
}

void LogosEmscriptenMessagePort::post(const QByteArray& message)
{
    if (!m_open)
        return;

    // COPIED, DELIBERATELY. typed_memory_view is a window onto wasm linear
    // memory; handing it to postMessage would transfer a view that a later
    // memory growth can detach. `new Uint8Array(view)` takes the bytes out.
    const emscripten::val view = emscripten::val(emscripten::typed_memory_view(
        static_cast<size_t>(message.size()),
        reinterpret_cast<const uint8_t*>(message.constData())));
    const emscripten::val bytes = emscripten::val::global("Uint8Array").new_(view);
    m_port.call<void>("postMessage", bytes);
}

bool LogosEmscriptenMessagePort::isOpen() const
{
    return m_open;
}

void LogosEmscriptenMessagePort::close()
{
    if (!m_open)
        return;
    m_open = false;
    if (m_port.hasOwnProperty("close") || !m_port["close"].isUndefined())
        m_port.call<void>("close");
    emit closed();
}

void LogosEmscriptenMessagePort::deliverFromJs(int handle, emscripten::val data)
{
    LogosEmscriptenMessagePort* port = ports().value(handle);
    if (!port || !port->m_open)
        return;

    const std::vector<uint8_t> bytes = emscripten::convertJSArrayToNumberVector<uint8_t>(data);
    if (bytes.empty())
        return;
    emit port->messageReceived(
        QByteArray(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<qsizetype>(bytes.size())));
}

namespace {

// THE PAGE'S WHOLE API. One call, one name, no Qt types: the loader page says
// which port is which, and everything above it is QtRO.
//
// The port is owned by the wrapper and leaked on purpose — it lives as long as
// the image does, and there is no page-visible handle to free it with. A second
// adoption under the same name replaces the published entry (see
// LogosMessagePortTransport::publish), which is what a page that respawned its
// Worker needs.
void logosAdoptMessagePort(std::string name, emscripten::val port)
{
    auto* adopted = new LogosEmscriptenMessagePort(port);
    LogosMessagePortTransport::publish(QString::fromStdString(name), adopted);
}

} // namespace

EMSCRIPTEN_BINDINGS(logos_messageport)
{
    emscripten::function("logosAdoptMessagePort", &logosAdoptMessagePort);
    emscripten::function("logosMessagePortDeliver", &LogosEmscriptenMessagePort::deliverFromJs);
}

#endif // __EMSCRIPTEN__
