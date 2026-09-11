#pragma once

#include "LogosMessagePort.h"

#include <QByteArray>
#include <QIODevice>
#include <QPointer>

// ── LogosMessagePortDevice ──────────────────────────────────────────────────
//
// A QIODevice over a LogosMessagePort: the adapter that turns "discrete
// messages" into "a byte stream", which is the only shape QtRO knows how to
// talk.
//
// THE TWO SHAPES ARE NOT THE SAME THING and the conversion is one-way. QtRO
// frames its own packets with a length prefix and reads them off a QDataStream,
// so it does not care where one message ends and the next begins — a read may
// span messages and a write may be split across them. What it DOES require is
// order and completeness, which a MessagePort gives. So this device simply
// appends every arriving message to one read buffer and posts every write as
// one message; nothing here re-frames, and nothing here may reorder.
//
// OPEN FROM BIRTH. A MessagePort is live the moment you hold it — there is no
// connect step to wait for — so the device opens in its constructor and the
// transport above it can call initializeDataStream() straight away.
class LogosMessagePortDevice : public QIODevice
{
    Q_OBJECT
public:
    // The port is BORROWED, not owned: in a browser it belongs to the page, in
    // a test to the fixture. Closing the device does not close the port.
    explicit LogosMessagePortDevice(LogosMessagePort* port, QObject* parent = nullptr);
    ~LogosMessagePortDevice() override;

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override;
    void close() override;

    LogosMessagePort* port() const { return m_port; }

Q_SIGNALS:
    // The peer is gone. Named `disconnected` because that is the signal QtRO's
    // device wrappers look for — QtROExternalIoDevice goes as far as probing
    // the metaobject for `disconnected()` by name.
    void disconnected();

protected:
    qint64 readData(char* data, qint64 maxSize) override;
    qint64 writeData(const char* data, qint64 len) override;

private:
    void onMessage(const QByteArray& message);
    void onPortClosed();

    QPointer<LogosMessagePort> m_port;

    // One buffer with a read cursor rather than remove-from-front: QtRO reads a
    // packet's length and then its body, so every packet would otherwise
    // memmove the rest of the buffer twice.
    QByteArray m_buffer;
    qsizetype m_readPos = 0;
};
