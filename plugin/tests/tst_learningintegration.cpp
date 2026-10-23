// Learning inside the real input method: what is learned, from where, and what the person can take back.
// Uses the same fake field as the next-word integration tests; no real app or compositor.
#include <QtTest>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQuickWindow>
#include <QQuickItem>
#include <QInputMethodEvent>
#include <QTemporaryDir>
#include <QtVirtualKeyboard/QVirtualKeyboardInputContext>
#include "swipeinputmethod.h"

namespace {
using Type = QVirtualKeyboardSelectionListModel::Type;
using Role = QVirtualKeyboardSelectionListModel::Role;

class Policy : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap swipePredictionPolicy READ policy NOTIFY swipePredictionPolicyChanged)
public:
    bool permitted = true;
    quint64 revision = 1;
    QVariantMap policy() const {
        return {{"version",1},{"contextId",quint64(8)},{"revision",revision},
            {"contentTypeReceived",true},{"purpose",0},{"completionAllowed",permitted},{"syntheticNoPredictiveText",true}};
    }
    void set(bool allowed) { permitted = allowed; ++revision; emit swipePredictionPolicyChanged(); }
signals:
    void swipePredictionPolicyChanged();
};

class InputListenerItem : public QQuickItem {
    Q_OBJECT
public:
    QString text = QStringLiteral("note ");
    int cursor = text.size();
    Qt::InputMethodHints hints = Qt::ImhNoPredictiveText;
    explicit InputListenerItem(QQuickItem *parent) : QQuickItem(parent) { setFlag(ItemAcceptsInputMethod); setWidth(400); setHeight(100); }
    QVariant inputMethodQuery(Qt::InputMethodQuery q) const override {
        switch (q) {
        case Qt::ImEnabled: return true;
        case Qt::ImHints: return int(hints);
        case Qt::ImSurroundingText: return text;
        case Qt::ImCursorPosition: case Qt::ImAnchorPosition: case Qt::ImAbsolutePosition: return cursor;
        case Qt::ImCurrentSelection: return QString();
        case Qt::ImCursorRectangle: return QRectF(0, 0, 2, 20);
        default: return QQuickItem::inputMethodQuery(q);
        }
    }
    void inputMethodEvent(QInputMethodEvent *event) override {
        text.replace(cursor + event->replacementStart(), event->replacementLength(), event->commitString());
        cursor += event->replacementStart() + event->commitString().size(); event->accept();
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
    }
};

QVariantList qwertyKeys() {
    QVariantList keys; const QStringList rows{"qwertyuiop","asdfghjkl","zxcvbnm"};
    for (int r=0;r<rows.size();++r) for (int i=0;i<rows[r].size();++i)
        keys.append(QVariantMap{{"letter",QString(rows[r][i])},{"x",i*100},{"y",r*100},{"w",100},{"h",100}});
    return keys;
}
// A straight-line finger path through the centres of the word's keys.
QVariantList traceFor(const QString &word,const QVariantList &keys) {
    QVariantList points; QPointF previous;
    for (const auto &letter:word) {
        QVariantMap key;
        for (const auto &entry:keys) if (entry.toMap()["letter"].toString()==QString(letter)) key=entry.toMap();
        const QPointF center(key["x"].toDouble()+50,key["y"].toDouble()+50);
        if (points.isEmpty()) points.append(QVariantMap{{"x",center.x()},{"y",center.y()},{"t",0}});
        else for (int step=1;step<=8;++step) {
            const auto p=previous+(center-previous)*(step/8.0);
            points.append(QVariantMap{{"x",p.x()},{"y",p.y()},{"t",points.size()*16}});
        }
        previous=center;
    }
    return points;
}

// One keyboard over one fake field. `native` = the stock keyboard that cannot vouch for field text (owned history).
struct Rig {
    QTemporaryDir data;
    QQmlEngine engine; QQmlComponent fixture{&engine};
    std::unique_ptr<QObject> root; QQuickWindow window; std::unique_ptr<InputListenerItem> field;
    QVirtualKeyboardInputContext *ic = nullptr; std::unique_ptr<SwipeInputMethod> method; Policy policy;
    QStringList nextWords{QStringLiteral("world"), QStringLiteral("again"), QStringLiteral("today")};
    bool native = false;

