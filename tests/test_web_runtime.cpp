// THE WEB CONTAINER'S QML RUNTIME, PROVEN WITHOUT A BROWSER.
//
// Everything ADR 0004's bundled runtime does except paint: one QML engine, one
// QtRO node over a MessagePort, `logos` in the engine's root context, and a
// module's QML compiled into it as TEXT at install time. None of that is
// browser-specific — LogosLoopbackMessagePort has a real MessagePort's four
// properties, and a QQmlEngine compiles the same document whether a canvas
// exists or not — so the claims are checkable here, against a real
// QRemoteObjectHost and a real replica.
//
// What is left for a browser is the scene: whether the view RENDERS, at what
// size, with which font. That is criterion 2 of the slice and no unit test in
// any language can answer it.

#include "LogosMessagePort.h"
#include "LogosMessagePortTransport.h"
#include "LogosWebBridge.h"
#include "LogosWebCallRouter.h"
#include "LogosWebRuntime.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QRemoteObjectHost>
#include <QSignalSpy>
#include <QTest>

namespace {

// The counter UI's backend, reduced to what its `web` variant needs: a property
// the view binds to, a slot the button calls, and one slot with a return value
// so logos.watch() has something to resolve.
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

    int sum(int a, int b) { return a + b; }

Q_SIGNALS:
    void valueChanged(int value);

private:
    int m_value = 0;
};

// A second module's backend, on the SAME port and the same runtime.
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

// THE IDIOM A MODULE'S QML USES IN THE WEB CONTAINER, and the test's reason for
// spelling it out rather than hiding it in a helper.
//
// `logos.module()` hands back a DYNAMIC replica, whose metaobject does not
// exist until the source's meta arrives over the wire. A binding written
// against `backend.value` before that registers a dependency on nothing —
// there is no `value` yet to depend on — and would never re-run. So a view
// re-takes the replica on viewModuleReadyChanged, and only THEN is its binding
// bound to a real property with a real NOTIFY. Everything after that is
// ordinary QML, which is what the increments below check.
const char* kCounterQml = R"QML(
import QtQml
QtObject {
    id: root
    property var backend: null
    property int shown: (backend && backend.value !== undefined) ? backend.value : -1
    property string mine: logosModuleName

    function press() { if (backend) backend.increment() }

    Component.onCompleted: {
        logos.viewModuleReadyChanged.connect(function (name, ready) {
            if (name !== "counter" || !ready) return
            root.backend = logos.module(name)
        })
        root.backend = logos.module("counter")
    }
}
)QML";

const char* kGreeterQml = R"QML(
import QtQml
QtObject {
    id: root
    property var backend: null
    property string shown: (backend && backend.greeting !== undefined) ? backend.greeting : "-"

    function say(who) { if (backend) backend.greet(who) }

    Component.onCompleted: {
        logos.viewModuleReadyChanged.connect(function (name, ready) {
            if (name !== "greeter" || !ready) return
            root.backend = logos.module(name)
        })
        root.backend = logos.module("greeter")
    }
}
)QML";

} // namespace

class TestWebRuntime : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { LogosMessagePortTransport::registerTransport(); }
    void init() { ++m_run; }

    // ── installing a module's QML ───────────────────────────────────────
    void aModulesQmlIsCompiledIntoTheOneEngine();
    void qmlThatDoesNotCompileIsReportedNotSilentlyDropped();
    void reinstallingAModuleReplacesItsView();

    // ── the module's own backend, over the replica ──────────────────────
    void aBindingFollowsTheBackendAndASlotDrivesIt();
    void theSameRuntimeServesASecondModulesQml();
    void watchResolvesASlotsReturnValue();

    // ── other modules, over the host's call router ──────────────────────
    void callModuleAsyncReachesTheHostRouter();
    void aCallMadeBeforeTheBackendIsThereStillLands();
    void callModuleAsyncTimesOutWithAnErrorPayload();
    void callModuleIsRefusedAndNamesCallModuleAsync();

    // ── teardown ────────────────────────────────────────────────────────
    void theBridgeIsDestroyedBeforeTheNode();

