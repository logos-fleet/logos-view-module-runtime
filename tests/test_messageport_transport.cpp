// QT REMOTE OBJECTS OVER A MESSAGEPORT, PROVEN WITHOUT A BROWSER.
//
// In the Web container the two ends of this wire are two wasm images in one
// page (ADR 0004) and the port between them is an HTML MessagePort. Nothing
// about the TRANSPORT is browser-specific, though: a MessagePort is "ordered,
// reliable, message-oriented, asynchronous delivery, no address", and
// LogosLoopbackMessagePort is the same four properties in one process. So
// every claim this transport makes is checkable here, on the desktop, with a
// real QRemoteObjectHost and a real replica — and what is left for a browser
// is only whether the emscripten port implementation is wired to the right JS
// object, which no unit test in any language could answer anyway.

#include "LogosMessagePort.h"
#include "LogosMessagePortDevice.h"
#include "LogosMessagePortTransport.h"

#include <QtRemoteObjects/qconnectionfactories.h>

#include <QCoreApplication>
#include <QRemoteObjectDynamicReplica>
#include <QRemoteObjectHost>
#include <QRemoteObjectNode>
#include <QSignalSpy>
#include <QTest>

namespace {

// The module backend's shape, reduced to what the counter UI's `web` variant
// actually needs of it: a slot the button calls, a property the view binds to,
// and a signal it listens for.
class CounterSource : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int value READ value NOTIFY valueChanged)
public:
    explicit CounterSource(QObject* parent = nullptr) : QObject(parent) { }

    int value() const { return m_value; }

public Q_SLOTS:
    void increment()
    {
        ++m_value;
        emit valueChanged(m_value);
    }

Q_SIGNALS:
    void valueChanged(int value);
    void pinged(const QString& what);

private:
    int m_value = 0;
};

// A second source on the SAME port: one runtime, two modules' backends, no
// second channel — the transport half of "the same runtime serves a second
// module's QML without a second runtime download".
class GreeterSource : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString greeting READ greeting NOTIFY greetingChanged)
public:
    explicit GreeterSource(QObject* parent = nullptr) : QObject(parent) { }
    QString greeting() const { return m_greeting; }

public Q_SLOTS:
    void greet(const QString& who)
    {
        m_greeting = QStringLiteral("hello, ") + who;
        emit greetingChanged(m_greeting);
    }

Q_SIGNALS:
    void greetingChanged(const QString& greeting);

private:
    QString m_greeting;
};

} // namespace

class TestMessagePortTransport : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { LogosMessagePortTransport::registerTransport(); }

    void init()
    {
        // Each test names its own ports, so a port left published by a failed
        // test cannot be picked up by the next one.
        ++m_run;
    }

    // ── the port itself ─────────────────────────────────────────────────
    void loopbackDeliversToThePeerAndNeverInline();
    void loopbackQueuesUntilStarted();
    void loopbackClosesBothEnds();

    // ── the QIODevice over it ───────────────────────────────────────────
    void deviceStreamsWhatThePeerPosted();
    void deviceReportsThePeerGoingAway();

    // ── QtRO over the whole thing ───────────────────────────────────────
    void schemeIsRegisteredWithBothFactories();
    void replicaBecomesValidOverAMessagePort();
    void aSlotCallDrivesTheBackendAndThePropertyComesBack();
    void aSourceSignalReachesTheReplica();
    void oneMessagePortCarriesTwoModulesBackends();
    void aRuntimeThatStartsBeforeItsBackendReconnects();
    void closingThePortInvalidatesTheReplica();

private:
    QString name(const char* role) const
    {
        return QStringLiteral("t%1-%2").arg(m_run).arg(QLatin1StringView(role));
    }

    int m_run = 0;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestMessagePortTransport::loopbackDeliversToThePeerAndNeverInline()
{
    QObject owner;
    auto pair = LogosLoopbackMessagePort::createPair(&owner);

    pair.second->start();
    QSignalSpy spy(pair.second, &LogosMessagePort::messageReceived);
    pair.first->post(QByteArrayLiteral("ping"));

    // NOT YET. Delivery inside post() would let a QtRO reply re-enter the
    // packet reader that is still parsing the request; the loopback has to be
    // as asynchronous as the browser's port is.
    QCOMPARE(spy.count(), 0);

    QTRY_COMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArrayLiteral("ping"));
}

