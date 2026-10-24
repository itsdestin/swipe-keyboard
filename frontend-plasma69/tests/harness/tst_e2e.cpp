// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Swipe Keyboard's 6.9 front end inside KDE's REAL keyboard core (plasma-keyboard master, built unmodified by
// CMakeLists.txt here), driven by synthesized touches, offscreen. A fake Wayland backend plays the application:
// it applies commits and byte-based deletions to its text and echoes the result, like KWin + an app do.
//
// Covers: stock behaviour without our packages, our packages winning the lookup, tap, swipe, suggestion row,
// completion, tap-to-swap, whole-word Backspace, password fields, language switching, symbol pages and the
// compatibility check of every KDE internal we rely on.

#include "inputbackend.h"
#include "inputengine.h"
#include "keyboardcontroller.h"
#include "keyboardpackageresolver.h"
#include "virtualkeyboardcontext.h"

#include <KLocalizedQmlContext>
#include <KLocalizedString>

#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <memory>

class FakeBackend : public InputBackend
{
    Q_OBJECT
public:
    QByteArray text;
    uint32_t cursor = 0;
    Qt::InputMethodHints hints;
    QStringList log;
    bool m_deletePending = false;

    bool isActive() const override
    {
        return true;
    }
    Qt::InputMethodHints inputMethodHints() const override
    {
        return hints;
    }
    QString surroundingText() const override
    {
        return QString::fromUtf8(text);
    }
    uint32_t cursorPositionUtf8() const override
    {
        return cursor;
    }
    uint32_t anchorPositionUtf8() const override
    {
        return cursor;
    }
    void setPreeditText(const QString &t) override
    {
        if (!t.isEmpty()) {
            log << QStringLiteral("preedit ") + t;
        }
    }
    void commitText(const QString &t) override
    {
        log << QStringLiteral("commit '%1'").arg(t);
        const QByteArray bytes = t.toUtf8();
        text.insert(cursor, bytes);
        cursor += bytes.size();
        m_deletePending = false;
        Q_EMIT surroundingTextChanged();
    }
    void deleteSurroundingText(int index, int length) override
    {
        // input-method-v1: BYTES, relative to the cursor.
        log << QStringLiteral("delete %1 %2").arg(index).arg(length);
        const int start = int(cursor) + index;
        QVERIFY(start >= 0 && start + length <= text.size());
        text.remove(start, length);
        if (start < int(cursor)) {
            cursor = uint32_t(qMax(start, int(cursor) - length));
        }
        // WHY not at once: KWin applies delete_surrounding_text together with the commit_string that follows it,
        // and the app reports one new state. A lone deletion is reported on its own a moment later.
        m_deletePending = true;
        QTimer::singleShot(0, this, [this] {
            if (m_deletePending) {
                m_deletePending = false;
                Q_EMIT surroundingTextChanged();
            }
        });
    }
    bool sendKeyClick(int key) override
    {
        log << QStringLiteral("key %1").arg(key);
        if (key == Qt::Key_Backspace && cursor > 0) {
            int start = int(cursor) - 1;
            while (start > 0 && (uchar(text.at(start)) & 0xC0) == 0x80) {
                --start;
            }
            text.remove(start, int(cursor) - start);
            cursor = uint32_t(start);
        } else if (key == Qt::Key_Space) {
            text.insert(cursor, ' ');
            ++cursor;
        } else if (key == Qt::Key_Left && cursor > 0) {
            do {
                --cursor;
            } while (cursor > 0 && (uchar(text.at(cursor)) & 0xC0) == 0x80);
        } else if (key == Qt::Key_Right && cursor < uint32_t(text.size())) {
            do {
                ++cursor;
            } while (cursor < uint32_t(text.size()) && (uchar(text.at(cursor)) & 0xC0) == 0x80);
        } else if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            text.insert(cursor, '\n');
            ++cursor;
        }
        Q_EMIT surroundingTextChanged();
        return true;
    }
    bool sendKeyPressed(int key, bool pressed) override
    {
        setKeyPressed(key, pressed);
        return true;
    }

    void setField(const QString &t, int cursorChars = -1)
    {
        text = t.toUtf8();
        cursor = uint32_t(cursorChars < 0 ? text.size() : t.left(cursorChars).toUtf8().size());
        Q_EMIT surroundingTextChanged();
    }
    void setHints(Qt::InputMethodHints h)
    {
        hints = h;
        Q_EMIT inputMethodHintsChanged();
    }
};

