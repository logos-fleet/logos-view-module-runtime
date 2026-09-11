#pragma once

#include "LogosMessagePort.h"

#include <QString>
#include <QUrl>

// ── LogosMessagePortTransport ───────────────────────────────────────────────
//
// Qt Remote Objects over a MessagePort, registered as the `messageport:`
// scheme so that both ends are ordinary QtRO:
//
//     LogosMessagePortTransport::registerTransport();
//
//     // the module's backend, inside its Wasm host
//     LogosMessagePortTransport::publish("backend", portToTheRuntime);
//     QRemoteObjectHost host(LogosMessagePortTransport::url("backend"));
//     host.enableRemoting(counter, "counter");
//
//     // the QML runtime, in the page
//     LogosMessagePortTransport::publish("runtime", portToTheBackend);
//     QRemoteObjectNode node;
//     node.connectToNode(LogosMessagePortTransport::url("runtime"));
//     auto* replica = node.acquireDynamic("counter");
//
// A SCHEME RATHER THAN addClientSideConnection(). QtRO's external-device API
// would also carry these bytes, but it makes the transport the CALLER's
// problem: every host would have to build the device, decide when to hand it
// over, and re-do it on a reconnect. Registering with QtRO's connection
// factories — the extension point qconnectionfactories.h is exported for —
// keeps the node API the one every other Logos transport already uses, and
// gets the node's reconnect timer for free: a runtime that comes up before its
// backend retries until the port appears instead of failing once.
//
// A PORT NAME IS PROCESS-LOCAL, NOT A RENDEZVOUS. `messageport:runtime` means
// "the port this process published as `runtime`", never "find me whoever else
// calls itself runtime" — a browser has no such directory, and inventing one
// here would be inventing an identity a page could assert about itself, which
// is precisely what ADR 0005 forbids. The two ends of a channel therefore name
// their OWN halves, and are free to name them the same thing because they are
// never in the same process. Tests, which are, name them differently.
namespace LogosMessagePortTransport {

// "messageport". A function rather than a constant so there is exactly one
// spelling of it anywhere in the system.
QString scheme();

// The URL for a published port: `messageport:<portName>`.
QUrl url(const QString& portName);

// Teach QtRO the scheme. Idempotent, and safe to call from anywhere — every
// entry point that might be first (a host, a runtime, a test) calls it.
void registerTransport();

// Make `port` available to whatever connects to or listens on
// `messageport:<portName>` IN THIS PROCESS. The port is borrowed: the caller
// keeps ownership and must outlive the QtRO node using it.
//
// Publishing a second port under a name already taken REPLACES it — a page
// that reloaded its Worker hands over a new port, and the old one is dead by
// then anyway.
void publish(const QString& portName, LogosMessagePort* port);

// Remove a published port that nobody took yet. Ports already handed to a node
// are unaffected: withdrawing a name cannot revoke a live connection, which
// would be a channel closed by someone other than its two ends.
void withdraw(const QString& portName);

// The port published under this name and not yet taken, or nullptr. Exists for
// tests and diagnostics; the transport itself takes ports rather than peeking.
LogosMessagePort* published(const QString& portName);

} // namespace LogosMessagePortTransport
