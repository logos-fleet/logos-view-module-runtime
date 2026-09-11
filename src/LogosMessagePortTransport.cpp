#include "LogosMessagePortTransport.h"

#include "LogosMessagePortDevice.h"

#include <QtRemoteObjects/qconnectionfactories.h>

#include <QHash>
#include <QList>
#include <QMetaObject>
#include <QPointer>

namespace {

// THE PUBLISHED PORTS OF THIS PROCESS. Not a rendezvous directory (see the
// header): a name is a label the holder of a port chose for it, and the only
// thing that can look it up is code running in the same address space that put
// it there.
class PortRegistry : public QObject
{
    Q_OBJECT
public:
    static PortRegistry* instance()
    {
        static PortRegistry registry;
        return &registry;
    }

    void publish(const QString& name, LogosMessagePort* port)
    {
        if (name.isEmpty() || !port)
            return;
        m_ports.insert(name, port);
        emit published(name);
    }

    void withdraw(const QString& name) { m_ports.remove(name); }

    LogosMessagePort* peek(const QString& name) const { return m_ports.value(name); }

    // TAKEN, NOT BORROWED. A port is one channel with two ends, so handing the
    // same one to a second node would give two QtRO connections the same wire
    // and interleave their packets. Removing it on the way out makes that
    // impossible rather than merely discouraged.
    LogosMessagePort* take(const QString& name)
    {
        auto it = m_ports.find(name);
        if (it == m_ports.end())
            return nullptr;
        LogosMessagePort* port = *it;
        m_ports.erase(it);
        return port;
    }

Q_SIGNALS:
    void published(const QString& name);

private:
    PortRegistry() = default;
    QHash<QString, QPointer<LogosMessagePort>> m_ports;
};

QString portNameOf(const QUrl& url)
{
    // `messageport:runtime` parses as an opaque URL: no authority, path
    // "runtime". `messageport:///runtime` — which a QUrl round trip can
    // produce — leaves the name in the path with a leading slash, so both
    // spellings resolve to the same port.
    QString name = url.path();
    while (name.startsWith(QLatin1Char('/')))
        name.remove(0, 1);
    return name;
}

// ── the client end ──────────────────────────────────────────────────────────
class MessagePortClientIo : public QtROClientIoDevice
{
    Q_OBJECT
public:
    explicit MessagePortClientIo(QObject* parent = nullptr) : QtROClientIoDevice(parent) { }

    // CLOSE HERE, NOT IN THE BASE. ~QtROClientIoDevice() calls close() for a
    // device that is not already closing, and close() dispatches to the pure
    // virtual doClose() — which by then has no override left, because this
    // destructor has already run. Qt's own LocalClientIo does exactly this for
    // exactly this reason; leaving it out aborts the process with "Pure virtual
    // function called" whenever a node is destroyed with a live connection.
    ~MessagePortClientIo() override
    {
        if (!isClosing())
            close();
    }

    QIODevice* connection() const override { return m_device; }

    // THE NODE'S RETRY ENTERS HERE, over and over, for the whole life of a
    // connection that keeps failing — so anything this early-returns on has to
    // be a state a retry can still get out of. A live device is; a dead one is
    // not, which is why it is dropped rather than kept.
    //
    // A MessagePort has no reconnect of its own: a port whose peer is gone is
    // gone for good, and what the page does instead is respawn the Worker and
    // hand over a FRESH channel under the same names (see
    // LogosMessagePortTransport::publish). Taking that new port means letting
    // go of the old device first.
    void connectToServer() override
    {
        if (m_device && m_device->isOpen())
            return;
        discardDevice();

        LogosMessagePort* port = PortRegistry::instance()->take(portNameOf(url()));
        if (!port) {
            // NOT AN ERROR. A QML runtime routinely comes up before the Wasm
            // host that will hand it a port; shouldReconnect puts the node's
            // retry timer on the case, which is the same answer a TCP client
            // gives a server that is not listening yet.
            emit shouldReconnect(this);
            return;
        }

        m_device = new LogosMessagePortDevice(port, this);
        connect(m_device, &QIODevice::readyRead, this, &QtROIoDeviceBase::readyRead);
        connect(m_device, &LogosMessagePortDevice::disconnected, this, [this]() {
            // Dropped HERE as well as in connectToServer(): between the peer
            // dying and the node's next retry, connection() must not hand QtRO
            // a device reading from a closed port.
            discardDevice();
            emit shouldReconnect(this);
        });
        initializeDataStream();
    }

