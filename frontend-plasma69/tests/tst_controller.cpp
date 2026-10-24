// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// The 6.9 controller's rules against a fake text field and the REAL engine files (SWIPEKB_TEST_MODEL_DATA: an
// installed-layout folder with dict/, models/swipe-model.bin and language-packs/; no FUTO file is needed or used).
// Skipped (exit 77) when the fixture is absent.

#include "fakeeditor.h"
#include "languagetextrules.h"
#include "swipecontroller.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace
{
const char *const kRows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};

QVariantList qwertyKeys(const QString &extraLetters = {})
{
    QVariantList keys;
    for (int row = 0; row < 3; ++row) {
        const QString letters = QString::fromLatin1(kRows[row]) + (row == 0 ? extraLetters : QString());
        for (int i = 0; i < letters.size(); ++i) {
            keys << QVariantMap{{QStringLiteral("letter"), QString(letters[i])},
                                {QStringLiteral("x"), 40.0 * i + row * 20.0},
                                {QStringLiteral("y"), 60.0 * row},
                                {QStringLiteral("w"), 40.0},
                                {QStringLiteral("h"), 60.0}};
        }
    }
    return keys;
}

// A finger path through the centres of the word's keys (12 points per letter-to-letter segment).
QVariantList trace(const QString &word, const QVariantList &keys)
{
    QList<QPointF> centres;
    for (const QChar c : word) {
        for (const auto &k : keys) {
            const auto m = k.toMap();
            if (m.value(QStringLiteral("letter")).toString() == QString(c)) {
                centres << QPointF(m[QStringLiteral("x")].toDouble() + 20, m[QStringLiteral("y")].toDouble() + 30);
            }
        }
    }
    QVariantList points;
    double t = 0;
    for (int i = 0; i + 1 < centres.size(); ++i) {
        for (int s = 0; s < 12; ++s) {
            const QPointF p = centres[i] + (centres[i + 1] - centres[i]) * (s / 12.0);
            points << QVariantMap{{QStringLiteral("x"), p.x()}, {QStringLiteral("y"), p.y()}, {QStringLiteral("t"), t}};
            t += 8;
        }
    }
    points << QVariantMap{{QStringLiteral("x"), centres.last().x()}, {QStringLiteral("y"), centres.last().y()}, {QStringLiteral("t"), t}};
    return points;
}
} // namespace

class ControllerTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_data;
    FakeEditor *m_editor = nullptr;
    SwipeController *m_controller = nullptr;

    void make(const QString &locale = QStringLiteral("en_US"))
    {
        delete m_controller;
        delete m_editor;
        m_editor = new FakeEditor;
        m_editor->m_locale = locale;
        m_controller = new SwipeController(m_editor);
    }
    void swipe(const QString &word)
    {
        const auto keys = qwertyKeys();
        const int generation = m_controller->beginSwipe();
        QVERIFY(generation >= 0);
        m_controller->finishSwipe(trace(word, keys), keys, generation);
    }
    void type(const QString &letters)
    {
        for (const QChar c : letters) {
            m_editor->tapThrough(m_controller, c == QLatin1Char(' ') ? Qt::Key_Space : Qt::Key_A, QString(c));
        }
    }
    bool waitForBar(const std::function<bool(const QStringList &)> &done)
    {
        return QTest::qWaitFor(
            [&] {
                return done(m_controller->candidates());
            },
            5000);
    }

