// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include <QtTest>
#include <limits>
#include "swipekb/text.hpp"
#include "swipekb/spellings.hpp"
#include "languagetextrules.h"
using namespace swipekb;
class LanguageGeometryTest : public QObject {
    Q_OBJECT
private slots:
    void unicodeRules() {
        QCOMPARE(displayForm(QString::fromUtf8("e\xcc\x81" "cole")), QString::fromUtf8("école"));
        QVERIFY(lookupForm("n", "es") != lookupForm("ñ", "es"));
        QVERIFY(lookupForm("si", "es") != lookupForm("sí", "es"));
        QCOMPARE(lookupForm("l’école", "fr"), lookupForm("l'école", "fr"));
        QCOMPARE(displayForm("cœur garçon"), QString("cœur garçon"));
        QCOMPARE(displayForm("schön Straße"), QString("schön Straße"));
        QCOMPARE(applyTextCase("straße", true), QString("STRASSE"));
        QCOMPARE(textContextWords("Bonjour. l’école, Straße sí",3),QStringList({"l’école","Straße","sí"}));
        QCOMPARE(lookupForm(QString::fromUtf8("e\xcc\x81" "cole"),"fr"),lookupForm("école","fr"));
        QVERIFY(lookupForm("schon","de")!=lookupForm("schön","de"));
        QCOMPARE(popupBase(U'é', "fr"), U'e'); QCOMPARE(popupBase(U'ñ', "es"), U'ñ');
        QCOMPARE(popupBase(U'ö', "de"), U'o');
    }
    void sharedContextParity() {
        for (const auto &text:QStringList{"price $5 can't foo:bar", "Bonjour. l’école, Straße sí", "hello, world! next", "one\u00a0two"}) {
            QStringList engineWords;
            for(const auto &word:contextWords(text.toStdString(),20)) engineWords.append(QString::fromStdString(word));
            QCOMPARE(textContextWords(text),engineWords);
        }
    }
    void measuredGeometry() {
        std::vector<KeyRect> keys;
        for (char32_t c = U'a'; c <= U'z'; ++c)
            keys.push_back({c, float(c-U'a')*10, 0, 10, 10});
        std::vector<TracePoint> trace{{0,0,0},{250,10,10}};
        auto q = normalize(trace,keys); QVERIFY(q);
        std::swap(keys[0].x,keys[16].x); // AZERTY-like permutation: never synthesize QWERTY centers.
        auto az = normalize(trace,keys); QVERIFY(az);
        QCOMPARE(az->key_cx[0],q->key_cx[16]);
        std::swap(keys[24].x,keys[25].x); // QWERTZ
        auto de = normalize(trace,keys); QVERIFY(de); QCOMPARE(de->key_cx[24],q->key_cx[25]);
        keys.push_back({U'ñ',125,10,10,10});
        auto es = normalize(trace,keys,"abcdefghijklmnopqrstuvwxyzñ"); QVERIFY(es);
        QCOMPARE(es->key_cx.size(),size_t(27));
        keys[0].x = std::numeric_limits<float>::quiet_NaN(); QVERIFY(!normalize(trace,keys));
        keys[0].x=0; keys[0].w=-1; QVERIFY(!normalize(trace,keys));
        keys[0].w=10; trace[0].x=std::numeric_limits<float>::infinity(); QVERIFY(!normalize(trace,keys));
        trace[0].x=1e38f;
        for (auto &key:keys) { key.x*=1e-38f; key.y*=1e-38f; key.w*=1e-38f; key.h*=1e-38f; }
        QVERIFY(!normalize(trace,keys)); // finite inputs can overflow during normalization too.
    }
    void displayAlternatives() {
        auto s = Spellings::fromCombinedText(" word=si,f=200\n word=sí,f=200\n word=l’école,f=200\n word=cœur,f=180\n word=garçon,f=180\n word=Straße,f=190\n", "de");
        QCOMPARE(QString::fromStdString(s.spell("sí")),QString("sí"));
        QCOMPARE(QString::fromStdString(s.spell("lécole")),QString("l’école"));
        QCOMPARE(QString::fromStdString(s.spell("cœur")),QString("cœur"));
        QCOMPARE(QString::fromStdString(s.spell("straße")),QString("Straße"));
    }
};
QTEST_GUILESS_MAIN(LanguageGeometryTest)
#include "tst_languagegeometry.moc"
