// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Unit tests for the daemon's language choice (gaming/daemon/languages.hpp), no sockets or models: which Steam
// layouts we serve, which languages the user enabled, and whether a key table can carry a language.
#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include "languages.hpp"
using namespace gamingd;

TEST(SteamLayouts, FourSupportedLayoutsMapToTheirLanguageAndPhysicalKeys)
{
    // Steam's own ids (read from steamui sp.js): qwerty = English US, spanish, french = AZERTY, german = QWERTZ.
    struct { const char *id, *language, *physical; } want[] = {
        {"qwerty", "en", "qwerty"}, {"spanish", "es", "qwerty"}, {"french", "fr", "azerty"}, {"german", "de", "qwertz"}};
    for (const auto &w : want) {
        const auto c = mapSteamLayout(w.id);
        ASSERT_TRUE(c) << w.id;
        EXPECT_EQ(c->language.toStdString(), w.language) << w.id;
        EXPECT_EQ(c->physicalLayout.toStdString(), w.physical) << w.id;
    }
}

TEST(SteamLayouts, EveryOtherLayoutIsUnsupportedSoTheLayerStaysOff)
{
    // CJK, Cyrillic, Greek and other-script layouts, and Latin layouts we have no pack for.
    for (const char *id : {"schinese", "tchinese", "tchinese_bopomofo", "tchinese_cangjie", "tchinese_quick", "japanese",
                           "japanese_kana", "korean", "russian", "bulgarian", "bulgarian_phonetic_traditional", "ukrainian",
                           "greek", "vietnamese", "czech", "polish", "dvorak", "colemak", "qwerty_int", "turkish_f", "", "QWERTY",
                           "german ", "../etc", "french\n"})
        EXPECT_FALSE(mapSteamLayout(id)) << id;
}

TEST(EnabledLocales, ReadsPlasmaKeyboardSettingsInFileOrder)
{
    QTemporaryDir dir;
    const QString path = dir.filePath("plasmakeyboardrc");
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("[General]\nsoundEnabled=false\nenabledLocales=es_ES,es_MX,en_US,en_GB\n");
    f.close();
    // Same canonical spelling the plugin's profile code uses (en-US), same order the user enabled them in.
    EXPECT_EQ(readEnabledLocales(path), (QStringList{"es-ES", "es-MX", "en-US", "en-GB"}));
}

TEST(EnabledLocales, MissingEmptyOrGarbledSettingsGiveNothingNeverAGuess)
{
    QTemporaryDir dir;
    EXPECT_TRUE(readEnabledLocales(dir.filePath("absent")).isEmpty());
    QFile f(dir.filePath("rc"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("[General]\nenabledLocales=\n");
    f.close();
    EXPECT_TRUE(readEnabledLocales(dir.filePath("rc")).isEmpty());
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("[General]\nenabledLocales=xx_,../../etc,fr_FR,es-ES-extra,,DE_de\n");
    f.close();
    // WHY: malformed tags are dropped by the plugin's own canonicalLocale; good ones survive, spelling normalised.
    EXPECT_EQ(readEnabledLocales(dir.filePath("rc")), (QStringList{"fr-FR", "de-DE"}));
}

TEST(EnabledLocales, EmptyListFallsBackToTheSystemLanguageLikeTheDesktopKeyboard)
{
    // Plasma Keyboard: no enabled locales -> only the system locale; "C" counts as en_US.
    EXPECT_EQ(effectiveLocales({}, "de_DE.UTF-8"), (QStringList{"de-DE"}));
    EXPECT_EQ(effectiveLocales({}, "C"), (QStringList{"en-US"}));
    EXPECT_EQ(effectiveLocales({}, "POSIX"), (QStringList{"en-US"}));
    EXPECT_EQ(effectiveLocales({"es-ES"}, "de_DE"), (QStringList{"es-ES"}));
}

TEST(EnabledLocales, PickTakesTheFirstEnabledLocaleOfTheLanguage)
{
    const QStringList enabled{"es-ES", "es-MX", "en-GB", "en-US"};
    EXPECT_EQ(pickLocale(enabled, "en"), QString("en-GB"));
    EXPECT_EQ(pickLocale(enabled, "es"), QString("es-ES"));
    EXPECT_TRUE(pickLocale(enabled, "fr").isEmpty());   // not enabled -> the layer stays off
    EXPECT_TRUE(pickLocale({}, "en").isEmpty());
}

TEST(Geometry, SpanishNeedsItsNTildeKeyOthersUseBaseKeys)
{
    std::vector<swipekb::KeyRect> keys;
    for (char32_t c = U'a'; c <= U'z'; ++c) keys.push_back({c, float(c - U'a') * 10, 0, 10, 10});
    EXPECT_TRUE(geometryCoversLanguage(keys, "en"));
    EXPECT_TRUE(geometryCoversLanguage(keys, "fr"));   // accents ride their base key
    EXPECT_TRUE(geometryCoversLanguage(keys, "de"));
    EXPECT_FALSE(geometryCoversLanguage(keys, "es"));  // ñ has no base key
    keys.push_back({U'ñ', 270, 0, 10, 10});
    EXPECT_TRUE(geometryCoversLanguage(keys, "es"));
    keys.erase(keys.begin(), keys.begin() + 3);        // a, b, c missing
    EXPECT_FALSE(geometryCoversLanguage(keys, "en"));
    // A Cyrillic table has none of the Latin letters.
    std::vector<swipekb::KeyRect> cyr;
    for (char32_t c = U'а'; c < U'а' + 26; ++c) cyr.push_back({c, float(c - U'а') * 10, 0, 10, 10});
    EXPECT_FALSE(geometryCoversLanguage(cyr, "en"));
}
