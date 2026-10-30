// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

// The scoreboards' shared junk rule (tools/benchdata.h): a rank in the language's own word list, which for English
// must stay exactly the old "frequency under 80, names under 120".
#include <QTemporaryDir>
#include <QTest>
#include <fstream>
#include "../benchdata.h"
class TstJunkRule : public QObject { Q_OBJECT
private slots:
    void rankBecomesTheFrequencyOfThatWord() {
        // A synthetic list of 50,000 words with falling frequencies (lower-case duplicates count once, at their best).
        QTemporaryDir dir;
        const std::string path = dir.filePath("t.combined").toStdString();
        {
            std::ofstream f(path);
            f << "dictionary=t,locale=xx\n";
            for (int i = 0; i < 50000; ++i) f << " word=w" << i << ",f=" << (250 - i / 250) << "\n";
            f << " word=W0,f=1\n";
        }
        const benchdata::JunkRule en(path, "en"), de(path, "de");
        QCOMPARE(en.rareBelow(), 250 - (benchdata::JunkRule::kEnglishRareRank - 1) / 250);
        QCOMPARE(de.rareBelow(), 250 - (48000 - 1) / 250);
        QCOMPARE(en.nameBelow(), 250 - (benchdata::JunkRule::kNameRank - 1) / 250);
    }
    void englishKeepsItsOldMeaning() {
        // WHY with the shipped list: the ranks were read off it, and English junk numbers must stay comparable.
        const QByteArray assets = qgetenv("SWIPEKB_ASSETS");
        const std::string dict = assets.toStdString() + "/language-files/en_swipe.combined";
        if (assets.isEmpty() || !std::ifstream(dict)) QSKIP("SWIPEKB_ASSETS with language-files/en_swipe.combined not set");
        const benchdata::JunkRule en(dict, "en");
        QCOMPARE(en.rareBelow(), 80);
        QCOMPARE(en.nameBelow(), 120);
    }
};
QTEST_GUILESS_MAIN(TstJunkRule)
#include "tst_junkrule.moc"