private:
    QString name(const char* role) const
    {
        return QStringLiteral("r%1-%2").arg(m_run).arg(QLatin1StringView(role));
    }

    int m_run = 0;
};

// ─────────────────────────────────────────────────────────────────────────────

void TestWebRuntime::aModulesQmlIsCompiledIntoTheOneEngine()
{
    QQmlEngine engine;
    LogosWebRuntime runtime(&engine);

    QSignalSpy installed(&runtime, &LogosWebRuntime::moduleViewInstalled);

    QObject* view = runtime.installModuleView(QStringLiteral("counter"), kCounterQml);
    QVERIFY(view);
    QCOMPARE(installed.count(), 1);
    QCOMPARE(runtime.installedModules(), QStringList{ QStringLiteral("counter") });
    QCOMPARE(runtime.moduleView(QStringLiteral("counter")), view);
    QVERIFY(runtime.lastError().isEmpty());

    // A view knows its own name and nothing else about the runtime.
    QCOMPARE(view->property("mine").toString(), QStringLiteral("counter"));
}

void TestWebRuntime::qmlThatDoesNotCompileIsReportedNotSilentlyDropped()
{
    QQmlEngine engine;
    LogosWebRuntime runtime(&engine);

    QSignalSpy failed(&runtime, &LogosWebRuntime::moduleViewFailed);

    // A module whose QML is broken looks EXACTLY like one that has not loaded
    // yet — a blank rectangle — so the runtime has to say which it is.
    QObject* view = runtime.installModuleView(QStringLiteral("broken"),
                                              QStringLiteral("import QtQml\nNotAType { }"));
    QVERIFY(!view);
    QCOMPARE(failed.count(), 1);
    QVERIFY(!runtime.lastError().isEmpty());
    QVERIFY(runtime.installedModules().isEmpty());
}

void TestWebRuntime::reinstallingAModuleReplacesItsView()
{
    QQmlEngine engine;
    LogosWebRuntime runtime(&engine);

    QObject* first = runtime.installModuleView(QStringLiteral("counter"), kCounterQml);
    QVERIFY(first);
    QPointer<QObject> watched(first);

    QObject* second = runtime.installModuleView(QStringLiteral("counter"), kCounterQml);
    QVERIFY(second);
    QVERIFY(second != first);
    QTRY_VERIFY(watched.isNull());
    QCOMPARE(runtime.installedModules().count(), 1);
}

// THE ORDER, PINNED, because getting it wrong is invisible until it is not.
//
// ~QObject deletes children in the order they were ADDED, and the runtime adds
// its QtRO node before its bridge — so the DEFAULT destructor takes the node
// down first and the bridge's replicas, which are that node's clients and touch
// its private on the way out, second. It does not crash every time: the freed
// pages are usually still readable, so what it produced was an intermittent
// SIGSEGV in this suite, around one run in six, in whichever test happened to
// be last.
//
// Asserting the ORDER rather than the absence of a crash is the whole point —
// "run it a hundred times and see" is not a test.
void TestWebRuntime::theBridgeIsDestroyedBeforeTheNode()
{
    QQmlEngine engine;
    auto* runtime = new LogosWebRuntime(&engine);

    // A view and a replica, so the objects whose lifetime depends on the node
    // actually exist when it goes. `module()` answers null until the backend's
    // meta has arrived — there is no backend here — but the call is what
    // CREATES the replica, which is the object this test is about.
    QVERIFY(runtime->connectToBackend(name("teardown")));
    QVERIFY(runtime->installModuleView(QStringLiteral("counter"), kCounterQml));
    runtime->bridge()->module(QStringLiteral("counter"));

    QStringList order;
    connect(runtime->bridge(), &QObject::destroyed, this,
            [&order]() { order.append(QStringLiteral("bridge")); });
    connect(runtime->node(), &QObject::destroyed, this,
            [&order]() { order.append(QStringLiteral("node")); });

    delete runtime;

    QCOMPARE(order, (QStringList{ QStringLiteral("bridge"), QStringLiteral("node") }));
}

