// Private diagnostic only: public event query must neither refresh a cache nor trust a deleted proxy.
#include <QGuiApplication>
#include <QInputMethodQueryEvent>
#include <QQuickItem>
#include <QtTest>
#include <functional>
#include "livehintsquery.h"

class Proxy : public QQuickItem {
public:
    Proxy() { setFlag(ItemAcceptsInputMethod, true); }
    QVariant hints = int(Qt::ImhNone);
    std::function<void()> duringQuery;
    bool event(QEvent *event) override {
        if (event->type() != QEvent::InputMethodQuery) return QQuickItem::event(event);
        auto *query = static_cast<QInputMethodQueryEvent *>(event);
        if (query->queries() != Qt::ImHints) qFatal("Diagnostic queried more than flags");
        query->setValue(Qt::ImHints, hints);
        if (duringQuery) duringQuery();
        return true;
    }
};
class LiveHintsTest : public QObject {
    Q_OBJECT
private slots:
    void staleCacheAndPublicLiveQuery() {
        Proxy proxy;
        const int cached = int(Qt::ImhNone);
        auto ordinary = queryLiveHints(&proxy);
        QVERIFY(ordinary.valid); QVERIFY(ordinary.alive);
        QCOMPARE(ordinary.hints, cached);
        proxy.hints = int(Qt::ImhSensitiveData | Qt::ImhHiddenText);
        auto changed = queryLiveHints(&proxy);
        QVERIFY(changed.valid); QVERIFY(changed.alive);
        QVERIFY(changed.hints & Qt::ImhSensitiveData);
        QCOMPARE(cached, int(Qt::ImhNone)); // observation never writes/refreshes the synthetic cache
    }
    void invalidAndAbsentQuery() {
        QVERIFY(!queryLiveHints(nullptr).valid);
        Proxy proxy; proxy.hints = QVariant();
        QVERIFY(!queryLiveHints(&proxy).valid);
        proxy.hints = QStringLiteral("not a flags value");
        QVERIFY(!queryLiveHints(&proxy).valid);
    }
    void reentrancyAndLifetime() {
        int generation = 0;
        Proxy proxy; proxy.duringQuery = [&] { ++generation; };
        QVERIFY(queryLiveHints(&proxy).valid);
        QCOMPARE(generation, 1); // caller can detect reset/generation mutation around the public event
        auto *doomed = new Proxy;
        doomed->duringQuery = [doomed] { delete doomed; };
        auto dead = queryLiveHints(doomed);
        QVERIFY(!dead.alive); QVERIFY(!dead.valid);
    }
};
QTEST_MAIN(LiveHintsTest)
#include "tst_livehintsquery.moc"