private Q_SLOTS:
    void initTestCase()
    {
        const QString fixture = qEnvironmentVariable("SWIPEKB_TEST_MODEL_DATA");
        if (fixture.isEmpty() || !QFile::exists(fixture + QStringLiteral("/dict/en_swipe.combined"))) {
            QSKIP("SWIPEKB_TEST_MODEL_DATA not set: no engine files to test against");
        }
        qputenv("SWIPEKB_MODEL_DATA", fixture.toUtf8());
        qputenv("SWIPEKB_DATA", m_data.path().toUtf8());
    }
    void init()
    {
        make();
        QVERIFY2(m_controller->ready(), qPrintable(m_controller->diagnostics().value(QStringLiteral("unavailableReason")).toString()));
    }
    void cleanup()
    {
        delete m_controller;
        delete m_editor;
        m_controller = nullptr;
        m_editor = nullptr;
    }

    void usesOnlyOpenFiles()
    {
        // WHY: the 6.9 front end must never use FUTO. The fixture holds no FUTO model, and the engine still answers.
        QCOMPARE(m_controller->diagnostics().value(QStringLiteral("swipeEngine")).toString(), QStringLiteral("own"));
        QVERIFY(m_controller->diagnostics().value(QStringLiteral("nextWord")).toBool());
    }

    void swipeTypesAWord()
    {
        swipe(QStringLiteral("hello"));
        QCOMPARE(m_editor->m_text, QStringLiteral("hello"));
        QVERIFY(!m_controller->candidates().isEmpty()); // the swipe's other guesses
        QVERIFY(m_controller->undoArmed());
        // A second swipe gets its automatic space.
        swipe(QStringLiteral("world"));
        QCOMPARE(m_editor->m_text, QStringLiteral("hello world"));
    }

    void swipeIsCapitalizedByShiftAndReleasesIt()
    {
        m_editor->m_upper = true;
        swipe(QStringLiteral("hello"));
        QCOMPARE(m_editor->m_text, QStringLiteral("Hello"));
        QVERIFY(!m_editor->m_upper);
    }

    void appThatReportsLateGetsOneSpaceAndTheSentence()
    {
        // WHY (found on the real Plasma 6.9 dev desktop, 2026-10-24): Firefox reports each edit one step late. A swipe
        // starts as a press on its first key, and right after a swipe that press types the new word's space; the
        // swipe then still saw the old text, added a second space ("they  we're"), and lost the previous word, so
        // "were" became "we're". Like Firefox, this field also does not let the keyboard read it (owned words only).
        m_editor->m_lagging = true;
        m_editor->setHints(Qt::ImhNoPredictiveText);
        const auto swipeFromKey = [&](const QString &word) {
            // What the real layer does: the finger lands on the first key (reported as a press), then travels.
            QMetaObject::invokeMethod(m_controller, "keyPressed", Q_ARG(int, Qt::Key_A), Q_ARG(QString, word.left(1)));
            swipe(word);
        };
        swipeFromKey(QStringLiteral("they"));
        swipeFromKey(QStringLiteral("were"));
        QCOMPARE(m_editor->m_text, QStringLiteral("they were"));
        swipeFromKey(QStringLiteral("here"));
        QCOMPARE(m_editor->m_text, QStringLiteral("they were here"));
    }

    void tapTypesNormallyAndGetsSpaceAfterSwipe()
    {
        swipe(QStringLiteral("hello"));
        type(QStringLiteral("a"));
        // Gboard-style: a tapped letter right after a swiped word starts a new word.
        QCOMPARE(m_editor->m_text, QStringLiteral("hello a"));
        QVERIFY(!m_controller->undoArmed());
    }

    void pickingAnAlternativeReplacesTheSwipedWord()
    {
        swipe(QStringLiteral("hello"));
        const auto alternatives = m_controller->candidates();
        QVERIFY(!alternatives.isEmpty());
        m_controller->pressCandidate(m_controller->selectionToken());
        m_controller->selectCandidate(0);
        QCOMPARE(m_editor->m_text, alternatives.first());
    }

    void backspaceAfterSwipeErasesTheWord()
    {
        m_editor->setField(QStringLiteral("say "));
        swipe(QStringLiteral("hello"));
        QCOMPARE(m_editor->m_text, QStringLiteral("say hello"));
        QVERIFY(m_controller->undoArmed());
        m_controller->claimedKey(Qt::Key_Backspace, QString());
        QCOMPARE(m_editor->m_text, QStringLiteral("say "));
        // Not armed any more: the next claimed Backspace is an ordinary one.
        m_controller->claimedKey(Qt::Key_Backspace, QString());
        QCOMPARE(m_editor->m_text, QStringLiteral("say"));
    }

    // WHY: the built-in capitals list (owner decision 2026-10-23) reaches the 6.9 front end too: names and days keep
    // their capital mid-sentence, ordinary words stay lower case.
    void namesKeepTheirCapital()
    {
        m_editor->setField(QStringLiteral("we went to "));
        swipe(QStringLiteral("paris"));
        QCOMPARE(m_editor->m_text, QStringLiteral("we went to Paris"));
        m_editor->setField(QStringLiteral("in the "));
        swipe(QStringLiteral("hall"));
        QCOMPARE(m_editor->m_text, QStringLiteral("in the hall"));
        m_editor->setField(QStringLiteral("see you on "));
        type(QStringLiteral("mond"));
        QVERIFY(waitForBar([](const QStringList &bar) { return bar.contains(QStringLiteral("Monday")); }));
    }

    void completionAppearsAndApplies()
    {
        type(QStringLiteral("hel"));
        QVERIFY(waitForBar([](const QStringList &bar) {
            return bar.size() == 3;
        }));
        const auto bar = m_controller->candidates();
        QVERIFY2(bar.contains(QStringLiteral("hello")) || bar.contains(QStringLiteral("help")), qPrintable(bar.join(QLatin1Char(','))));
        const int index = bar.indexOf(QStringLiteral("hello")) >= 0 ? int(bar.indexOf(QStringLiteral("hello"))) : int(bar.indexOf(QStringLiteral("help")));
        m_controller->pressCandidate(m_controller->selectionToken());
        m_controller->selectCandidate(index);
        QCOMPARE(m_editor->m_text, bar.at(index) + QLatin1Char(' '));
    }

    void completionCorrectsOneWrongLetter()
    {
        type(QStringLiteral("hrllo"));
        QVERIFY(waitForBar([](const QStringList &bar) {
            return bar.contains(QStringLiteral("hello"));
        }));
    }

    void nextWordAfterSpace()
    {
        type(QStringLiteral("thank "));
        QVERIFY(waitForBar([](const QStringList &bar) {
            return !bar.isEmpty();
        }));
        QVERIFY2(m_controller->candidates().contains(QStringLiteral("you")), qPrintable(m_controller->candidates().join(QLatin1Char(','))));
        m_controller->pressCandidate(m_controller->selectionToken());
        m_controller->selectCandidate(int(m_controller->candidates().indexOf(QStringLiteral("you"))));
        QCOMPARE(m_editor->m_text, QStringLiteral("thank you "));
    }

    void noSwearWordOutOfTheBlue()
    {
        // WHY: the swear rule (packaging/blocked-words) applies to next words in this front end too.
        type(QStringLiteral("what the "));
        QVERIFY(waitForBar([](const QStringList &bar) {
            return !bar.isEmpty();
        }));
        for (const auto &w : m_controller->candidates()) {
            QVERIFY2(swipekb::offeredUnprompted(w, QStringLiteral("en")), qPrintable(w));
        }
    }

    void textingWordsAreSwipeable()
    {
        // "lol" is one of the built-in texting words; it must be reachable by completion.
        type(QStringLiteral("lo"));
        QVERIFY(waitForBar([](const QStringList &bar) {
            return bar.size() == 3;
        }));
        type(QStringLiteral("l"));
        QVERIFY(waitForBar([](const QStringList &bar) {
            return bar.contains(QStringLiteral("lol"));
        }));
    }

    void tapToSwapOffersAndSwaps()
    {
        m_editor->setField(QStringLiteral("the cat sat"));
        QTest::qWait(300); // the field settles (our own edits are told apart from taps by a short settle time)
        m_editor->moveCursor(5); // the person taps inside "cat"
        QVERIFY(waitForBar([](const QStringList &bar) {
            return !bar.isEmpty();
        }));
        const auto bar = m_controller->candidates();
        QVERIFY(!bar.contains(QStringLiteral("cat")));
        m_controller->pressCandidate(m_controller->selectionToken());
        m_controller->selectCandidate(0);
        QCOMPARE(m_editor->m_text, QStringLiteral("the ") + bar.first() + QStringLiteral(" sat"));
    }

    void tapToSwapNeedsAnAppThatAllowsCorrection()
    {
        // WHY: without the app's completion/correction hints the keyboard may not read the field (stock rule), so
        // text it did not type itself never gets alternatives.
        m_editor->setHints(Qt::ImhNoPredictiveText);
        m_editor->setField(QStringLiteral("the cat sat"));
        QTest::qWait(300);
        m_editor->moveCursor(5);
        QTest::qWait(300);
        QVERIFY(m_controller->candidates().isEmpty());
    }

    void tapToSwapOffersTheKeyboardsOwnWordsInUnreadableFields()
    {
        // WHY (Destin): tapping back into a word the keyboard itself typed must offer alternatives even where the
        // field may not be read; an outside edit ends that.
        m_editor->setHints(Qt::ImhNoPredictiveText);
        m_editor->setField(QStringLiteral("we "));
        QTest::qWait(300);
        swipe(QStringLiteral("hello"));
        const QString text = m_editor->m_text;
        QVERIFY2(text.startsWith(QStringLiteral("we ")) && text.size() > 5, qPrintable(text));
        QTest::qWait(400); // our own edit settles
        m_editor->moveCursor(5); // inside the swiped word
        QVERIFY2(waitForBar([](const QStringList &bar) {
                     return !bar.isEmpty();
                 }),
                 "no alternatives for the keyboard's own word");
        m_editor->moveCursor(1); // "we" was already there: never offered
        QTest::qWait(300);
        QVERIFY(m_controller->candidates().isEmpty());
        m_editor->setField(text + QStringLiteral("there")); // someone else types after it
        QTest::qWait(400);
        m_editor->moveCursor(5);
        QTest::qWait(300);
        QVERIFY(m_controller->candidates().isEmpty());
    }

    void passwordFieldDisablesEverything()
    {
        m_editor->setHints(Qt::ImhSensitiveData | Qt::ImhHiddenText | Qt::ImhNoPredictiveText);
        QVERIFY(!m_controller->fieldAllowsSwipe());
        QCOMPARE(m_controller->beginSwipe(), -1);
        type(QStringLiteral("hel"));
        QTest::qWait(300);
        QVERIFY(m_controller->candidates().isEmpty());
        type(QStringLiteral(" "));
        QTest::qWait(300);
        QVERIFY(m_controller->candidates().isEmpty());
        // A swipe that began before the field turned private is refused at release.
        m_editor->setHints({});
        const auto keys = qwertyKeys();
        const int generation = m_controller->beginSwipe();
        QVERIFY(generation >= 0);
        m_editor->setHints(Qt::ImhSensitiveData);
        m_controller->finishSwipe(trace(QStringLiteral("hello"), keys), keys, generation);
        QCOMPARE(m_editor->m_text, QStringLiteral("hel "));
    }

    void emailFieldHasNoPredictionButSwipes()
    {
        m_editor->setHints(Qt::ImhEmailCharactersOnly);
        type(QStringLiteral("hel"));
        QTest::qWait(300);
        QVERIFY(m_controller->candidates().isEmpty());
        QVERIFY(m_controller->fieldAllowsSwipe());
    }

    void languageSwitchFollowsTheLayout()
    {
        QSignalSpy ready(m_controller, &SwipeController::readyChanged);
        m_editor->m_locale = QStringLiteral("es_ES");
        Q_EMIT m_editor->localeChanged();
        QCOMPARE(m_controller->language(), QStringLiteral("es"));
        QVERIFY(m_controller->ready());
        QVERIFY(!ready.isEmpty());
        m_editor->m_locale = QStringLiteral("de_DE");
        Q_EMIT m_editor->localeChanged();
        QCOMPARE(m_controller->language(), QStringLiteral("de"));
        // Accented letters without a key are swiped through their base key (ü through u), like the legacy front end;
        // a layout missing a base letter stays tap-only.
        QVERIFY(m_controller->readyForKeys(qwertyKeys()));
        QVERIFY(m_controller->readyForKeys(qwertyKeys(QStringLiteral("üöä"))));
        QVariantList noQ = qwertyKeys();
        noQ.removeFirst();
        QVERIFY(!m_controller->readyForKeys(noQ));
        m_editor->m_locale = QStringLiteral("ru_RU");
        Q_EMIT m_editor->localeChanged();
        QVERIFY(!m_controller->ready());
        QCOMPARE(m_controller->beginSwipe(), -1);
    }

    void replacementCountsUtf8Bytes()
    {
        // WHY: input-method-v1 deletes BYTES. Swapping "café" (4 characters, 5 bytes) right after the person taps it must
        // delete exactly its 5 bytes, not 4.
        m_editor->setField(QStringLiteral("the café is"));
        QTest::qWait(300);
        m_editor->moveCursor(8); // right after "café"
        QVERIFY(waitForBar([](const QStringList &bar) {
            return !bar.isEmpty();
        }));
        const QString alternative = m_controller->candidates().first();
        m_controller->pressCandidate(m_controller->selectionToken());
        m_controller->selectCandidate(0);
        QCOMPARE(m_editor->m_text, QStringLiteral("the ") + alternative + QStringLiteral(" is"));
        QVERIFY2(m_editor->log.last().endsWith(QStringLiteral(" -5 5")), qPrintable(m_editor->log.last()));
    }

    void hiddenTextAppsNeverDeleteBlind()
    {
        // An app that reports no text: swipes still type, but nothing is deleted that we cannot see.
        m_editor->m_hidden = true;
        swipe(QStringLiteral("hello"));
        swipe(QStringLiteral("world"));
        QCOMPARE(m_editor->m_text, QStringLiteral("hello world"));
        for (const auto &line : std::as_const(m_editor->log)) {
            QVERIFY2(line.endsWith(QStringLiteral(" 0 0")), qPrintable(line));
        }
    }

    void spacebarSlideMovesTheCursorAndEndsTheSwipe()
    {
        swipe(QStringLiteral("hello"));
        QVERIFY(m_controller->undoArmed());
        m_controller->slideCursor(-3);
        QCOMPARE(m_editor->m_cursor, 2);
        QCOMPARE(m_editor->m_text, QStringLiteral("hello")); // no space, no letter
        QVERIFY(!m_controller->undoArmed()); // the swiped word's Backspace undo ends, like an arrow key
        m_controller->slideCursor(+100); // a huge jump is capped at 30 characters
        QCOMPARE(m_editor->sentKeys, 3 + 30);
    }

    void spaceLearnsOnlyWhenTheSpaceIsTyped()
    {
        // WHY: a press on Space that becomes a cursor slide types no space, so it must not finish (learn) the word.
        QFile flag(m_data.filePath(QStringLiteral("learn-typing")));
        QVERIFY(flag.open(QIODevice::WriteOnly));
        flag.close();
        make();
        for (int i = 0; i < 2; ++i) {
            type(QStringLiteral("quorbix"));
            m_controller->keyPressed(Qt::Key_Space, QStringLiteral(" ")); // pressed ...
            m_controller->slideCursor(-1);                                 // ... but it became a slide
            m_editor->setField(QString());
        }
        type(QStringLiteral("quor"));
        QTest::qWait(500);
        QVERIFY2(!m_controller->candidates().contains(QStringLiteral("quorbix")), qPrintable(m_controller->candidates().join(QLatin1Char(','))));
        QFile::remove(m_data.filePath(QStringLiteral("learn-typing")));
    }

    void learningUsesTheSameStore()
    {
        QFile flag(m_data.filePath(QStringLiteral("learn-typing")));
        QVERIFY(flag.open(QIODevice::WriteOnly));
        flag.close();
        make();
        type(QStringLiteral("zorblax "));
        type(QStringLiteral("zorblax "));
        delete m_controller; // flushes the store
        m_controller = nullptr;
        QVERIFY(QFile::exists(m_data.filePath(QStringLiteral("learning/en.swklearn"))));
        make();
        type(QStringLiteral("zorb"));
        QVERIFY(waitForBar([](const QStringList &bar) {
            return bar.contains(QStringLiteral("zorblax"));
        }));
        QFile::remove(m_data.filePath(QStringLiteral("learn-typing")));
    }
};

QTEST_GUILESS_MAIN(ControllerTest)
#include "tst_controller.moc"
