# logos-view-module-runtime

Shared runtime for hosting Logos **view modules** (Qt/QML UI plugins) in a child
process, isolated from the main application.

This repo exists so that `logos-basecamp`, `logos-standalone-app`, and any
future Logos host application can link the same library and use the same
`ui-host` binary instead of each one carrying its own copy.

## What's in here

- **`logos_view_module_runtime`** — static C++ library, linked into host
  applications (or their plugins). Provides:
  - `LogosQmlBridge` — `QObject` exposed to QML as `logos`. Routes
    `callModule` / `callModuleAsync` to **backend** modules via `LogosAPI`
    (IPC); results are serialized to JSON strings so QML always sees a string.
    **View** modules are reached through `logos.module(name)` /
    `logos.model(name)`, which hand back a typed replica built by that module's
    `LogosViewReplicaFactory` plugin. Calling `callModule` on a view module is
    refused with an error payload, not routed.
  - `LogosIntent.h` — the **frozen** app-to-app intent vocabulary: the six
    error codes, the intent-name grammar, the payload rules and the result
    envelope. Header-only and not a `QObject`, so both a view module and a
    host's broker include the same definitions and cannot disagree about what
    an error code means. See "App-to-app intents" below.
  - `ViewModuleHost` — spawns a `ui-host` child process for a given view
    module plugin, generates a unique local socket name, watches stdout for
    `READY`, and emits `ready()`. The parent then points `LogosQmlBridge` at
    that socket via `setViewModuleSocket(name, socket)`.

- **`ui-host`** — standalone executable. Loads a single Qt plugin
  (`--path <plugin.so>`), calls `initLogos(LogosAPI*)` on it via reflection
  (`QMetaObject::invokeMethod`), and then exposes a QObject on a
  `QRemoteObjectHost` at the socket given by `--socket`. Remoting strategy:
  - **Typed remoting (preferred)**: if the plugin declares the
    `LogosViewPlugin` interface (from `logos-plugin-qt`) via
    `Q_INTERFACES(LogosViewPlugin)` so that `qobject_cast<LogosViewPlugin*>`
    succeeds, `ui-host` calls
    `viewPlugin->enableRemoting(&host)`. The generated
    `<Foo>ViewPluginBase` (produced by `logos_module(REP_FILE …)` in
    `logos-plugin-qt`) invokes `host->enableRemoting<FooSourceAPI>(backend)`
    so typed replicas on the client side reach the `Valid` state. The
    remoted object is `viewPlugin->viewObject()`.
  - **Dynamic remoting (fallback)**: for plugins without a `.rep` /
    `LogosViewPlugin` implementation, `ui-host` falls back to
    `host.enableRemoting(pluginObject, moduleName)`, which propagates all
    `Q_INVOKABLE`s, slots, signals, and `Q_PROPERTY`s (with `NOTIFY`) via a
    `QRemoteObjectDynamicReplica` on the client side.

  Any `Q_PROPERTY` on the remoted object whose value is a
  `QAbstractItemModel*` is additionally remoted as a child source named
  `<moduleName>/<propertyName>`. Prints `READY` once it's listening.

## View object convention

A view module plugin keeps its plugin-lifecycle class separate from the
QObject that QML actually talks to. The preferred path is to inherit the
generated `<Foo>ViewPluginBase` from `logos-plugin-qt` (produced by
`logos_module(REP_FILE my_view.rep …)`), which implements `LogosViewPlugin`
and wires typed remoting:

```cpp
class MyPlugin : public MyViewPluginBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "co.logos.MyPlugin" FILE "metadata.json")
    Q_INTERFACES(PluginInterface LogosViewPlugin)
public:
    Q_INVOKABLE void initLogos(LogosAPI* api) {
        m_backend = new MyBackend(api, this);
    }
    QObject* viewObject() override { return m_backend; }
private:
    MyBackend* m_backend = nullptr;
};
```

`ui-host` calls `viewPlugin->enableRemoting(&host)`, which internally does
`host->enableRemoting<MySourceAPI>(m_backend)` using the typed source
generated from the `.rep` file. QML on the parent side talks to
`MyBackend` via a typed replica.

For plugins without a `.rep` file (no `LogosViewPlugin` implementation),
`ui-host` falls back to dynamic remoting of the plugin object itself — this
keeps legacy modules working unchanged.

