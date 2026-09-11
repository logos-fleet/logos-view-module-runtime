#pragma once

#include <QObject>
#include <QSet>
#include <QString>

#include <functional>

// ── LogosWebCallRouter ──────────────────────────────────────────────────────
//
// THE BACKEND MODULE CALL, MADE REMOTABLE. It is the one thing `logos.module()`
// cannot do for the Web container's QML runtime.
//
// A view reaches its own backend through a typed replica — that is
// LogosWebBridge::module(), and it needs nothing but QtRO. But QML also calls
// OTHER modules by name (`logos.callModuleAsync("keystore", "unlock", …)`), and
// those are native modules living on the far side of the core: the runtime is a
// wasm image in a page with no LogosAPI in it, no token store and no transport
// to the host beyond the port it was handed (ADR 0004).
//
// So the call is remoted like everything else. This object is hosted by the
// WASM HOST — the image that does have a protocol client — under one well-known
// name, and the runtime acquires a replica of it:
//
//     the page's QML runtime                the module's Worker
//     logos.callModuleAsync(…)  ── QtRO ──> LogosWebCallRouter::call()
//                                                 │ handler
//                                                 ▼
//                                           logos-protocol over the
//                                           webview bridge, to the core
//
// TEXT IN, TEXT OUT, and deliberately: the desktop bridge already serialises a
// backend result to a JSON string so QML always sees a string, and a request id
// rather than a return value is what lets one router carry any number of calls
// in flight over one connection without QtRO having to model them.
//
// The router does not know what a module is, does not hold a token and cannot
// answer a call itself. `setHandler` is the seam where the host that DOES puts
// itself, and a router with no handler answers every call with an error rather
// than leaving the view waiting for its own timeout.
class LogosWebCallRouter : public QObject
{
    Q_OBJECT
public:
    explicit LogosWebCallRouter(QObject* parent = nullptr);
    ~LogosWebCallRouter() override;

    // The QtRO name both ends agree on: `host.enableRemoting(router,
    // LogosWebCallRouter::sourceName())` on one side, and the bridge acquires
    // it by the same name on the other. A function rather than a constant so
    // there is exactly one spelling of it in the system — and NOT QObject's own
    // objectName(), which is a different thing that QtRO also reads.
    static QString sourceName();

    // What the host does with a call. Invoked on the host's thread, once per
    // call; the host answers by calling complete() with the same requestId,
    // whenever it has an answer. Not required to be synchronous — the whole
    // point is that it is not.
    using Handler = std::function<void(const QString& requestId,
                                       const QString& module,
                                       const QString& method,
                                       const QString& argsJson)>;
    void setHandler(Handler handler);

    // Answer a call. A requestId that was already answered, or was never asked,
    // is ignored: a view that stopped waiting must not be delivered to twice.
    void complete(const QString& requestId, const QString& payloadJson);

public Q_SLOTS:
    // Remoted. `argsJson` is a JSON array, `payloadJson` in the reply is
    // whatever the module returned, serialised — both are opaque here.
    void call(const QString& requestId,
              const QString& module,
              const QString& method,
              const QString& argsJson);

Q_SIGNALS:
    // Remoted. Carries the requestId back so the bridge can match it to the
    // callback the view handed in.
    void callCompleted(const QString& requestId, const QString& payloadJson);

private:
    Handler m_handler;
    // Ids asked but not yet answered. Only so that a second complete() for one
    // call is a no-op by construction rather than by a guard in every host.
    QSet<QString> m_inFlight;
};
