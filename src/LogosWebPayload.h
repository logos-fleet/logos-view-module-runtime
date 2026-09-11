#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

// THE ERROR ENVELOPE THE WEB HALF ANSWERS WITH, in one place because both ends
// of it are in this library and a page reads them the same way: whatever fails,
// a view's `JSON.parse(payload).error` has to find something. The bridge builds
// one when it cannot reach the router at all, and the router builds one when it
// has no handler to ask — two sites, one shape, and the shape is also
// LogosQmlBridge's, so a view moved from the desktop container to this one
// keeps working.
//
// Private to src/: it is an implementation detail of the payloads, not part of
// what a Wasm host compiles against.
inline QString logosWebErrorPayload(const QString& error,
                                    const QString& module = QString(),
                                    const QString& method = QString(),
                                    const QString& detail = QString())
{
    QJsonObject obj;
    obj.insert(QStringLiteral("error"), error);
    if (!module.isEmpty()) obj.insert(QStringLiteral("module"), module);
    if (!method.isEmpty()) obj.insert(QStringLiteral("method"), method);
    if (!detail.isEmpty()) obj.insert(QStringLiteral("message"), detail);
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}
