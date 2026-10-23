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

bool configureRecognizer(const ProfileSnapshot &snapshot, ModelPaths &paths, RecognizerOptions &options)
{
    if (!snapshot.capabilities.swipe) return false;
    auto path = [&](const QString &role) { return snapshot.assets.value(role).path.toStdString(); };
    paths.encoder = path("encoder"); paths.dictionary = path("dictionary"); paths.decoder = path("decoder");
    // WHY the file kind decides, not the language: English uses the open n-gram when its pack is installed and
    // verified (compatibility "en:context-ngram-v1"); otherwise it keeps FUTO's neural model as before. (This branch
    // had this rule in the plugin; the shared function must carry it so the Gaming Mode daemon agrees with the plugin.)
    const bool ngramContext = snapshot.assets.value("contextModel").compatibility.endsWith(QStringLiteral(":context-ngram-v1"));
    if (!ngramContext) {
        paths.lm_model = path("contextModel"); paths.lm_vocab = path("contextVocab");
    } else {
        paths.ngram = path("contextModel");
    }
    options.language = snapshot.effectiveLanguage.toStdString();
    // WHY foreign swipe ranking and completion share only that locale's pinned contextual n-gram; English joins them
    // when it has one (above), else the flag stays off and FUTO's model ranks.
    options.ngram = ngramContext;
    options.alphabet = languageAlphabet(snapshot.effectiveLanguage).toStdString();
    options.popupAliases = popupAliasesFor(snapshot.effectiveLanguage);
    return true;
}
} // namespace swipekb
