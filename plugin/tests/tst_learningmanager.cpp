// The on-disk side of learning: the on/off flag, where and how the store is written, resetting, damaged files.
#include <QtTest>
#include <QTemporaryDir>
#include <sys/stat.h>
#include "learningmanager.h"

namespace {
void switchOn(const QString &dir)
{
    QFile f(dir + "/" + LearningManager::flagFile());
    QVERIFY(f.open(QIODevice::WriteOnly));
}
}

class LearningManagerTest : public QObject {
    Q_OBJECT
private slots:
    void withoutTheFlagNothingIsLearnedOrWritten()
    {
        QTemporaryDir dir;
        LearningManager m([&] { return dir.path(); });
        m.setNoCache(true);
        QVERIFY(!m.enabled());
        QVERIFY(m.model("en") == nullptr);
        m.changed();
        m.flush();
        QVERIFY(!QFileInfo::exists(dir.path() + "/learning"));
        QVERIFY(QDir(dir.path()).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
    }
    void withTheFlagTheStoreIsPrivateAndWrittenAtomically()
    {
        QTemporaryDir dir;
        switchOn(dir.path());
        {
            LearningManager m([&] { return dir.path(); });
            m.setNoCache(true);
            auto *model = m.model("en");
            QVERIFY(model);
            model->learn("a", "b", true, false, 100);
            m.changed();
            m.flush();
            const QString file = m.filePathFor("en");
            QVERIFY(QFileInfo::exists(file));
            // 0600 file inside a 0700 folder: nobody else can read or even list it.
            // WHY masks: Qt reports each owner bit twice (Owner and User), and nobody else may hold any bit.
            const auto others = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
                | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther | QFileDevice::ExeOwner;
            const auto filePerms = QFile::permissions(file);
            QVERIFY(filePerms.testFlag(QFileDevice::ReadOwner) && filePerms.testFlag(QFileDevice::WriteOwner));
            QVERIFY2(!(filePerms & others), "the learned file must be 0600");
            const auto dirPerms = QFile::permissions(dir.path() + "/learning");
            QVERIFY(dirPerms.testFlag(QFileDevice::ReadOwner) && dirPerms.testFlag(QFileDevice::WriteOwner)
                    && dirPerms.testFlag(QFileDevice::ExeOwner));
            QVERIFY2(!(dirPerms & (others & ~QFileDevice::ExeOwner)), "the learned folder must be 0700");
            // No half-written leftovers beside it.
            QCOMPARE(QDir(dir.path() + "/learning").entryList(QDir::NoDotAndDotDot | QDir::AllEntries), QStringList{"en.swklearn"});
        }
        // A new keyboard process reads it back.
        LearningManager again([&] { return dir.path(); });
        again.setNoCache(true);
        QCOMPARE(again.model("en")->pairCount(), size_t(1));
        QVERIFY(again.model("es")->empty());   // one file per language
    }
    void everySaveReplacesTheFileWholeSoACrashCannotLeaveHalfAFile()
    {
        QTemporaryDir dir;
        switchOn(dir.path());
        LearningManager m([&] { return dir.path(); });
        m.setNoCache(true);
        m.model("en")->learn("a", "b", true, false, 1);
        m.changed(); m.flush();
        struct stat first {};
        QCOMPARE(::stat(qPrintable(m.filePathFor("en")), &first), 0);
        m.model("en")->learn("c", "d", true, false, 2);
        m.changed(); m.flush();
        struct stat second {};
        QCOMPARE(::stat(qPrintable(m.filePathFor("en")), &second), 0);
        // A temp file renamed over the old one is a different file (new inode); rewriting in place would keep it.
        QVERIFY2(first.st_ino != second.st_ino, "the store must be written to a temp file and renamed into place");
        QCOMPARE(QDir(dir.path() + "/learning").entryList(QDir::NoDotAndDotDot | QDir::AllEntries), QStringList{"en.swklearn"});
    }
    void writesWaitForTheDelayInsteadOfHappeningOnEveryWord()
    {
        QTemporaryDir dir;
        switchOn(dir.path());
        LearningManager m([&] { return dir.path(); });
        m.setSaveDelayMs(400);
        for (int i = 0; i < 5; ++i) { m.model("en")->learn("a", "w" + std::to_string(i), true, false, i); m.changed(); QTest::qWait(30); }
        QVERIFY(!QFileInfo::exists(m.filePathFor("en")));   // still inside the delay: nothing written yet
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(m.filePathFor("en")), 3000);
    }
    void aDelayedWriteHappensOnItsOwnAndOnShutdown()
    {
        QTemporaryDir dir;
        switchOn(dir.path());
        {
            LearningManager m([&] { return dir.path(); });
            m.setSaveDelayMs(20);
            m.model("en")->learn("", "word", true, false, 1);
            m.changed();
            QTRY_VERIFY(QFileInfo::exists(m.filePathFor("en")));
            m.model("en")->learn("", "later", true, false, 2);
            m.changed();   // never waited for: the destructor must still write it
        }
        LearningManager again([&] { return dir.path(); });
        QCOMPARE(again.model("en")->wordCount(), size_t(2));
    }
    void aDamagedFileStartsEmptyAndTheKeyboardKeepsWorking()
    {
        QTemporaryDir dir;
        switchOn(dir.path());
        QDir().mkpath(dir.path() + "/learning");
        QFile bad(dir.path() + "/learning/en.swklearn");
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("SWKBLEARN 1\nW\tbroken\n");
        bad.close();
        LearningManager m([&] { return dir.path(); });
        m.setNoCache(true);
        auto *model = m.model("en");
        QVERIFY(model);
        QVERIFY(model->empty());
        model->learn("", "fresh", true, false, 5);
        m.changed();
        m.flush();
        LearningManager again([&] { return dir.path(); });
        again.setNoCache(true);
        QVERIFY(again.model("en")->usedByPerson("fresh", 1));
    }
    void forgetEverythingDeletesFilesAndMemoryAtOnce()
    {
        QTemporaryDir dir;
        switchOn(dir.path());
        LearningManager m([&] { return dir.path(); });
        m.setNoCache(true);
        m.model("en")->learn("a", "b", true, false, 1);
        m.model("de")->learn("c", "d", true, false, 1);
        m.changed();
        m.flush();
        QVERIFY(QFileInfo::exists(m.filePathFor("en")));
        QVERIFY(QFileInfo::exists(m.filePathFor("de")));
        m.forgetAll();
        QVERIFY(!QFileInfo::exists(m.filePathFor("en")));
        QVERIFY(!QFileInfo::exists(m.filePathFor("de")));
        QVERIFY(!QFileInfo::exists(dir.path() + "/learning"));
        QVERIFY(m.model("en")->empty());   // memory too
        m.flush();                         // nothing comes back from a stale copy
        QVERIFY(!QFileInfo::exists(m.filePathFor("en")));
    }
    void aResetFromAnotherProcessEmptiesAKeyboardThatIsAlreadyRunning()
    {
        QTemporaryDir dir;
        switchOn(dir.path());
        LearningManager running([&] { return dir.path(); });
        running.setNoCache(true);
        running.model("en")->learn("a", "b", true, false, 1);
        running.changed();
        running.flush();
        QVERIFY(!running.model("en")->empty());
        QTest::qWait(1100);   // the marker is judged by its time stamp
        LearningManager tool([&] { return dir.path(); });   // e.g. `swipe-keyboard learning forget`
        tool.forgetAll();
        QVERIFY(running.model("en")->empty());
        running.model("en")->learn("c", "d", true, false, 2);   // new learning after the reset is kept
        running.changed();
        running.flush();
        LearningManager again([&] { return dir.path(); });
        again.setNoCache(true);
        QCOMPARE(again.model("en")->pairCount(), size_t(1));
        QVERIFY(again.model("en")->usedByPerson("d", 1));
        QVERIFY(!again.model("en")->usedByPerson("b", 1));
    }
    void switchingLearningOffStopsEverythingAndWritesNothing()
    {
        QTemporaryDir dir;
        switchOn(dir.path());
        LearningManager m([&] { return dir.path(); });
        m.setNoCache(true);
        m.model("en")->learn("a", "b", true, false, 1);
        QVERIFY(QFile::remove(dir.path() + "/" + LearningManager::flagFile()));
        QVERIFY(!m.enabled());
        QVERIFY(m.model("en") == nullptr);
        m.changed();
        m.flush();
        QVERIFY(!QFileInfo::exists(m.filePathFor("en")));
    }
    void languageNamesCannotEscapeTheFolder()
    {
        QTemporaryDir dir;
        switchOn(dir.path());
        LearningManager m([&] { return dir.path(); });
        m.setNoCache(true);
        for (const char *bad : {"", "../x", "en/../../x", "EN", "e", "en us", "a\nb"})
            QVERIFY2(m.model(bad) == nullptr, bad);
        QVERIFY(m.model("en") != nullptr);
        QVERIFY(m.model("pt-BR") != nullptr);
    }
    void aDifferentDataFolderStartsFromItsOwnFiles()
    {
        QTemporaryDir a, b;
        switchOn(a.path());
        QString current = a.path();
        LearningManager m([&] { return current; });
        m.setNoCache(true);
        m.model("en")->learn("x", "y", true, false, 1);
        current = b.path();
        QVERIFY(m.model("en") == nullptr);   // b has no flag
        switchOn(b.path());
        QVERIFY(m.model("en")->empty());     // and a's learning is not carried over
    }
};

QTEST_GUILESS_MAIN(LearningManagerTest)
#include "tst_learningmanager.moc"
