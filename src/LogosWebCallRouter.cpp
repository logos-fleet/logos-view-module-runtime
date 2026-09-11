#include "LogosWebCallRouter.h"

#include "LogosWebPayload.h"

LogosWebCallRouter::LogosWebCallRouter(QObject* parent)
    : QObject(parent)
{
}

LogosWebCallRouter::~LogosWebCallRouter() = default;

QString LogosWebCallRouter::sourceName()
{
    return QStringLiteral("logos_bridge");
}

void LogosWebCallRouter::setHandler(Handler handler)
{
    m_handler = std::move(handler);
}

void LogosWebCallRouter::call(const QString& requestId,
                              const QString& module,
                              const QString& method,
                              const QString& argsJson)
{
    if (requestId.isEmpty())
        return;

    m_inFlight.insert(requestId);

    if (!m_handler) {
        // ANSWERED, NOT DROPPED. A host that has not installed its handler yet
        // is a real state — the image is up before its protocol client is — and
        // the view's only other outcome would be its own timeout, seconds later
        // and with nothing to say about why.
        complete(requestId,
                 logosWebErrorPayload(QStringLiteral("No backend router"), module, method));
        return;
    }

    m_handler(requestId, module, method, argsJson);
}

void LogosWebCallRouter::complete(const QString& requestId, const QString& payloadJson)
{
    if (m_inFlight.remove(requestId) == 0)
        return;
    emit callCompleted(requestId, payloadJson);
}