## Architecture

```
┌────────────────────────────┐         ┌──────────────────────────┐
│ Host app (basecamp / etc.) │         │ ui-host (child process)  │
│                            │         │                          │
│   QML  ──logos.module()────▶         │   QRemoteObjectHost      │
│           │                │ QRO     │     │                    │
│   LogosQmlBridge ──────────┼────────▶│     ▼                    │
│           │                │ local   │   QPluginLoader          │
│           ▼                │ socket  │     │                    │
│   ViewModuleHost ──spawn──▶│         │     ▼                    │
│                            │         │   <view module>.so       │
└────────────────────────────┘         └──────────────────────────┘
```

Each view module gets its own `ui-host` process and its own private socket, so
a crash or hang in one view module cannot take down the host app or other
view modules.

Non-view backend modules continue to use the existing `LogosAPI` IPC path
unchanged — `LogosQmlBridge` only switches to QRO when the requested module
name was previously registered via `setViewModuleSocket`.

## QtRO over a MessagePort (the Web container's wire)

On desktop a view module is reached over a **local socket** — `ui-host` in a
child process, `ViewModuleHost` spawning it. In the Web container there is no
socket and no child process: the QML runtime and the module's backend are two
wasm images in one page, and the only channel a browser offers between them is
an HTML **MessagePort** (ADR 0004). `logos_messageport` is that same QtRO wire
over that channel, and it is a **separate library** from
`logos_view_module_runtime` on purpose: it links Qt and nothing else, because
inside a Qt-for-WebAssembly image `LogosAPI`, the token manager and the rest of
the native SDK stack do not exist.

```
   the page                         the module's Worker
   ┌───────────────────────┐        ┌───────────────────────┐
   │ Qt-wasm QML runtime   │        │ Wasm host (backend)   │
   │   QRemoteObjectNode   │        │   QRemoteObjectHost   │
   │     messageport:      │        │     messageport:      │
   │        backend  ──────┼────────┼──────►  runtime       │
   └───────────────────────┘  one   └───────────────────────┘
                          MessagePort
```

Three symbols, and the node API is the one every other transport already uses:

```cpp
LogosMessagePortTransport::registerTransport();                 // once
LogosMessagePortTransport::publish("backend", port);            // the port you were handed

QRemoteObjectNode node;
node.connectToNode(LogosMessagePortTransport::url("backend"));  // messageport:backend
auto* replica = node.acquireDynamic("counter");
```

and on the hosting side, unchanged but for the URL:

```cpp
QRemoteObjectHost host;
host.setHostUrl(LogosMessagePortTransport::url("runtime"));
host.enableRemoting(backend, "counter");
```

Three things about it are worth knowing before you use it:

- **A port name is process-local, not a rendezvous.** `messageport:backend`
  means "the port THIS process published as `backend`". A browser has no
  directory to look one up in — you are handed a port or you have none — so the
  two ends name their own halves and are free to use the same name.
- **Delivery is off until `start()`.** Exactly as an HTML MessagePort's queue
  is. `LogosMessagePortDevice` calls it when it attaches, which is what keeps
  the object list a backend writes the instant it begins hosting from being
  dropped on the floor by a runtime that has not connected yet.
- **A runtime that starts before its backend just waits.** Connecting to a name
  nothing has published yet is not an error; QtRO's reconnect timer retries
  until the port appears.

In the browser the page hands the port over through one embind call and never
touches a Qt type:

```js
const channel = new MessageChannel();
worker.postMessage({ logosPort: channel.port2 }, [channel.port2]);
Module.logosAdoptMessagePort('backend', channel.port1);
```

The transport's behaviour is checked on the **desktop**, against a real
`QRemoteObjectHost` over a loopback port pair with the same four properties a
MessagePort has (`tests/test_messageport_transport.cpp`).

## The QML runtime (the Web container's page)

The transport above is the wire. `logos_web_runtime` is what sits on it: ADR
0004's **bundled QML runtime** — one Qt-for-WebAssembly image, shipped and
signed with the app, into which every Downloaded module's QML is loaded at
install time.