void TestWebRuntime::aBindingFollowsTheBackendAndASlotDrivesIt()
{
    QObject owner;
    auto pair = LogosLoopbackMessagePort::createPair(&owner);
    LogosMessagePortTransport::publish(name("backend"), pair.first);
    LogosMessagePortTransport::publish(name("runtime"), pair.second);

    CounterSource counter;
    QRemoteObjectHost host;
    QVERIFY(host.setHostUrl(LogosMessagePortTransport::url(name("backend"))));
    QVERIFY(host.enableRemoting(&counter, QStringLiteral("counter")));

    QQmlEngine engine;
    LogosWebRuntime runtime(&engine);
    QVERIFY(runtime.connectToBackend(name("runtime")));

    QObject* view = runtime.installModuleView(QStringLiteral("counter"), kCounterQml);
    QVERIFY(view);

    // The view is up before the replica is, showing its own placeholder — the
    // ordinary first paint.
    QCOMPARE(view->property("shown").toInt(), -1);

    // ...and then the source's meta arrives and the view shows the backend.
    QTRY_COMPARE(view->property("shown").toInt(), 0);
    QVERIFY(runtime.bridge()->isViewModuleReady(QStringLiteral("counter")));

    // THE BUTTON. A slot invoked from the module's own QML drives the backend...
    QVERIFY(QMetaObject::invokeMethod(view, "press"));
    QTRY_COMPARE(counter.value(), 1);

    // ...and the property change comes back on its own, with nothing in the
    // view asking for it. That is criterion 2's second half.
    QTRY_COMPARE(view->property("shown").toInt(), 1);
}

void TestWebRuntime::theSameRuntimeServesASecondModulesQml()
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

    QQmlEngine engine;
    LogosWebRuntime runtime(&engine);
    QVERIFY(runtime.connectToBackend(name("runtime")));

    // ONE RUNTIME, ONE ENGINE, ONE NODE, ONE PORT — and a second module costs a
    // document. That is the whole of "the same runtime serves a second module's
    // QML without a second runtime download": there is nothing here to download
    // a second time.
    QObject* counterView = runtime.installModuleView(QStringLiteral("counter"), kCounterQml);
    QObject* greeterView = runtime.installModuleView(QStringLiteral("greeter"), kGreeterQml);
    QVERIFY(counterView);
    QVERIFY(greeterView);
    QCOMPARE(runtime.installedModules().count(), 2);

    QTRY_COMPARE(counterView->property("shown").toInt(), 0);
    QTRY_COMPARE(greeterView->property("shown").toString(), QStringLiteral(""));

    QVERIFY(QMetaObject::invokeMethod(counterView, "press"));
    QVERIFY(QMetaObject::invokeMethod(greeterView, "say",
                                      Q_ARG(QVariant, QVariant(QStringLiteral("logos")))));

    QTRY_COMPARE(counterView->property("shown").toInt(), 1);
    QTRY_COMPARE(greeterView->property("shown").toString(), QStringLiteral("hello, logos"));
}

