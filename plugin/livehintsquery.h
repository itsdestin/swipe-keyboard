#pragma once
#include <QCoreApplication>
#include <QInputMethodQueryEvent>
#include <QPointer>
#include <QVariant>

struct LiveHintsQuery {
    bool alive = false;
    bool valid = false;
    int hints = 0;
};

// WHY: authorization queries only public ImHints on the actual proxy, never text or Qt's
// cache-update API. Qt's cached flags can lag behind a sensitive backend purpose. A query may
// synchronously reenter/delete its receiver; do not dereference it after.
inline LiveHintsQuery queryLiveHints(QObject *item)
{
    QPointer<QObject> guard(item);
    if (!guard) return {};
    QInputMethodQueryEvent query(Qt::ImHints);
    QCoreApplication::sendEvent(guard.data(), &query);
    if (!guard) return {};
    const QVariant value = query.value(Qt::ImHints);
    bool converted = false;
    const int hints = value.toInt(&converted);
    return {true, value.isValid() && converted, hints};
}