void TestMessagePortTransport::loopbackQueuesUntilStarted()
{
    QObject owner;
    auto pair = LogosLoopbackMessagePort::createPair(&owner);

    // Said before anyone was listening. An HTML MessagePort holds these until
    // start(); so must this one, or the QtRO object list a backend writes the
    // instant it begins hosting is lost and the runtime's replica waits for an
    // announcement that was already made.
    pair.first->post(QByteArrayLiteral("one"));
    pair.first->post(QByteArrayLiteral("two"));

    QSignalSpy spy(pair.second, &LogosMessagePort::messageReceived);
    QTest::qWait(50);
    QCOMPARE(spy.count(), 0);

    pair.second->start();
    QTRY_COMPARE(spy.count(), 2);
    QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArrayLiteral("one"));
    QCOMPARE(spy.at(1).at(0).toByteArray(), QByteArrayLiteral("two"));

    // And after start it is a plain pipe again, in the same order.
    pair.first->post(QByteArrayLiteral("three"));
    QTRY_COMPARE(spy.count(), 3);
    QCOMPARE(spy.at(2).at(0).toByteArray(), QByteArrayLiteral("three"));
}

void TestMessagePortTransport::loopbackClosesBothEnds()
{
    QObject owner;
    auto pair = LogosLoopbackMessagePort::createPair(&owner);

    QSignalSpy closedHere(pair.first, &LogosMessagePort::closed);
    QSignalSpy closedThere(pair.second, &LogosMessagePort::closed);

    pair.first->close();
    QCOMPARE(closedHere.count(), 1);
    QTRY_COMPARE(closedThere.count(), 1);

    QVERIFY(!pair.first->isOpen());
    QVERIFY(!pair.second->isOpen());

    // Idempotent: a second close is not a second report.
    pair.first->close();
    QCOMPARE(closedHere.count(), 1);
}

void TestMessagePortTransport::deviceStreamsWhatThePeerPosted()
{
    QObject owner;
    auto pair = LogosLoopbackMessagePort::createPair(&owner);
    LogosMessagePortDevice device(pair.first, &owner);

    QVERIFY(device.isOpen());
    QVERIFY(device.isSequential());

    // TWO MESSAGES, ONE STREAM. QtRO frames its own packets, so a reader is
    // entitled to see these as four bytes followed by four more — and must
    // never see a message boundary it did not put there.
    pair.second->post(QByteArrayLiteral("abcd"));
    pair.second->post(QByteArrayLiteral("efgh"));

    QTRY_COMPARE(device.bytesAvailable(), 8);
    QCOMPARE(device.read(3), QByteArrayLiteral("abc"));
    QCOMPARE(device.bytesAvailable(), 5);
    QCOMPARE(device.readAll(), QByteArrayLiteral("defgh"));
    QCOMPARE(device.bytesAvailable(), 0);

    // And the other direction crosses as one message per write. (The device
    // started pair.first; nothing has started the far end, so this test plays
    // the part its reader would.)
    pair.second->start();
    QSignalSpy spy(pair.second, &LogosMessagePort::messageReceived);
    QCOMPARE(device.write(QByteArrayLiteral("out")), 3);
    QTRY_COMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toByteArray(), QByteArrayLiteral("out"));
}

void TestMessagePortTransport::deviceReportsThePeerGoingAway()
{
    QObject owner;
    auto pair = LogosLoopbackMessagePort::createPair(&owner);
    auto* device = new LogosMessagePortDevice(pair.first, &owner);

    QSignalSpy spy(device, &LogosMessagePortDevice::disconnected);
    pair.second->close();

    QTRY_COMPARE(spy.count(), 1);
    QVERIFY(!device->isOpen());
}