// WHY a macro: a failed wait must end the test function, not let it run on with a null controller.
#define WAIT_ATTACHED()                                                                                                \
    do {                                                                                                               \
        waitAttached();                                                                                                \
        if (QTest::currentTestFailed()) {                                                                              \
            return;                                                                                                    \
        }                                                                                                              \
    } while (false)

class E2E : public QObject
{
    Q_OBJECT
    QTemporaryDir m_temp;
    QString m_overlay, m_packages;
    QQmlEngine *m_engine = nullptr;
    QQuickWindow *m_window = nullptr;
    FakeBackend *m_backend = nullptr;
    VirtualKeyboardContext *m_context = nullptr;
    QPointingDevice *m_touch = nullptr;

    QString text() const
    {
        return QString::fromUtf8(m_backend->text);
    }

    void start(bool withOverlay, bool withModule = true)
    {
        stop();
        // WHY before the context: KDE's package resolver indexes the data folders when it is created.
        qputenv("XDG_DATA_DIRS", ((withOverlay ? m_overlay + QLatin1Char(':') : QString()) + QStringLiteral(PK_PREFIX "/share")).toUtf8());
        m_backend = new FakeBackend;
        m_context = new VirtualKeyboardContext(m_backend);
        m_engine = new QQmlEngine;
        // WHY: KDE's QML calls i18n(); the application installs this context the same way.
        KLocalization::setupLocalizedContext(m_engine);
        if (withModule) {
            m_engine->addImportPath(QStringLiteral(SWIPEKB_QML_DIR));
        }
        m_engine->addImportPath(QStringLiteral(PK_QML_DIR));
        QQmlComponent component(m_engine, QUrl::fromLocalFile(QStringLiteral(PK_HOST_QML)));
        QObject *object = component.createWithInitialProperties({{QStringLiteral("virtualKeyboardContext"), QVariant::fromValue(m_context)}});
        QVERIFY2(object, qPrintable(component.errorString()));
        m_window = qobject_cast<QQuickWindow *>(object);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        QTRY_VERIFY_WITH_TIMEOUT(key(QStringLiteral("q")), 5000);
    }

    void stop()
    {
        delete m_window;
        m_window = nullptr;
        delete m_engine;
        m_engine = nullptr;
        delete m_context; // owns the backend through its input engine
        m_context = nullptr;
        m_backend = nullptr;
    }

    QList<QQuickItem *> items(QQuickItem *from = nullptr) const
    {
        QList<QQuickItem *> out;
        QList<QQuickItem *> todo{from ? from : m_window->contentItem()};
        while (!todo.isEmpty()) {
            QQuickItem *item = todo.takeFirst();
            out << item;
            todo << item->childItems();
        }
        return out;
    }

    QQuickItem *key(const QString &label) const
    {
        for (QQuickItem *item : items()) {
            if (item->isVisible() && item->metaObject()->indexOfProperty("commitText") >= 0 && item->property("text").toString() == label) {
                return item;
            }
        }
        return nullptr;
    }

    QQuickItem *backspace() const
    {
        for (QQuickItem *item : items()) {
            if (item->isVisible() && item->metaObject()->indexOfProperty("commitText") >= 0 && item->property("key").toInt() == Qt::Key_Backspace) {
                return item;
            }
        }
        return nullptr;
    }

    QQuickItem *named(const QString &name) const
    {
        for (QQuickItem *item : items()) {
            if (item->objectName() == name && item->isVisible()) {
                return item;
            }
        }
        return nullptr;
    }

    QStringList row() const
    {
        QStringList out;
        for (int i = 0; i < 3; ++i) {
            if (QQuickItem *cell = named(QStringLiteral("swipeSuggestion%1").arg(i))) {
                out << cell->property("word").toString();
            }
        }
        return out;
    }

    QObject *controller() const
    {
        QQuickItem *strip = named(QStringLiteral("swipeSuggestionStrip"));
        return strip ? strip->property("controller").value<QObject *>() : nullptr;
    }

    QPoint centre(QQuickItem *item) const
    {
        return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
    }

    void tap(QQuickItem *item)
    {
        QVERIFY(item);
        const QPoint p = centre(item);
        QTest::touchEvent(m_window, m_touch).press(0, p);
        QTest::qWait(30);
        QTest::touchEvent(m_window, m_touch).release(0, p);
        QTest::qWait(30);
    }