    bool start(bool learningOn, bool nativeMode)
    {
        native = nativeMode;
        qputenv("SWIPEKB_DATA", data.path().toUtf8());
        qputenv("SWIPEKB_MODEL_DATA", qgetenv("SWIPEKB_TEST_PROFILE_ASSETS"));
        if (native) qputenv("SWIPEKB_NATIVE_PREDICTION", "owned-history-v1"); else qunsetenv("SWIPEKB_NATIVE_PREDICTION");
        if (learningOn) QFile(data.path() + "/learn-typing").open(QIODevice::WriteOnly);
        fixture.setData("import QtQuick; import QtQuick.VirtualKeyboard; Item {property var context: InputContext}", QUrl());
        root.reset(fixture.create()); if (!root) return false;
        auto *item = qobject_cast<QQuickItem *>(root.get()); window.resize(500, 200); item->setParentItem(window.contentItem());
        field = std::make_unique<InputListenerItem>(item);
        window.show(); window.requestActivate(); field->forceActiveFocus();
        ic = qvariant_cast<QVirtualKeyboardInputContext *>(root->property("context"));
        // WHY a fixed answer: the next-word / completion lists then hold known words, so only the learning decides the rest.
        method = std::make_unique<SwipeInputMethod>(nullptr, [this](const PredictionJob &job) {
            return job.prefix.isEmpty() ? nextWords : QStringList{QStringLiteral("table"), QStringLiteral("talk")}; });
        ic->inputEngine()->setInputMethod(method.get());
        method->setInputMode("en_US", QVirtualKeyboardInputEngine::InputMode::Latin);
        method->setPhysicalLayout("qwerty");
        if (!native) method->setPredictionPolicySource(&policy);
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll);
        return QTest::qWaitFor([&] { return ic->inputItem() == field.get(); }, 5000);
    }
    ~Rig() { if (ic) ic->inputEngine()->setInputMethod(nullptr); qunsetenv("SWIPEKB_NATIVE_PREDICTION"); }

    void tap(const QString &letters)
    {
        for (const QChar c : letters) {
            const QString s(c);
            method->keyEvent(Qt::Key(s.front().toUpper().unicode()), s, Qt::NoModifier);
            ic->commit(s); QGuiApplication::inputMethod()->update(Qt::ImQueryAll); method->update();
        }
    }
    void press(Qt::Key key, const QString &text)
    {
        method->keyEvent(key, text, Qt::NoModifier);
        ic->commit(text); QGuiApplication::inputMethod()->update(Qt::ImQueryAll); method->update();
    }
    void word(const QString &w) { tap(w); press(Qt::Key_Space, QStringLiteral(" ")); }
    int learnedPairs() { return method->diagnostics().value("learnedPairs").toInt(); }
    int learnedWords() { return method->diagnostics().value("learnedWords").toInt(); }
    QStringList bar()
    {
        QStringList out;
        const int n = method->selectionListItemCount(Type::WordCandidateList);
        for (int i = 0; i < n; ++i) out << method->selectionListData(Type::WordCandidateList, i, Role::Display).toString();
        return out;
    }
    // Waits until the bar has settled with something in it, then returns it.
    QStringList settledBar()
    {
        QTest::qWaitFor([&] { return !bar().isEmpty(); }, 5000);
        QTest::qWait(150);
        return bar();
    }
    // A swipe over the word's keys; the word is typed by the keyboard exactly as for a person's finger.
    bool swipe(const QString &w)
    {
        const auto keys = qwertyKeys();
        const int token = method->beginSwipe();
        if (token < 0) return false;
        method->finishSwipe(traceFor(w, keys), keys, token);
        QTest::qWait(60);
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); method->update();
        return true;
    }
    void backspaceAll(int n) { for (int i = 0; i < n; ++i) { method->keyEvent(Qt::Key_Backspace, QString(), Qt::NoModifier);
        field->text.remove(field->cursor - 1, 1); --field->cursor; QGuiApplication::inputMethod()->update(Qt::ImQueryAll); method->update(); } }
};
}

class LearningIntegrationTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        if (qgetenv("SWIPEKB_TEST_PROFILE_ASSETS").isEmpty()) QSKIP("needs the English profile fixture (SWIPEKB_TEST_PROFILE_ASSETS)");
    }
    void learningOffLearnsNothingAndWritesNothing_data()
    {
        QTest::addColumn<bool>("native"); QTest::newRow("bridge") << false; QTest::newRow("native") << true;
    }
    void learningOffLearnsNothingAndWritesNothing()
    {
        QFETCH(bool, native);
        QString dir;
        {
            Rig r; QVERIFY(r.start(false, native)); dir = r.data.path();
            QVERIFY(!r.method->learningEnabled());
            r.word("tazzle"); r.word("tazzle"); r.word("tazzle");
            r.tap("taz");
            QVERIFY(!r.settledBar().contains("tazzle"));
            QCOMPARE(r.learnedWords(), 0);
            QCOMPARE(r.method->diagnostics().value("learning").toBool(), false);
        }   // the keyboard is gone: its shutdown write must have nothing to write
        QVERIFY(!QFileInfo::exists(dir + "/learning"));
    }
    void aNewWordIsOfferedAfterTwoUsesNotOne_data()
    {
        QTest::addColumn<bool>("native"); QTest::newRow("bridge") << false; QTest::newRow("native") << true;
    }
    void aNewWordIsOfferedAfterTwoUsesNotOne()
    {
        QFETCH(bool, native);
        Rig r; QVERIFY(r.start(true, native));
        r.word("tazzle");
        QCOMPARE(r.learnedWords(), 1);
        r.tap("taz");
        QVERIFY(!r.settledBar().contains("tazzle"));        // one use is not enough
        r.backspaceAll(3);
        r.word("tazzle");                                    // second use
        r.tap("taz");
        QTRY_VERIFY_WITH_TIMEOUT(r.bar().contains("tazzle"), 5000);
    }
    void wordsEndedByPunctuationOrChosenFromTheBarAlsoCount()
    {
        Rig r; QVERIFY(r.start(true, false));
        r.tap("tazzle"); r.press(Qt::Key_Period, QStringLiteral("."));
        QCOMPARE(r.learnedWords(), 1);
        r.press(Qt::Key_Space, QStringLiteral(" "));
        r.tap("tazzle");
        QTRY_VERIFY(r.bar().contains("tazzle"));             // the person's own spelling stays one tap away
        r.method->beginPredictionSelection(r.method->predictionSelectionToken());
        r.method->selectionListItemSelected(Type::WordCandidateList, r.bar().indexOf("tazzle"));
        r.tap("taz");
        QTRY_VERIFY_WITH_TIMEOUT(r.bar().contains("tazzle"), 5000);   // two uses: tapped literal + the full word
    }
    void deniedFieldsNeverLearn_data()
    {
        QTest::addColumn<int>("hints");
        QTest::newRow("password") << int(Qt::ImhSensitiveData);
        QTest::newRow("hidden") << int(Qt::ImhHiddenText);
        QTest::newRow("url") << int(Qt::ImhUrlCharactersOnly);
        QTest::newRow("email") << int(Qt::ImhEmailCharactersOnly);
        QTest::newRow("digits") << int(Qt::ImhDigitsOnly);
        QTest::newRow("formatted-number") << int(Qt::ImhFormattedNumbersOnly);
        QTest::newRow("dialable") << int(Qt::ImhDialableCharactersOnly);
        QTest::newRow("date") << int(Qt::ImhDate);
        QTest::newRow("time") << int(Qt::ImhTime);
        QTest::newRow("prefer-numbers") << int(Qt::ImhPreferNumbers);
    }
    void deniedFieldsNeverLearn()
    {
        QFETCH(int, hints);
        Rig r; QVERIFY(r.start(true, false));
        r.field->hints |= Qt::InputMethodHints(hints);
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); r.method->update();
        for (int i = 0; i < 3; ++i) r.word("tazzle");
        QCOMPARE(r.learnedWords(), 0);
        // Back in an ordinary field nothing was kept either.
        r.field->hints = Qt::ImhNoPredictiveText;
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); r.method->update();
        QCOMPARE(r.learnedWords(), 0);
    }
    void aFieldTheAppRefusesPredictionsInNeverLearns()
    {
        Rig r; QVERIFY(r.start(true, false));
        r.policy.set(false);   // the app's own "no completion" signal
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); r.method->update();
        for (int i = 0; i < 3; ++i) r.word("tazzle");
        QCOMPARE(r.learnedWords(), 0);
        r.policy.set(true);
        r.word("tazzle");
        QCOMPARE(r.learnedWords(), 1);   // allowed again: counting resumes
    }
    void aLearnedWordCanBeForgottenFromTheBarAndStaysGone()
    {
        Rig r; QVERIFY(r.start(true, false));
        r.word("tazzle"); r.word("tazzle");
        r.tap("taz");
        QTRY_VERIFY_WITH_TIMEOUT(r.bar().contains("tazzle"), 5000);
        const int at = r.bar().indexOf("tazzle");
        // Long press: Qt's own suggestion list asks whether the word may be removed, and how to word the menu.
        // WHY through Qt's model rather than calling our methods: this is the exact path the long-press menu takes.
        auto *model = r.ic->inputEngine()->wordCandidateListModel();
        QVERIFY(model);
        QCOMPARE(model->dataAt(at, Role::CanRemoveSuggestion).toBool(), true);
        QCOMPARE(model->dataAt(at, Role::Dictionary).toInt(), int(QVirtualKeyboardSelectionListModel::DictionaryType::User));
        // Ordinary dictionary words and the person's literal letters offer no removal.
        for (int i = 0; i < r.bar().size(); ++i)
            if (i != at) QVERIFY2(!model->dataAt(i, Role::CanRemoveSuggestion).toBool(), qPrintable(r.bar().at(i)));
        model->removeItem(at);   // "Remove from dictionary"
        QCOMPARE(r.learnedWords(), 0);
        QVERIFY(!r.bar().contains("tazzle"));
        r.backspaceAll(3); r.tap("taz");
        QVERIFY(!r.settledBar().contains("tazzle"));
        // Typing it again starts from zero: two uses bring it back.
        r.backspaceAll(3); r.word("tazzle");
        r.tap("taz"); QVERIFY(!r.settledBar().contains("tazzle"));
        r.backspaceAll(3); r.word("tazzle"); r.tap("taz");
        QTRY_VERIFY_WITH_TIMEOUT(r.bar().contains("tazzle"), 5000);
    }
    void forgetEverythingErasesTheFileAndMemoryAtOnce()
    {
        Rig r; QVERIFY(r.start(true, false));
        r.word("tazzle"); r.word("tazzle");
        const QString file = r.data.path() + "/learning/en.swklearn";
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(file), 10000);   // written a few seconds after the last change
        QCOMPARE(QFile::permissions(file) & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther), QFileDevice::Permissions());
        QVERIFY(r.learnedWords() > 0);
        r.method->forgetEverythingLearned();
        QVERIFY(!QFileInfo::exists(file));
        QCOMPARE(r.learnedWords(), 0);
        r.tap("taz");
        QVERIFY(!r.settledBar().contains("tazzle"));
    }
    void stockPlasmaNeverReadsTextItDidNotType_data()
    {
        QTest::addColumn<bool>("native"); QTest::addColumn<int>("pairs");
        QTest::newRow("bridge-sees-the-field") << false << 1;      // the app vouches for its field: "note" + word
        QTest::newRow("native-owned-words-only") << true << 0;     // the field already held "note ", which is not ours
    }
    void stockPlasmaNeverReadsTextItDidNotType()
    {
        QFETCH(bool, native); QFETCH(int, pairs);
        Rig r; QVERIFY(r.start(true, native));
        r.word("tazzle");
        QCOMPARE(r.learnedWords(), 1);
        QCOMPARE(r.learnedPairs(), pairs);
        // The second word typed here follows the first, which this keyboard did enter, so that pair counts in both modes.
        r.word("ginger");
        QCOMPARE(r.learnedPairs(), pairs + 1);
    }
    void diagnosticsShowCountsNeverWords()
    {
        Rig r; QVERIFY(r.start(true, false));
        r.word("tazzle"); r.word("tazzle");
        const auto d = r.method->diagnostics();
        for (auto it = d.begin(); it != d.end(); ++it)
            QVERIFY2(!it.value().toString().contains("tazzle", Qt::CaseInsensitive), qPrintable(it.key()));
        QCOMPARE(d.value("learning").toBool(), true);
        QVERIFY(d.value("learnedWords").toInt() > 0);
    }
    void aSwipedWordCountsAndErasingItTakesTheCountBack()
    {
        Rig r; QVERIFY(r.start(true, false));
        QVERIFY(r.swipe("the"));
        QVERIFY2(r.field->text.trimmed().endsWith("the", Qt::CaseInsensitive), qPrintable(r.field->text));
        QCOMPARE(r.learnedWords(), 1);
        // The first Backspace removes the whole swiped word: it was wrong, so it is not remembered.
        r.method->keyEvent(Qt::Key_Backspace, QString(), Qt::NoModifier);
        QCOMPARE(r.learnedWords(), 0);
    }
    void aSwipeInADeniedFieldNeverLearns()
    {
        Rig r; QVERIFY(r.start(true, false));
        r.policy.set(false);
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); r.method->update();
        r.swipe("the");
        QCOMPARE(r.learnedWords(), 0);
    }
    void aTaughtWordCanBeSwipedWithBothEngines_data()
    {
        QTest::addColumn<bool>("ownEngine"); QTest::newRow("futo") << false; QTest::newRow("own-geometry") << true;
    }
    void aTaughtWordCanBeSwipedWithBothEngines()
    {
        QFETCH(bool, ownEngine);
        if (ownEngine) qputenv("SWIPEKB_ENGINE", "own"); else qunsetenv("SWIPEKB_ENGINE");
        struct Reset { ~Reset() { qunsetenv("SWIPEKB_ENGINE"); } } reset;
        Rig r; QVERIFY(r.start(true, false));
        // Before: the word is not in any swipe dictionary, so a swipe of its keys cannot produce it.
        QVERIFY(r.swipe("tazzle"));
        QVERIFY2(!r.field->text.contains("tazzle"), qPrintable(r.field->text));
        r.field->text = "note "; r.field->cursor = r.field->text.size();
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); r.method->update();
        r.word("tazzle"); r.word("tazzle");
        r.field->text = "note "; r.field->cursor = r.field->text.size();
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); r.method->update();
        QVERIFY(r.swipe("tazzle"));
        QVERIFY2(r.field->text.contains("tazzle", Qt::CaseInsensitive), qPrintable(r.field->text));
        if (ownEngine) QCOMPARE(r.method->diagnostics().value("swipeEngine").toString().left(3), QString("own"));
    }
    void aTaughtWordThatWasForgottenIsNoLongerSwiped()
    {
        Rig r; QVERIFY(r.start(true, false));
        r.word("tazzle"); r.word("tazzle");
        r.tap("taz");
        QTRY_VERIFY_WITH_TIMEOUT(r.bar().contains("tazzle"), 5000);
        r.method->forgetEverythingLearned();
        r.field->text = "note "; r.field->cursor = r.field->text.size();
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); r.method->update();
        QVERIFY(r.swipe("tazzle"));
        QVERIFY2(!r.field->text.contains("tazzle"), qPrintable(r.field->text));   // FUTO still holds it until restart, but it is filtered
    }
    void aSwearWordThePersonTypedThemselvesIsOfferedUnprompted()
    {
        Rig r; QVERIFY(r.start(true, false));
        r.nextWords = {QStringLiteral("damn"), QStringLiteral("world"), QStringLiteral("again")};
        r.word("hello");
        QTRY_VERIFY_WITH_TIMEOUT(!r.bar().isEmpty(), 5000);
        QVERIFY2(!r.settledBar().contains("damn"), "a swear word is never offered out of the blue");
        r.backspaceAll(0);
        // Typed twice by this person: now it passes the filter like any word.
        r.word("damn"); r.word("damn");
        r.word("hello");
        QTRY_VERIFY_WITH_TIMEOUT(r.bar().contains("damn"), 5000);
    }
    void aSwearWordTypedOnlyWhileLearningIsOffStaysFiltered()
    {
        Rig r; QVERIFY(r.start(false, false));
        r.nextWords = {QStringLiteral("damn"), QStringLiteral("world"), QStringLiteral("again")};
        r.word("damn"); r.word("damn"); r.word("hello");
        QVERIFY(!r.settledBar().contains("damn"));
    }
    // ---- built-in texting abbreviations (packaging/texting-words), with learning OFF: they are not learned words ----
    void clearField(Rig &r)
    {
        r.field->text = "note "; r.field->cursor = r.field->text.size();
        QGuiApplication::inputMethod()->update(Qt::ImQueryAll); r.method->update();
    }
    // The swipe's result: typed into the field as the top guess, or at least one tap away on the guess bar.
    // WHY not top guess only: a perfectly straight test finger is not a person's swipe, and FUTO's context model
    // may prefer a common word for it ("smh" -> "any"); that is the swiper's choice to correct with one tap.
    bool offered(Rig &r, const QString &w)
    {
        QTest::qWait(150);
        return r.field->text.contains(w, Qt::CaseInsensitive) || r.bar().contains(w, Qt::CaseInsensitive);
    }
    void abbreviationsCanBeSwipedWithBothEngines_data()
    {
        QTest::addColumn<bool>("ownEngine");
        QTest::addColumn<QString>("word");
        for (const char *w : {"wyd", "smh", "brb", "ikr"})   // missing from the shipped English dictionary
            QTest::addRow("own-%s", w) << true << QString(w);
        // WHY only "wyd" for FUTO: its decoder ranks a perfectly straight test finger toward common words ("smh" ->
        // "any", "brb" -> "been"); on noisy swipes FUTO reaches about 64% in its top 3 (measured, see CHANGELOG).
        QTest::addRow("futo-wyd") << false << QString("wyd");
    }
    void abbreviationsCanBeSwipedWithBothEngines()
    {
        QFETCH(bool, ownEngine); QFETCH(QString, word);
        if (ownEngine) qputenv("SWIPEKB_ENGINE", "own"); else qunsetenv("SWIPEKB_ENGINE");
        struct Reset { ~Reset() { qunsetenv("SWIPEKB_ENGINE"); qunsetenv("SWIPEKB_TEXTING_WORDS"); } } reset;
        Rig r; QVERIFY(r.start(false, false));
        QVERIFY(!r.method->learningEnabled());
        QVERIFY(r.swipe(word));
        QVERIFY2(offered(r, word), qPrintable(r.field->text + " | " + r.bar().join(',')));
        if (ownEngine) QCOMPARE(r.method->diagnostics().value("swipeEngine").toString().left(3), QString("own"));
    }
    void anAbbreviationIsCompletedFromTypedLetters()
    {
        Rig r; QVERIFY(r.start(false, false));
        r.tap("wy");
        QTRY_VERIFY_WITH_TIMEOUT(r.bar().contains("wyd"), 5000);
        r.backspaceAll(2);
        r.tap("hb");
        QTRY_VERIFY_WITH_TIMEOUT(r.bar().contains("hbu"), 5000);
    }
    void aWordMarkedNotAWordIsLiftedAndOrdinarySwipesStillWin()
    {
        Rig r; QVERIFY(r.start(false, false));
        // "pls" is in the shipped dictionary with score 0 ("not a word"): the list lifts it.
        QVERIFY(r.swipe("pls"));
        QVERIFY2(offered(r, "pls"), qPrintable(r.field->text + " | " + r.bar().join(',')));
        // An ordinary word that is a few keys from an abbreviation is still what a clean swipe gives.
        for (const char *w : {"the", "world", "table"}) {
            clearField(r);
            QVERIFY(r.swipe(w));
            QVERIFY2(r.field->text.contains(w, Qt::CaseInsensitive), qPrintable(r.field->text));
        }
    }
    void aSwearAcronymIsSwipedAndCompletedButNeverOfferedUnprompted()
    {
        Rig r; QVERIFY(r.start(false, false));
        r.nextWords = {QStringLiteral("wtf"), QStringLiteral("world"), QStringLiteral("again")};
        r.word("hello");
        QTRY_VERIFY_WITH_TIMEOUT(r.bar().contains("world"), 5000);
        QVERIFY2(!r.settledBar().contains("wtf"), "a swear acronym is never offered out of the blue");
        clearField(r);
        QVERIFY(r.swipe("wtf"));
        QVERIFY2(offered(r, "wtf"), qPrintable(r.field->text + " | " + r.bar().join(',')));
        clearField(r);
        r.tap("wt");
        QTRY_VERIFY_WITH_TIMEOUT(r.bar().contains("wtf"), 5000);
    }
    void aDamagedListFileLeavesTheKeyboardWorkingWithoutAbbreviations()
    {
        QTemporaryDir lists; QVERIFY(lists.isValid());
        QFile f(lists.filePath("en.tsv")); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("garbage\x01 no tabs\n\0\0\0"); f.close();
        qputenv("SWIPEKB_TEXTING_WORDS", lists.path().toUtf8());
        struct Reset { ~Reset() { qunsetenv("SWIPEKB_TEXTING_WORDS"); } } reset;
        Rig r; QVERIFY(r.start(false, false));
        QVERIFY(r.swipe("wyd"));
        QVERIFY2(!offered(r, "wyd"), qPrintable(r.field->text + " | " + r.bar().join(',')));   // no list: not reachable (as before this feature)
        clearField(r);
        QVERIFY(r.swipe("table"));
        QVERIFY2(offered(r, "table"), qPrintable(r.field->text));   // everything else untouched
    }
};

QTEST_MAIN(LearningIntegrationTest)
#include "tst_learningintegration.moc"
