#include "LogosMessagePortDevice.h"

#include <QtGlobal>

#include <algorithm>
#include <cstring>

LogosMessagePortDevice::LogosMessagePortDevice(LogosMessagePort* port, QObject* parent)
    : QIODevice(parent)
    , m_port(port)
{
    if (m_port) {
        connect(m_port, &LogosMessagePort::messageReceived,
                this, &LogosMessagePortDevice::onMessage);
        connect(m_port, &LogosMessagePort::closed,
                this, &LogosMessagePortDevice::onPortClosed);
    }

    // Unbuffered: QIODevice's own read buffer would sit in front of the one
    // below and make bytesAvailable() two numbers that have to agree. There is
    // nothing for it to save — the bytes are already in memory.
    QIODevice::open(QIODevice::ReadWrite | QIODevice::Unbuffered);

    // LAST, and only once the signals above are connected: start() releases
    // everything the peer said before this device existed, and a port started
    // before its reader is attached would deliver that backlog to nobody.
    if (m_port)
        m_port->start();
}

LogosMessagePortDevice::~LogosMessagePortDevice() = default;

qint64 LogosMessagePortDevice::bytesAvailable() const
{
    return (m_buffer.size() - m_readPos) + QIODevice::bytesAvailable();
}

void LogosMessagePortDevice::close()
{
    if (!isOpen())
        return;
    QIODevice::close();
}

qint64 LogosMessagePortDevice::readData(char* data, qint64 maxSize)
{
    const qint64 available = m_buffer.size() - m_readPos;
    if (available <= 0)
        return 0;

    const qint64 n = std::min(available, maxSize);
    std::memcpy(data, m_buffer.constData() + m_readPos, static_cast<size_t>(n));
    m_readPos += n;

    // Reclaim only once the buffer is drained. QtRO reads a packet's length and
    // then its body, so compacting per read would memmove the backlog twice per
    // packet for no gain.
    if (m_readPos == m_buffer.size()) {
        m_buffer.clear();
        m_readPos = 0;
    }
    return n;
}

qint64 LogosMessagePortDevice::writeData(const char* data, qint64 len)
{
    if (!m_port || !m_port->isOpen())
        return -1;

    m_port->post(QByteArray(data, static_cast<qsizetype>(len)));
    return len;
}

void LogosMessagePortDevice::onMessage(const QByteArray& message)
{
    if (message.isEmpty())
        return;
    m_buffer.append(message);
    emit readyRead();
}

void LogosMessagePortDevice::onPortClosed()
{
    // Order matters: QtRO's wrappers treat aboutToClose() as "this device is
    // going down" and disconnected() as "the peer is gone". Reporting the peer
    // first and then closing leaves the node one turn in which the device still
    // reads, which is what drains a final packet that crossed with the close.
    emit disconnected();
    close();
}