void TestMessagePortTransport::schemeIsRegisteredWithBothFactories()
{
    const QUrl u = LogosMessagePortTransport::url(QStringLiteral("anything"));
    QCOMPARE(u.scheme(), QStringLiteral("messageport"));
    QCOMPARE(u.toString(), QStringLiteral("messageport:anything"));

    // BOTH ENDS. A scheme registered only as a client is a runtime that can
    // connect to a backend nobody can host.
    QVERIFY(QtROClientFactory::instance()->isValid(u));
    QVERIFY(QtROServerFactory::instance()->isValid(u));
}

void TestMessagePortTransport::replicaBecomesValidOverAMessagePort()
{
    QObject owner;
    auto pair = LogosLoopbackMessagePort::createPair(&owner);
    LogosMessagePortTransport::publish(name("backend"), pair.first);
    LogosMessagePortTransport::publish(name("runtime"), pair.second);

    CounterSource counter;
    QRemoteObjectHost host;
    QVERIFY(host.setHostUrl(LogosMessagePortTransport::url(name("backend"))));
    QVERIFY(host.enableRemoting(&counter, QStringLiteral("counter")));

    QRemoteObjectNode node;
    QVERIFY(node.connectToNode(LogosMessagePortTransport::url(name("runtime"))));

    QScopedPointer<QRemoteObjectDynamicReplica> replica(
        node.acquireDynamic(QStringLiteral("counter")));
    QVERIFY(replica->waitForSource(5000));
    QVERIFY(replica->isInitialized());
    QCOMPARE(replica->property("value").toInt(), 0);
}

void TestMessagePortTransport::aSlotCallDrivesTheBackendAndThePropertyComesBack()
{
    QObject owner;
    auto pair = LogosLoopbackMessagePort::createPair(&owner);
    LogosMessagePortTransport::publish(name("backend"), pair.first);
    LogosMessagePortTransport::publish(name("runtime"), pair.second);

    CounterSource counter;
    QRemoteObjectHost host;
    QVERIFY(host.setHostUrl(LogosMessagePortTransport::url(name("backend"))));
    QVERIFY(host.enableRemoting(&counter, QStringLiteral("counter")));

    QRemoteObjectNode node;
    QVERIFY(node.connectToNode(LogosMessagePortTransport::url(name("runtime"))));
    QScopedPointer<QRemoteObjectDynamicReplica> replica(
        node.acquireDynamic(QStringLiteral("counter")));
    QVERIFY(replica->waitForSource(5000));

    // THE BUTTON. A slot invoked on the replica has to reach the source...
    QVERIFY(QMetaObject::invokeMethod(replica.data(), "increment"));
    QTRY_COMPARE(counter.value(), 1);

    // ...and the property change has to come back the other way on its own,
    // without the view asking for it.
    QTRY_COMPARE(replica->property("value").toInt(), 1);
}

void TestMessagePortTransport::aSourceSignalReachesTheReplica()
{
    QObject owner;
    auto pair = LogosLoopbackMessagePort::createPair(&owner);
    LogosMessagePortTransport::publish(name("backend"), pair.first);
    LogosMessagePortTransport::publish(name("runtime"), pair.second);

    CounterSource counter;
    QRemoteObjectHost host;
    QVERIFY(host.setHostUrl(LogosMessagePortTransport::url(name("backend"))));
    QVERIFY(host.enableRemoting(&counter, QStringLiteral("counter")));

    QRemoteObjectNode node;
    QVERIFY(node.connectToNode(LogosMessagePortTransport::url(name("runtime"))));
    QScopedPointer<QRemoteObjectDynamicReplica> replica(
        node.acquireDynamic(QStringLiteral("counter")));
    QVERIFY(replica->waitForSource(5000));

    QSignalSpy spy(replica.data(), SIGNAL(pinged(QString)));
    QVERIFY(spy.isValid());
    emit counter.pinged(QStringLiteral("from the backend"));
    QTRY_COMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("from the backend"));
}