void TestWebRuntime::watchResolvesASlotsReturnValue()
{
    QObject owner;
    auto pair = LogosLoopbackMessagePort::createPair(&owner);
    LogosMessagePortTransport::publish(name("backend"), pair.first);
    LogosMessagePortTransport::publish(name("runtime"), pair.second);

    CounterSource counter;
    QRemoteObjectHost host;
    QVERIFY(host.setHostUrl(LogosMessagePortTransport::url(name("backend"))));
    QVERIFY(host.enableRemoting(&counter, QStringLiteral("counter")));

    QQmlEngine engine;
    LogosWebRuntime runtime(&engine);
    QVERIFY(runtime.connectToBackend(name("runtime")));

    // A slot with a return value hands QML a pending call, and logos.watch()
    // resolves it — so a view never imports QtRemoteObjects.
    const char* qml = R"QML(
import QtQml
QtObject {
    id: root
    property var backend: null
    property int answer: -1
    property bool failed: false

    function ask() {
        logos.watch(backend.sum(2, 3),
                    function (v) { root.answer = v },
                    function (e) { root.failed = true })
    }

    Component.onCompleted: {
        logos.viewModuleReadyChanged.connect(function (name, ready) {
            if (name !== "counter" || !ready) return
            root.backend = logos.module(name)
        })
        root.backend = logos.module("counter")
    }
}
)QML";

    QObject* view = runtime.installModuleView(QStringLiteral("counter"), QString::fromUtf8(qml));
    QVERIFY(view);
    QTRY_VERIFY(runtime.bridge()->isViewModuleReady(QStringLiteral("counter")));

    QVERIFY(QMetaObject::invokeMethod(view, "ask"));
    QTRY_COMPARE(view->property("answer").toInt(), 5);
    QVERIFY(!view->property("failed").toBool());
}

void TestWebRuntime::callModuleAsyncReachesTheHostRouter()
{
    QObject owner;
    auto pair = LogosLoopbackMessagePort::createPair(&owner);
    LogosMessagePortTransport::publish(name("backend"), pair.first);
    LogosMessagePortTransport::publish(name("runtime"), pair.second);

    // THE WASM HOST'S HALF. In the page this router is inside the module's
    // Worker, and its handler is a logos-protocol client speaking to the core
    // over the webview bridge; here the handler is the test, and what is being
    // checked is everything between the view's callback and that seam.
    LogosWebCallRouter router;
    QStringList seen;
    router.setHandler([&](const QString& id, const QString& module,
                          const QString& method, const QString& argsJson) {
        seen << (module + QLatin1Char('.') + method + argsJson);
        QJsonObject result;
        result.insert(QStringLiteral("success"), true);
        result.insert(QStringLiteral("value"), 3);
        router.complete(id, QString::fromUtf8(
            QJsonDocument(result).toJson(QJsonDocument::Compact)));
    });

    QRemoteObjectHost host;
    QVERIFY(host.setHostUrl(LogosMessagePortTransport::url(name("backend"))));
    QVERIFY(host.enableRemoting(&router, LogosWebCallRouter::sourceName()));

    QQmlEngine engine;
    LogosWebRuntime runtime(&engine);
    QVERIFY(runtime.connectToBackend(name("runtime")));

    const char* qml = R"QML(
import QtQml
QtObject {
    id: root
    property string payload: ""
    property int calls: 0
    function ask() {
        logos.callModuleAsync("native_counter", "add", [1, 2], function (p) {
            root.payload = p
            root.calls += 1
        }, 5000)
    }
}
)QML";

    QObject* view = runtime.installModuleView(QStringLiteral("counter"), QString::fromUtf8(qml));
    QVERIFY(view);

    QVERIFY(QMetaObject::invokeMethod(view, "ask"));
    QTRY_COMPARE(view->property("calls").toInt(), 1);

    // A JSON STRING, exactly as on the desktop — a view that does
    // JSON.parse(payload).value works in either container.
    const QJsonObject payload =
        QJsonDocument::fromJson(view->property("payload").toString().toUtf8()).object();
    QCOMPARE(payload.value(QStringLiteral("value")).toInt(), 3);

    QCOMPARE(seen.count(), 1);
    QCOMPARE(seen.first(), QStringLiteral("native_counter.add[1,2]"));

    // EXACTLY ONCE. The timeout above is still armed; it must find nothing.
    QTest::qWait(100);
    QCOMPARE(view->property("calls").toInt(), 1);
}