```
   ~26 MB of runtime, once                 a module's QML, per module
   ┌──────────────────────────────┐        ┌─────────┐ ┌─────────┐
   │ Qt Quick + Logos design      │  ◄──── │ counter │ │ wallet  │   text, fetched
   │ system + MessagePort QtRO    │        └─────────┘ └─────────┘
   │ + LogosWebRuntime            │
   └──────────────────────────────┘
```

Three classes, one of which a host never touches:

- **`LogosWebRuntime`** — the runtime. Registers the scheme, connects one node
  to the port the page published, puts one `LogosWebBridge` in the engine's root
  context as `logos`, and compiles a module's QML from a string
  (`installModuleView`). A second module costs a document, not a second image.
- **`LogosWebBridge`** — `logos`, as a module's QML sees it. The same three
  calls the desktop bridge offers a view: `module()`, `callModuleAsync()`,
  `watch()`.
- **`LogosWebCallRouter`** — the other half of `callModuleAsync`, hosted by the
  module's **Wasm host** and reached over the same port. A view calling a
  *native* module by name has to leave the page somehow, and a QtRO source with
  a handler on it is that seam; the runtime has no LogosAPI and no token store
  in it, and must not grow one.

From the page, the whole API is five embind calls and no Qt type:

```js
const channel = new MessageChannel();
worker.postMessage({ logosPort: channel.port2 }, [channel.port2]);
Module.logosAdoptMessagePort('backend', channel.port1);
Module.logosInstallModuleView('counter', await (await fetch(qmlUrl)).text());
// Module.logosRemoveModuleView(name), Module.logosRuntimeLastError()
// Module.logosConnectBackend(name) — only for a port published under a name
// other than `backend`, which the image connects to on its own
```

### The one way a module's QML differs from the desktop

It is one difference and not two: the NAMES are the desktop's.
`viewModuleReadyChanged` and `isViewModuleReady` are spelled exactly as
`LogosQmlBridge` spells them, so a module's document is the same file in both
containers. What differs is only WHEN the backend arrives.

`logos.module(name)` **answers null until the backend is there**, and a view
takes it again on `viewModuleReadyChanged`:

```qml
property var backend: null
property int shown: (backend && backend.value !== undefined) ? backend.value : -1

Component.onCompleted: {
    logos.viewModuleReadyChanged.connect(function (name, ready) {
        if (name === "counter" && ready) backend = logos.module(name)
    })
    backend = logos.module("counter")     // the early call is what starts the acquire
}
```

That is not a style preference. A desktop host loads the view module's generated
factory plugin and gets a **typed** replica whose metaobject is compiled in; a
page cannot dlopen, so the types come off the wire and the replica is a
**dynamic** one. Qt's QML engine builds a property cache for an object the first
time JS touches it and keeps it — hand a dynamic replica over early and QML
caches the generic `QRemoteObjectReplica` metaobject, after which the module's
properties read `undefined` forever and its slots are "not a function". Waiting
is the only version of this that works, and returning null is how the bridge
makes the wait visible instead of silent.

Everything after that edge is ordinary QML: a slot call drives the backend and
the property change comes back on its own.

`callModule` — the synchronous form — is **refused** here, with an error payload
naming `callModuleAsync`. The reply has to cross a MessagePort, a MessagePort
delivers through the event loop, and a page that blocks its event loop waiting
has stopped reading the port the reply arrives on.

### What a module's QML may import

`wasm/runtime/RuntimeImports.qml`, and nothing else. A static Qt has no plugin
directory to search, so a QML module is reachable only if **the build saw the
import** — `qmlimportscanner` reads this target's own QML and links the plugins
it names. A module's document arrives at runtime and is therefore invisible to
that scan, which makes that one file the runtime's published surface: adding to
it costs image size, removing from it breaks modules already published.

Linking the CMake target is not enough and looks exactly like enough. With
`Qt6::QmlCore` linked but nothing importing `QtCore`, the image builds, boots
and paints its own shell, and the first document that reaches `Logos.Theme`
fails with `plugin "qtqmlcoreplugin" not found`.

### The image

