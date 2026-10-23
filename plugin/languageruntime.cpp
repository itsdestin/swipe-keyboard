#include "languageruntime.h"
#include "languagetextrules.h"

namespace swipekb {
QString languageModelRoot(const QString &locale, const QString &baseDir)
{
    const auto language = canonicalLocale(locale).section('-', 0, 0);
    // WHY packaged packs are the default; an explicit local override never enables English fallback.
    return language == "en" ? baseDir : qEnvironmentVariable("SWIPEKB_LANGUAGE_PACKS", baseDir + "/language-packs");
}

std::unordered_map<char32_t, char32_t> popupAliasesFor(const QString &language)
{
    std::unordered_map<char32_t, char32_t> aliases;
    for (char32_t c : languageAlphabet(language).toUcs4()) {
        const auto base = popupBase(c, language);
        if (base != c) aliases[c] = base;
    }
    return aliases;
}

bool configureLanguageEngine(const ProfileSnapshot &snapshot, LanguageEngineSetup &setup)
{
    if (!snapshot.capabilities.swipe) return false;
    auto path = [&](const QString &role) { return snapshot.assets.value(role).path.toStdString(); };
    setup = {};
    setup.language = snapshot.effectiveLanguage.toStdString();
    setup.dictionary = path("dictionary");
    // WHY only the verified n-gram pack: it is the sentence prior for swipes; without it word frequency ranks.
    if (snapshot.assets.value("contextModel").compatibility.endsWith(QStringLiteral(":context-ngram-v1")))
        setup.ngram = path("contextModel");
    // WHY an override only for trials/tests: the normal model is the lock-verified one; a missing or altered file
    // simply leaves the engine on geometry.
    const QString overridePath = qEnvironmentVariable("SWIPEKB_SWIPE_MODEL");
    setup.swipeModel = overridePath.isEmpty() ? path("swipeModel") : overridePath.toStdString();
    return !setup.dictionary.empty();
}
} // namespace swipekb