void TestMessagePortTransport::oneMessagePortCarriesTwoModulesBackends()
{
    QObject owner;
    auto pair = LogosLoopbackMessagePort::createPair(&owner);
    LogosMessagePortTransport::publish(name("backend"), pair.first);
    LogosMessagePortTransport::publish(name("runtime"), pair.second);

    CounterSource counter;
    GreeterSource greeter;
    QRemoteObjectHost host;
    QVERIFY(host.setHostUrl(LogosMessagePortTransport::url(name("backend"))));
    QVERIFY(host.enableRemoting(&counter, QStringLiteral("counter")));
    QVERIFY(host.enableRemoting(&greeter, QStringLiteral("greeter")));

    QRemoteObjectNode node;
    QVERIFY(node.connectToNode(LogosMessagePortTransport::url(name("runtime"))));

    QScopedPointer<QRemoteObjectDynamicReplica> a(node.acquireDynamic(QStringLiteral("counter")));
    QScopedPointer<QRemoteObjectDynamicReplica> b(node.acquireDynamic(QStringLiteral("greeter")));
    QVERIFY(a->waitForSource(5000));
    QVERIFY(b->waitForSource(5000));

    QVERIFY(QMetaObject::invokeMethod(a.data(), "increment"));
    QVERIFY(QMetaObject::invokeMethod(b.data(), "greet", Q_ARG(QString, QStringLiteral("logos"))));

    QTRY_COMPARE(a->property("value").toInt(), 1);
    QTRY_COMPARE(b->property("greeting").toString(), QStringLiteral("hello, logos"));
}

void TestMessagePortTransport::aRuntimeThatStartsBeforeItsBackendReconnects()
{
    QObject owner;

    // The runtime connects to a port that does not exist yet — the ordinary
    // case in a page, where the QML runtime is up long before the Worker has
    // instantiated its image and handed a port over.
    QRemoteObjectNode node;
    QVERIFY(node.connectToNode(LogosMessagePortTransport::url(name("runtime"))));
    QScopedPointer<QRemoteObjectDynamicReplica> replica(
        node.acquireDynamic(QStringLiteral("counter")));
    QVERIFY(!replica->isInitialized());

    auto pair = LogosLoopbackMessagePort::createPair(&owner);
    LogosMessagePortTransport::publish(name("backend"), pair.first);
    LogosMessagePortTransport::publish(name("runtime"), pair.second);

    CounterSource counter;
    QRemoteObjectHost host;
    QVERIFY(host.setHostUrl(LogosMessagePortTransport::url(name("backend"))));
    QVERIFY(host.enableRemoting(&counter, QStringLiteral("counter")));

    QVERIFY(replica->waitForSource(10000));
    QCOMPARE(replica->property("value").toInt(), 0);
}

void TestMessagePortTransport::closingThePortInvalidatesTheReplica()
{
    QObject owner;
    auto pair = LogosLoopbackMessagePort::createPair(&owner);
    LogosMessagePortTransport::publish(name("backend"), pair.first);
    LogosMessagePortTransport::publish(name("runtime"), pair.second);

    CounterSource counter;
    QRemoteObjectHost host;
    QVERIFY(host.setHostUrl(LogosMessagePortTransport::url(name("backend"))));
    QVERIFY(host.enableRemoting(&counter, QStringLiteral("counter")));

    QRemoteObjectNode node;
    QVERIFY(node.connectToNode(LogosMessagePortTransport::url(name("runtime"))));
    QScopedPointer<QRemoteObjectDynamicReplica> replica(
        node.acquireDynamic(QStringLiteral("counter")));
    QVERIFY(replica->waitForSource(5000));

    // A Worker that trapped takes its port down with it. The view must be told
    // rather than left binding to values that stopped changing.
    pair.first->close();
    QTRY_VERIFY(!replica->isReplicaValid());
}

QTEST_GUILESS_MAIN(TestMessagePortTransport)
#include "test_messageport_transport.moc"