`nix build .#qml-runtime-wasm`: this repo's web half compiled for
wasm32-emscripten against logos-nix' Qt-for-WebAssembly, installed as a prefix,
and linked off that prefix into the runtime app in `wasm/runtime/`. The build
asserts what a link cannot: that every page-facing embind export is in the image
(nothing in C++ references them, so a linker is free to drop them), and that the
design system's QML plugins are still in it (under a static Qt, Qt's own plugin
auto-import and the design system's `WHOLE_ARCHIVE` umbrella compete for the
same plugins and the loser is silent). It logs the image raw and brotli against
ADR 0004's budget: **25,888,755 B / 6,674,406 B** on aarch64-darwin.

### The browser smoke

```sh
nix build .#qml-runtime-wasm
node wasm/runtime/browser-smoke/run.mjs result/www
```

Not a nix check and it cannot become one — the sandbox has no browser and
darwin has no chromium in nixpkgs — so it is run by hand, on a venue with a
Chrome. It serves the built `www/` over http (a `file://` page cannot fetch a
sibling `.wasm`), boots the image in headless Chrome, and asserts every embind
export, a real `MessagePort` adopted, each promised QML module instantiated,
**two** modules' documents installed into the one image, a broken document
reported rather than swallowed, and a removal.

It exists because a class of failure lives only here, and it caught three of
them while it was being written:

- `QGuiApplication::exec()` **returns** in a wasm image, so an engine on
  `main()`'s stack is gone before the page's first call arrives;
- `import QtCore` needed both `Qt6::QmlCore` linked and an import the build
  could see (above);
- `return 1` from `main()` aborts the emscripten runtime, after which every call
  from the page throws a bare pointer — including the one that would have asked
  what went wrong.

What it still does not cover is a **peer**: nothing hosts a QtRO source on the
other end of the port, so the replica half is proven on the desktop only
(`tests/test_web_runtime.cpp`, against a real `QRemoteObjectHost` and a real
`QQmlEngine`).

## Building

### Nix (recommended)

```sh
nix build .#default
```

Outputs:
- `result/lib/liblogos_view_module_runtime.a`
- `result/lib/liblogos_messageport.a`
- `result/lib/liblogos_web_runtime.a`
- `result/include/` — public headers
- `result/bin/ui-host`

The Web container's half, against logos-nix' Qt-for-WebAssembly, linked into the
QML runtime image (`result/www/` is what a page loads):

```sh
nix build .#qml-runtime-wasm
```

### CMake (manual)

```sh
cmake -S . -B build -GNinja \
  -DLOGOS_CPP_SDK_ROOT=/path/to/logos-cpp-sdk \
  -DLOGOS_QT_HOST_ROOT=/path/to/logos-qt-host \
  -DLOGOS_PROTOCOL_ROOT=/path/to/logos-protocol
cmake --build build
cmake --install build --prefix ./out
```

All three roots are required — the build stops with `FATAL_ERROR` if any is
undefined, unless `-DLOGOS_WEB_ONLY=ON` is passed, which builds
`logos_messageport` and `logos_web_runtime` and stops there (this is what the
wasm build does, and it is also how the Web half's own tests are built on a
desktop with no SDK around).
`LOGOS_CPP_SDK_ROOT` must point at an installed `logos-cpp-sdk` (provides
`logos_api.h` and `liblogos_sdk`), `LOGOS_QT_HOST_ROOT` at `logos-qt-host`, and
`LOGOS_PROTOCOL_ROOT` at `logos-protocol`.

## Consuming from another repo

In the consumer's `flake.nix`:

```nix
inputs.logos-view-module-runtime.url = "github:logos-co/logos-view-module-runtime";
```

Pass the package into the consumer's app derivation and forward it as a CMake
variable:

```nix
cmakeFlags = [
  "-DLOGOS_VIEW_MODULE_RUNTIME_ROOT=${logosViewModuleRuntime}"
];
```

In the consumer's `CMakeLists.txt`:

```cmake
target_include_directories(my_app PRIVATE ${LOGOS_VIEW_MODULE_RUNTIME_ROOT}/include)
target_link_directories(my_app PRIVATE ${LOGOS_VIEW_MODULE_RUNTIME_ROOT}/lib)
target_link_libraries(my_app PRIVATE logos_view_module_runtime)
```

The `ui-host` binary should be copied into the app's `bin/` directory at
install time so `ViewModuleHost` can `QProcess::start("ui-host", ...)` it:

```nix
cp ${logosViewModuleRuntime}/bin/ui-host $out/bin/ui-host
```

## Using the bridge