void TestWebRuntime::aCallMadeBeforeTheBackendIsThereStillLands()
{
    QObject owner;

    QQmlEngine engine;
    LogosWebRuntime runtime(&engine);
    QVERIFY(runtime.connectToBackend(name("runtime")));

    const char* qml = R"QML(
import QtQml
QtObject {
    id: root
    property string payload: ""
    Component.onCompleted: {
        // The one moment a view is guaranteed to be earlier than its Worker.
        logos.callModuleAsync("native_counter", "add", [], function (p) {
            root.payload = p
        }, 15000)
    }
}
)QML";

    QObject* view = runtime.installModuleView(QStringLiteral("counter"), QString::fromUtf8(qml));
    QVERIFY(view);
    QCOMPARE(runtime.bridge()->pendingCallIds().count(), 1);
    QVERIFY(view->property("payload").toString().isEmpty());

    // Now the Worker comes up and hands its port over.
    auto pair = LogosLoopbackMessagePort::createPair(&owner);
    LogosMessagePortTransport::publish(name("backend"), pair.first);
    LogosMessagePortTransport::publish(name("runtime"), pair.second);

    LogosWebCallRouter router;
    router.setHandler([&](const QString& id, const QString&, const QString&, const QString&) {
        router.complete(id, QStringLiteral("{\"value\":\"late\"}"));
    });
    QRemoteObjectHost host;
    QVERIFY(host.setHostUrl(LogosMessagePortTransport::url(name("backend"))));
    QVERIFY(host.enableRemoting(&router, LogosWebCallRouter::sourceName()));

    QTRY_COMPARE(view->property("payload").toString(), QStringLiteral("{\"value\":\"late\"}"));
    QVERIFY(runtime.bridge()->pendingCallIds().isEmpty());
}

void TestWebRuntime::callModuleAsyncTimesOutWithAnErrorPayload()
{
    QObject owner;
    auto pair = LogosLoopbackMessagePort::createPair(&owner);
    LogosMessagePortTransport::publish(name("backend"), pair.first);
    LogosMessagePortTransport::publish(name("runtime"), pair.second);

    // A router that never answers: the module is there, the call is not.
    LogosWebCallRouter router;
    router.setHandler([](const QString&, const QString&, const QString&, const QString&) { });
    QRemoteObjectHost host;
    QVERIFY(host.setHostUrl(LogosMessagePortTransport::url(name("backend"))));
    QVERIFY(host.enableRemoting(&router, LogosWebCallRouter::sourceName()));

    QQmlEngine engine;
    LogosWebRuntime runtime(&engine);
    QVERIFY(runtime.connectToBackend(name("runtime")));

    const char* qml = R"QML(
import QtQml
QtObject {
    id: root
    property string payload: ""
    Component.onCompleted: {
        logos.callModuleAsync("silent", "never", [], function (p) { root.payload = p }, 300)
    }
}
)QML";

    QObject* view = runtime.installModuleView(QStringLiteral("counter"), QString::fromUtf8(qml));
    QVERIFY(view);

    QTRY_VERIFY(!view->property("payload").toString().isEmpty());
    const QJsonObject payload =
        QJsonDocument::fromJson(view->property("payload").toString().toUtf8()).object();
    QVERIFY(payload.contains(QStringLiteral("error")));
    QCOMPARE(payload.value(QStringLiteral("module")).toString(), QStringLiteral("silent"));
}

void TestWebRuntime::callModuleIsRefusedAndNamesCallModuleAsync()
{
    QQmlEngine engine;
    LogosWebRuntime runtime(&engine);

    // Refusing is the honest answer, and the error has to say what to do
    // instead: a page that blocked its event loop waiting for a MessagePort
    // reply has stopped reading the port the reply arrives on.
    const QString payload = runtime.bridge()->callModule(QStringLiteral("keystore"),
                                                         QStringLiteral("unlock"));
    const QJsonObject obj = QJsonDocument::fromJson(payload.toUtf8()).object();
    QVERIFY(obj.contains(QStringLiteral("error")));
    QVERIFY(obj.value(QStringLiteral("message")).toString().contains(
        QStringLiteral("callModuleAsync")));
}

QTEST_GUILESS_MAIN(TestWebRuntime)
#include "test_web_runtime.moc"
