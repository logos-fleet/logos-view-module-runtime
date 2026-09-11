#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>

// ── LogosMessagePort ─────────────────────────────────────────────────────────
//
// A MESSAGE PORT: duplex, ordered, reliable, message-oriented — and with no
// address of its own.
//
// That last property is the whole reason this type exists rather than a
// QIODevice or a socket. In the Web container (ADR 0004) the QML runtime and a
// module's Wasm host are two wasm images in one page, and the only channel a
// browser offers between them is an HTML MessagePort: an object one side is
// HANDED, never one it looks up. There is no directory to connect to, no path
// to open, and nothing to authenticate against — whoever holds the port is the
// peer, which is exactly the structural identity ADR 0005 asks a transport for.
//
// The interface is deliberately four calls wide. Everything above it —
// framing, the QtRO wire, reconnection — is LogosMessagePortDevice's and
// LogosMessagePortTransport's business, and everything below it is the
// platform's: emscripten's MessagePort in a browser, a loopback pair in a
// test or in a host that runs both ends in one process.
//
// DELIVERY IS ALWAYS ASYNCHRONOUS. messageReceived() must never be emitted
// from inside post(), because a real MessagePort delivers through the event
// loop and QtRO's packet reader is not re-entrant. Every implementation here
// honours that, and a new one that does not will deadlock the first time a
// replica answers a call inside a slot.
class LogosMessagePort : public QObject
{
    Q_OBJECT
public:
    explicit LogosMessagePort(QObject* parent = nullptr) : QObject(parent) { }
    ~LogosMessagePort() override = default;

    // Send one message to the peer. A closed port drops it silently: the peer
    // going away is normal (the page navigated, the Worker died) and is
    // reported once, through closed(), rather than once per write.
    virtual void post(const QByteArray& message) = 0;

    // DELIVERY IS OFF UNTIL START, which is not a nicety: an HTML MessagePort's
    // message queue is disabled until `start()` (or assigning `onmessage`), and
    // everything that arrives before that is QUEUED, not dropped. The Web
    // container depends on it — a module's backend finishes enabling remoting
    // and writes QtRO's object list the moment its host listens, which is
    // routinely before the runtime on the other end has attached anything to
    // its port. A port that dropped those bytes would leave a replica waiting
    // forever for an announcement that was already made.
    //
    // LogosMessagePortDevice calls this, so a caller that uses the transport
    // never has to.
    virtual void start() = 0;

    virtual bool isOpen() const = 0;

    // Idempotent. Emits closed() exactly once, on the first call.
    virtual void close() = 0;

Q_SIGNALS:
    void messageReceived(const QByteArray& message);
    void closed();
};

// ── LogosLoopbackMessagePort ────────────────────────────────────────────────
//
// Two ports wired to each other inside one process, which is what
// `new MessageChannel()` gives a page and what a test needs to drive the QtRO
// transport without a browser anywhere near it.
//
// Not test-only: a desktop host that runs a module's backend in its own
// process can pair the two ends this way and get the same wire the browser
// gets, which is how the transport is exercised on a platform that has no
// MessagePort at all.
class LogosLoopbackMessagePort : public LogosMessagePort
{
    Q_OBJECT
public:
    // Both ports are parented to `parent` (or left ownerless if null), so a
    // caller frees them the way it frees anything else. They are NOT parented
    // to each other: either end may outlive the other, and a port whose peer
    // is gone reports itself closed rather than crashing.
    static QPair<LogosLoopbackMessagePort*, LogosLoopbackMessagePort*>
    createPair(QObject* parent = nullptr);

    void post(const QByteArray& message) override;
    void start() override;
    bool isOpen() const override;
    void close() override;

    LogosMessagePort* peer() const;

private:
    explicit LogosLoopbackMessagePort(QObject* parent);

    void deliver(const QByteArray& message);

    QPointer<LogosLoopbackMessagePort> m_peer;
    bool m_open = true;
    bool m_started = false;
    QList<QByteArray> m_queued;
};
