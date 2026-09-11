#include "LogosMessagePort.h"

#include <QMetaObject>

#include <utility>

LogosLoopbackMessagePort::LogosLoopbackMessagePort(QObject* parent)
    : LogosMessagePort(parent)
{
}

QPair<LogosLoopbackMessagePort*, LogosLoopbackMessagePort*>
LogosLoopbackMessagePort::createPair(QObject* parent)
{
    auto* a = new LogosLoopbackMessagePort(parent);
    auto* b = new LogosLoopbackMessagePort(parent);
    a->m_peer = b;
    b->m_peer = a;
    return { a, b };
}

LogosMessagePort* LogosLoopbackMessagePort::peer() const
{
    return m_peer;
}

bool LogosLoopbackMessagePort::isOpen() const
{
    return m_open && !m_peer.isNull() && m_peer->m_open;
}

void LogosLoopbackMessagePort::post(const QByteArray& message)
{
    if (!isOpen())
        return;

    // THROUGH THE EVENT LOOP, ALWAYS. A real MessagePort cannot deliver inside
    // postMessage(), and a loopback that did would let a QtRO reply re-enter
    // the packet reader that is still parsing the request that caused it.
    // Capturing the peer by QPointer keeps a message posted to a port that is
    // deleted before the loop runs from reaching freed memory.
    QPointer<LogosLoopbackMessagePort> peer = m_peer;
    QMetaObject::invokeMethod(
        this, [peer, message]() {
            if (peer)
                peer->deliver(message);
        },
        Qt::QueuedConnection);
}

void LogosLoopbackMessagePort::deliver(const QByteArray& message)
{
    if (!m_open)
        return;
    if (!m_started) {
        m_queued.append(message);
        return;
    }
    emit messageReceived(message);
}

void LogosLoopbackMessagePort::start()
{
    if (m_started)
        return;
    m_started = true;

    // The backlog goes out through the event loop too. Emitting it inline would
    // hand a whole conversation to a reader that was built one statement ago
    // and has not been connected to anything yet.
    const QList<QByteArray> queued = std::move(m_queued);
    m_queued.clear();
    if (queued.isEmpty())
        return;
    QPointer<LogosLoopbackMessagePort> self = this;
    QMetaObject::invokeMethod(
        this, [self, queued]() {
            if (!self || !self->m_open)
                return;
            for (const QByteArray& message : queued)
                emit self->messageReceived(message);
        },
        Qt::QueuedConnection);
}

void LogosLoopbackMessagePort::close()
{
    if (!m_open)
        return;
    m_open = false;
    emit closed();

    // The peer learns about it the way a browser tells it: asynchronously, and
    // as its own close. Queued rather than direct so a close() called from
    // inside a slot does not unwind the peer's stack underneath it.
    QPointer<LogosLoopbackMessagePort> peer = m_peer;
    QMetaObject::invokeMethod(
        this, [peer]() {
            if (peer)
                peer->close();
        },
        Qt::QueuedConnection);
}