    bool isOpen() const override { return !isClosing() && m_device && m_device->isOpen(); }

protected:
    void doClose() override
    {
        if (m_device)
            m_device->close();
        deleteLater();
    }

    void doDisconnectFromServer() override
    {
        if (m_device)
            m_device->close();
    }

private:
    // deleteLater, not delete: this runs from inside the device's own
    // disconnected() signal.
    void discardDevice()
    {
        if (!m_device)
            return;
        m_device->disconnect(this);
        m_device->deleteLater();
        m_device = nullptr;
    }

    LogosMessagePortDevice* m_device = nullptr;
};

// ── the host end ────────────────────────────────────────────────────────────
class MessagePortServerIo : public QtROServerIoDevice
{
    Q_OBJECT
public:
    explicit MessagePortServerIo(LogosMessagePortDevice* device, QObject* parent = nullptr)
        : QtROServerIoDevice(parent)
        , m_device(device)
    {
        m_device->setParent(this);
        connect(m_device, &QIODevice::readyRead, this, &QtROIoDeviceBase::readyRead);
        connect(m_device, &LogosMessagePortDevice::disconnected,
                this, &QtROIoDeviceBase::disconnected);
    }

    QIODevice* connection() const override { return m_device; }

protected:
    void doClose() override { m_device->close(); }

private:
    LogosMessagePortDevice* m_device = nullptr;
};

class MessagePortServer : public QConnectionAbstractServer
{
    Q_OBJECT
public:
    explicit MessagePortServer(QObject* parent) : QConnectionAbstractServer(parent)
    {
        connect(PortRegistry::instance(), &PortRegistry::published,
                this, &MessagePortServer::onPublished);
    }

    bool hasPendingConnections() const override { return !m_pending.isEmpty(); }

    QtROServerIoDevice* configureNewConnection() override
    {
        if (m_pending.isEmpty())
            return nullptr;
        LogosMessagePort* port = m_pending.takeFirst();
        return new MessagePortServerIo(new LogosMessagePortDevice(port), this);
    }

    QUrl address() const override { return LogosMessagePortTransport::url(m_name); }

    bool listen(const QUrl& address) override
    {
        m_name = portNameOf(address);
        if (m_name.isEmpty())
            return false;
        m_listening = true;
        // A backend that published its port before it built its host is the
        // normal order, not a race to lose.
        onPublished(m_name);
        return true;
    }

    QAbstractSocket::SocketError serverError() const override
    {
        return QAbstractSocket::UnknownSocketError;
    }

    void close() override
    {
        m_listening = false;
        m_pending.clear();
    }

private:
    void onPublished(const QString& name)
    {
        if (!m_listening || name != m_name)
            return;
        LogosMessagePort* port = PortRegistry::instance()->take(name);
        if (!port)
            return;
        m_pending.append(port);

        // QUEUED, AND THAT IS LOAD-BEARING. QRemoteObjectSourceIo calls
        // listen() and only then connects to newConnection(), so a server that
        // announced a waiting port synchronously would announce it to nobody
        // and the host would never serve.
        QMetaObject::invokeMethod(this, [this]() { emit newConnection(); },
                                  Qt::QueuedConnection);
    }

    QString m_name;
    bool m_listening = false;
    QList<LogosMessagePort*> m_pending;
};

} // namespace

namespace LogosMessagePortTransport {

QString scheme()
{
    return QStringLiteral("messageport");
}

QUrl url(const QString& portName)
{
    QUrl u;
    u.setScheme(scheme());
    u.setPath(portName);
    return u;
}

void registerTransport()
{
    static bool registered = false;
    if (registered)
        return;
    registered = true;
    QtROClientFactory::instance()->registerType<MessagePortClientIo>(scheme());
    QtROServerFactory::instance()->registerType<MessagePortServer>(scheme());
}

void publish(const QString& portName, LogosMessagePort* port)
{
    PortRegistry::instance()->publish(portName, port);
}

void withdraw(const QString& portName)
{
    PortRegistry::instance()->withdraw(portName);
}

LogosMessagePort* published(const QString& portName)
{
    return PortRegistry::instance()->peek(portName);
}

} // namespace LogosMessagePortTransport

#include "LogosMessagePortTransport.moc"