```cpp
auto* api = new LogosAPI(/* ... */);
auto* bridge = new LogosQmlBridge(api, this);
engine.rootContext()->setContextProperty("logos", bridge);

// For a view module, spawn its host process and wire the bridge to its socket
auto* host = new ViewModuleHost(this);
connect(host, &ViewModuleHost::ready, this, [bridge, host] {
    bridge->setViewModuleSocket("my_view_module", host->socketName());
});
if (!host->spawn("my_view_module", "/path/to/my_view_module.so", authToken)) {
    qWarning() << "Failed to start view module host";
}
```

From QML:

```qml
import QtQuick
Item {
    // A view module is a typed replica, not a JSON call. Properties, slots and
    // signals are reached directly; `callModule` on this name is refused.
    property var backend: logos.module("my_view_module")

    Text { text: backend.someProperty }

    Component.onCompleted: {
        // Slots that return a value hand back a pending call — logos.watch()
        // resolves it, replacing QtRemoteObjects.watch().
        logos.watch(backend.getStatus(), function (result) {
            console.log(result);
        });
    }
}
```

`callModuleAsync` is for **backend** modules, where the result really is a JSON
string:

```qml
logos.callModuleAsync("my_backend_module", "getStatus", [], function (payload) {
    console.log(JSON.parse(payload).value);
});
```

## App-to-app intents

One app asks for a capability; the shell decides who services it. This repo owns
the **frozen half** of that surface — the part apps compile against — and
nothing else. Resolution, consent and dispatch are host policy and live in the
host (in Basecamp, `IntentBroker`).

Three members on the bridge, plus `LogosIntent.h`:

```qml
// Ask. You never name a provider, and never learn which apps are installed.
logos.request("wallet.send", { to: "0xabc", amount: 12.5 }, function (res) {
    if (res.ok) console.log(res.data.txHash)
    else        console.log(res.error)   // one of six codes
})

// Answer, if you declared `provides` in metadata.json.
Connections {
    target: logos
    function onIntentRequested(requestId, intent, params, requesterName) {
        logos.respond(requestId, true, { txHash: "0x…" }, "")
    }
}
```

Frozen means these signatures do not change: `request`, `respond`,
`intentRequested`, the codes in `LogosIntent.h`, and the payload bounds. A host
may replace everything behind them.

Points a host implementer has to honour, because the surface assumes them:

- **`respond` takes all four arguments.** A provider that omits `error` on a
  failure path must not fall into reporting success, so there are no defaults.
- **`requesterName` is host-attested.** The router knows who called by
  construction; it is not read from the payload, and a caller cannot forge it.
- **Payloads are plain data only** — `isCanonicalPayload()` bounds depth,
  size and type, and refuses `QObject*` and `QJSValue`. That is what stops one
  app handing another a live handle into its engine. `respond` flattens
  engine-bound values on the way out for the same reason.
- **A provider may only report `cancelled`, `timeout`, `failed` or
  `bad_request`.** `normalizeError()` coerces anything else, because
  `not_declared` and `unavailable` carry meaning a provider is not entitled to
  assert — both reveal whether a provider exists at all.
- **Every request terminates exactly once**, asynchronously, even on immediate
  failure — for as long as the requester is there to hear it. If its engine is
  torn down or hot-reloaded first, the pending callbacks are dropped uninvoked
  and the broker is told via `intentsAbandoned()`. That is deliberate: running a
  callback against torn-down JS is worse than not answering.

Full reference: `logos-basecamp/docs/app-to-app-intents.md` and
`logos-tutorial/guide-intents-for-app-developers.md`.

## Dependencies

- Qt 6: `Core`, `Qml`, `RemoteObjects`
- `logos-plugin-qt`'s `logos-qt-host` (for `LogosAPI` / `logos_api.h`) —
  the Qt host runtime, linked as `logos-qt-host::logos_qt_host`
- `logos-protocol` (`token_manager.h`, `module_proxy.h`, `remote_transport.h`,
  …) — carried transitively by `logos-qt-host`
- `logos-cpp-sdk` (header-only types, via `logos-cpp-sdk::logos_headers`)

That's it — deliberately no dependency on `logos-liblogos`, `logos-module`, or
any specific module repo, so this runtime stays a thin shared layer.