    void tapText(const QString &letters)
    {
        for (const QChar c : letters) {
            tap(key(QString(c)));
        }
    }

    void swipe(const QString &word)
    {
        QList<QPoint> centres;
        for (const QChar c : word) {
            QQuickItem *k = key(QString(c));
            QVERIFY2(k, qPrintable(QStringLiteral("no key ") + c));
            centres << centre(k);
        }
        QTest::touchEvent(m_window, m_touch).press(0, centres.first());
        QTest::qWait(10);
        for (int i = 0; i + 1 < centres.size(); ++i) {
            for (int s = 1; s <= 10; ++s) {
                const QPoint p = centres[i] + (centres[i + 1] - centres[i]) * s / 10;
                QTest::touchEvent(m_window, m_touch).move(0, p);
                QTest::qWait(8);
            }
        }
        QTest::touchEvent(m_window, m_touch).release(0, centres.last());
        QTest::qWait(30);
    }

    void waitAttached()
    {
        QTRY_VERIFY_WITH_TIMEOUT(controller(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(controller()->property("ready").toBool(), 10000);
    }

private Q_SLOTS:
    void initTestCase()
    {
        const QString fixture = qEnvironmentVariable("SWIPEKB_TEST_MODEL_DATA");
        if (fixture.isEmpty()) {
            QSKIP("SWIPEKB_TEST_MODEL_DATA not set");
        }
        KLocalizedString::setApplicationDomain("plasma-keyboard");
        m_touch = QTest::createTouchDevice();
        m_overlay = m_temp.filePath(QStringLiteral("overlay"));
        // The launcher's generator, run exactly as the launcher runs it.
        QProcess generator;
        generator.start(QStringLiteral("python3"), {QStringLiteral(SWIPEKB_GENERATOR), QStringLiteral(PK_PREFIX "/share"), m_overlay});
        QVERIFY(generator.waitForFinished(30000));
        QVERIFY2(generator.exitCode() == 0, generator.readAllStandardError().constData());
        m_packages = QString::fromUtf8(generator.readAllStandardOutput()).trimmed();
        QVERIFY(m_packages.contains(QStringLiteral("org.kde.plasma.keyboard.en")));
        qputenv("SWIPEKB_PLASMA69_PACKAGES", m_packages.toUtf8());
        qputenv("XDG_DATA_HOME", m_temp.filePath(QStringLiteral("home")).toUtf8());
        qputenv("SWIPEKB_MODEL_DATA", fixture.toUtf8());
        qputenv("SWIPEKB_DATA", m_temp.filePath(QStringLiteral("data")).toUtf8());
    }
    void cleanup()
    {
        stop();
    }

    void stockKeyboardWithoutOurPackages()
    {
        start(false);
        QVERIFY(!named(QStringLiteral("swipeSuggestionStrip")));
        swipe(QStringLiteral("hello"));
        // Baseline: KDE's keyboard types just the first key of a slide.
        QCOMPARE(text().size(), 1);
    }

    void missingModuleLeavesKdesKeyboard()
    {
        // Break-check of the wrappers: our packages are first but our QML module cannot load. The layout must still
        // be KDE's and type normally (no blank keyboard).
        start(true, false);
        QTest::qWait(200);
        QVERIFY(!named(QStringLiteral("swipeSuggestionStrip")));
        tap(key(QStringLiteral("q")));
        QCOMPARE(text(), QStringLiteral("Q"));
    }

    void ourPackageWinsAndAttaches()
    {
        start(true);
        WAIT_ATTACHED();
        auto *resolver = m_context->keyboardPackageResolver();
        QUrl url;
        // WHY both signatures: master has layoutUrl(id, type); MR !179 has layoutUrl(id).
        if (resolver->metaObject()->indexOfMethod("layoutUrl(QString,QString)") >= 0) {
            QVERIFY(QMetaObject::invokeMethod(resolver, "layoutUrl", Q_RETURN_ARG(QUrl, url), Q_ARG(QString, QStringLiteral("org.kde.plasma.keyboard.en/mobile-qwerty")), Q_ARG(QString, QStringLiteral("main"))));
        } else {
            QVERIFY(QMetaObject::invokeMethod(resolver, "layoutUrl", Q_RETURN_ARG(QUrl, url), Q_ARG(QString, QStringLiteral("org.kde.plasma.keyboard.en/mobile-qwerty"))));
        }
        QVERIFY2(url.toLocalFile().startsWith(m_overlay), qPrintable(url.toString()));
        QVERIFY2(url.fileName().startsWith(QStringLiteral("SwipeKeyboard")), qPrintable(url.toString()));
        // Our row sits above the keys and the keys keep their height.
        QQuickItem *strip = named(QStringLiteral("swipeSuggestionStrip"));
        QVERIFY(strip->height() > 10);
        QVERIFY(strip->mapToScene(QPointF(0, strip->height())).y() <= centre(key(QStringLiteral("q"))).y());
    }

    void tapTypesNormally()
    {
        start(true);
        WAIT_ATTACHED();
        tap(key(QStringLiteral("q")));
        QCOMPARE(text(), QStringLiteral("Q")); // KDE's own sentence capital, untouched
        tap(key(QStringLiteral("w")));
        QCOMPARE(text(), QStringLiteral("Qw"));
    }

    void swipeProducesAWordAndAlternatives()
    {
        start(true);
        WAIT_ATTACHED();
        swipe(QStringLiteral("hello"));
        QTRY_COMPARE(text(), QStringLiteral("Hello"));
        QTRY_VERIFY(!row().join(QString()).isEmpty());
        swipe(QStringLiteral("world"));
        QTRY_COMPARE(text(), QStringLiteral("Hello world"));
    }

    void suggestionRowAppliesAlternative()
    {
        start(true);
        WAIT_ATTACHED();
        swipe(QStringLiteral("hello"));
        QTRY_VERIFY(!row().value(0).isEmpty());
        const QString alternative = row().value(0);
        tap(named(QStringLiteral("swipeSuggestion0")));
        QTRY_COMPARE(text(), alternative.left(1).toUpper() + alternative.mid(1));
        QVERIFY2(m_backend->log.join(QLatin1Char('|')).contains(QStringLiteral("delete -5 5")), qPrintable(m_backend->log.join(QLatin1Char('|'))));
    }

    void wholeWordBackspace()
    {
        start(true);
        WAIT_ATTACHED();
        swipe(QStringLiteral("hello"));
        QTRY_COMPARE(text(), QStringLiteral("Hello"));
        QTRY_VERIFY(controller()->property("undoArmed").toBool());
        QTest::qWait(50); // the claim area appears
        tap(backspace());
        QTRY_COMPARE(text(), QString());
    }

    void completionWhileTapping()
    {
        start(true);
        WAIT_ATTACHED();
        tapText(QStringLiteral("hel"));
        QCOMPARE(text(), QStringLiteral("Hel"));
        QTRY_VERIFY_WITH_TIMEOUT(row().size() == 3 && !row().at(1).isEmpty(), 5000);
        const QString word = row().at(1);
        tap(named(QStringLiteral("swipeSuggestion1")));
        QTRY_COMPARE(text(), word + QLatin1Char(' '));
        // Next words follow the accepted word.
        QTRY_VERIFY_WITH_TIMEOUT(!row().join(QString()).isEmpty(), 5000);
    }

    void tapToSwap()
    {
        start(true);
        WAIT_ATTACHED();
        m_backend->setField(QStringLiteral("the cat sat"));
        QTest::qWait(300);
        m_backend->setField(QStringLiteral("the cat sat"), 5); // the person taps inside "cat"
        QTRY_VERIFY_WITH_TIMEOUT(!row().join(QString()).isEmpty(), 5000);
        const QStringList shown = row();
        int index = 0;
        while (shown.value(index).isEmpty()) {
            ++index;
        }
        tap(named(QStringLiteral("swipeSuggestion%1").arg(index)));
        QTRY_COMPARE(text(), QStringLiteral("the ") + shown.at(index) + QStringLiteral(" sat"));
    }

    void passwordFieldDisablesEverything()
    {
        start(true);
        WAIT_ATTACHED();
        m_backend->setHints(Qt::ImhSensitiveData | Qt::ImhHiddenText | Qt::ImhNoPredictiveText);
        swipe(QStringLiteral("hello"));
        QCOMPARE(text().size(), 1); // only KDE's own tap, exactly like stock
        tapText(QStringLiteral("el"));
        QTest::qWait(300);
        QVERIFY(row().join(QString()).isEmpty());
        QVERIFY(!m_backend->log.join(QLatin1Char('|')).contains(QStringLiteral("delete")));
    }

    void layoutSwitchChangesLanguage()
    {
        start(true);
        WAIT_ATTACHED();
        m_context->keyboardController()->setCurrentLayout(QStringLiteral("org.kde.plasma.keyboard.de_DE/mobile-qwertz"));
        QTRY_VERIFY_WITH_TIMEOUT(key(QStringLiteral("ü")), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(controller() ? controller()->property("language").toString() : QString(), QStringLiteral("de"), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(controller()->property("ready").toBool(), 10000);
        swipe(QStringLiteral("hallo"));
        QTRY_COMPARE(text(), QStringLiteral("Hallo"));
    }

    void languagesWeDoNotSupportStayStock()
    {
        start(true);
        WAIT_ATTACHED();
        m_context->keyboardController()->setCurrentLayout(QStringLiteral("org.kde.plasma.keyboard.ru_RU/mobile-jcuken"));
        QTRY_VERIFY_WITH_TIMEOUT(key(QStringLiteral("й")), 5000);
        QTest::qWait(200);
        QVERIFY(!named(QStringLiteral("swipeSuggestionStrip")));
        swipe(QStringLiteral("йцу"));
        QCOMPARE(text().size(), 1); // KDE's own behaviour
    }

    void symbolsPageHasNoSwipeButKeepsRow()
    {
        start(true);
        WAIT_ATTACHED();
        const qreal before = named(QStringLiteral("swipeSuggestionStrip"))->height();
        m_context->keyboardController()->setSymbolMode(true);
        QTRY_VERIFY_WITH_TIMEOUT(!key(QStringLiteral("q")), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(named(QStringLiteral("swipeSuggestionStrip")), 5000);
        QCOMPARE(named(QStringLiteral("swipeSuggestionStrip"))->height(), before);
        QQuickItem *one = key(QStringLiteral("1"));
        QQuickItem *three = key(QStringLiteral("3"));
        QVERIFY(one && three);
        QTest::touchEvent(m_window, m_touch).press(0, centre(one));
        for (int s = 1; s <= 10; ++s) {
            QTest::touchEvent(m_window, m_touch).move(0, centre(one) + (centre(three) - centre(one)) * s / 10);
            QTest::qWait(8);
        }
        QTest::touchEvent(m_window, m_touch).release(0, centre(three));
        QTest::qWait(30);
        QCOMPARE(text(), QStringLiteral("1")); // a slide on symbols types one key, as stock does
    }

    QQuickItem *spaceKey() const
    {
        for (QQuickItem *item : items()) {
            if (item->isVisible() && item->metaObject()->indexOfProperty("commitText") >= 0 && item->property("key").toInt() == Qt::Key_Space) {
                return item;
            }
        }
        return nullptr;
    }

    // Presses Space and slides `keys` letter-key widths sideways in `moves` steps, `msPerMove` apart on the layer's clock.
    void slideOnSpace(double keys, int moves, int msPerMove)
    {
        QQuickItem *space = spaceKey();
        QQuickItem *layer = named(QStringLiteral("swipeLayer"));
        QVERIFY(space && layer);
        const double unit = key(QStringLiteral("q"))->width();
        const QPoint start = centre(space);
        double clock = 1000;
        layer->setProperty("slideClockMs", clock);
        QTest::touchEvent(m_window, m_touch).press(0, start);
        QTest::qWait(10);
        for (int i = 1; i <= moves; ++i) {
            clock += msPerMove;
            layer->setProperty("slideClockMs", clock);
            QTest::touchEvent(m_window, m_touch).move(0, start + QPoint(int(keys * unit * i / moves), 0));
            QTest::qWait(5);
        }
        QTest::touchEvent(m_window, m_touch).release(0, start + QPoint(int(keys * unit), 0));
        QTest::qWait(30);
        layer->setProperty("slideClockMs", -1);
    }

    void spaceTapStillTypesSpace()
    {
        start(true);
        WAIT_ATTACHED();
        m_backend->setField(QStringLiteral("hello"));
        tap(spaceKey());
        QCOMPARE(text(), QStringLiteral("hello "));
        // A wobble under half a key is still a tap.
        slideOnSpace(0.3, 3, 40);
        QCOMPARE(text(), QStringLiteral("hello  "));
    }

    void spaceSlideMovesTheCursorWithSpeedUp()
    {
        start(true);
        WAIT_ATTACHED();
        const QString line = QStringLiteral("the quick brown fox jumps over the lazy dog");
        m_backend->setField(line);
        slideOnSpace(-1.5, 15, 60); // slow: 1.5 keys in 0.9 s
        QCOMPARE(text(), line); // no space typed
        const int slow = int(line.size()) - int(m_backend->cursor);
        QVERIFY2(slow >= 8 && slow <= 12, qPrintable(QString::number(slow))); // 0.5 key threshold, then 1 char per 0.1 key
        m_backend->setField(line);
        slideOnSpace(-1.5, 15, 4); // a flick: the same distance in 60 ms
        const int fast = int(line.size()) - int(m_backend->cursor);
        QVERIFY2(fast > slow + 5, qPrintable(QStringLiteral("slow %1 fast %2").arg(slow).arg(fast)));
        QCOMPARE(text(), line);
        m_backend->setField(line, 0);
        slideOnSpace(1.0, 10, 60); // right moves right
        QVERIFY(m_backend->cursor > 0);
    }

    void spaceSlideStepsAsideForKdesOwn()
    {
        // KDE's own slide (MR !69 or a successor) is recognised by its type or a cursor property on the Space key.
        start(true);
        WAIT_ATTACHED();
        QObject *hook = m_engine->singletonInstance<QObject *>("SwipeKeyboard69", "SwipeHook");
        bool kde = true;
        QVERIFY(QMetaObject::invokeMethod(hook, "kdeSlidesCursor", Q_RETURN_ARG(bool, kde), Q_ARG(QObject *, spaceKey())));
        QVERIFY(!kde); // today's KDE Space key has no slide
        QQmlComponent fake(m_engine);
        fake.setData("import QtQuick\nItem { property int key: Qt.Key_Space; property bool cursorMovementEnabled: true }", QUrl());
        std::unique_ptr<QObject> theirs(fake.create());
        QVERIFY(theirs);
        QVERIFY(QMetaObject::invokeMethod(hook, "kdeSlidesCursor", Q_RETURN_ARG(bool, kde), Q_ARG(QObject *, theirs.get())));
        QVERIFY(kde);
        // Stepping aside: with ours off, a slide on Space is KDE's business (today: it types a space).
        qputenv("SWIPEKB_SPACE_SLIDE", "off");
        m_backend->setField(QStringLiteral("hello"));
        slideOnSpace(-1.5, 15, 60);
        qunsetenv("SWIPEKB_SPACE_SLIDE");
        QCOMPARE(text(), QStringLiteral("hello "));
    }

    void kdeInternalsWeRelyOnArePresent()
    {
        // WHY: fails loudly here when KDE renames anything we reach into (see docs/plasma-6.9-front-end.md).
        start(true);
        WAIT_ATTACHED();
        QObject *hook = m_engine->singletonInstance<QObject *>("SwipeKeyboard69", "SwipeHook");
        QVERIFY(hook);
        QStringList missing;
        QQuickItem *anchor = key(QStringLiteral("q"));
        QVERIFY(QMetaObject::invokeMethod(hook, "missingInternals", Q_RETURN_ARG(QStringList, missing), Q_ARG(QObject *, anchor)));
        QVERIFY2(missing.isEmpty(), qPrintable(missing.join(QStringLiteral(", "))));
        // Each key we hook exposes the members the layer uses.
        for (const char *member : {"key", "text", "commitText", "pressedVisual"}) {
            QVERIFY2(anchor->metaObject()->indexOfProperty(member) >= 0, member);
        }
        QVERIFY(anchor->metaObject()->indexOfSignal("clicked()") >= 0);
        bool keyArea = false;
        for (QObject *child : anchor->children()) {
            keyArea = keyArea || (child->inherits("QQuickMultiPointTouchArea") && child->metaObject()->indexOfSignal("pressStarted(QVariant)") >= 0);
        }
        QVERIFY2(keyArea, "KeyMouseArea (MultiPointTouchArea with pressStarted) inside each key");
        QVERIFY(m_window->property("alternativeKeysPopupItem").value<QObject *>());
        QVERIFY(m_window->property("alternativeKeysPopupItem").value<QObject *>()->metaObject()->indexOfProperty("active") >= 0);
        // Break-check of the checker itself: an object that is not inside a keyboard is reported, never accepted.
        QObject stranger;
        QVERIFY(QMetaObject::invokeMethod(hook, "missingInternals", Q_RETURN_ARG(QStringList, missing), Q_ARG(QObject *, &stranger)));
        QVERIFY(!missing.isEmpty());
    }
};

QTEST_MAIN(E2E)
#include "tst_e2e.moc"
