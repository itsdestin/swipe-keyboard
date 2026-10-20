#include "profilediagnostics.h"
#include <QRegularExpression>
namespace swipekb {
QVariantMap profileDiagnostics(const ProfileSnapshot &p)
{
    // WHY never serialize the snapshot: assets carry paths; reasons and manifest identifiers are untrusted.
    static const QRegularExpression identifier("^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}\\z");
    auto id=[&](const QString &s) { return identifier.match(s).hasMatch() ? s : QString(); };
    static const QStringList reasons{
        "", "Profile loading", "Unsupported or malformed locale tag",
        "Unsupported script; these profiles declare Latin only", "Unsupported physical layout or input mode",
        "Unsupported language", "Language pack manifest missing or invalid", "Pack schema/version/compatibility mismatch",
        "Pack does not declare requested locale/layout/mode", "Embedded fixed asset/license pins unavailable",
        "Pack license/attribution missing or repository pin mismatch", "Pack source/model revision mismatch",
        "Required encoder/dictionary missing", "Context model/vocabulary must be paired",
        "Non-English decoder/context models are not supported", "Unknown asset role",
        "Asset language/layout compatibility mismatch", "Asset missing, corrupt or checksum/size mismatch",
        "Encoder/model metadata does not match repository compatibility pins",
        "Bundled English assets/license missing or checksum mismatch", "English encoder metadata checksum mismatch",
        "English decoder checksum mismatch", "English context checksum mismatch", "Verified profile engine initialization failed"
    };
    return {{"locale",id(p.requestedLocale)}, {"language",id(p.effectiveLanguage)},
        {"layout",id(p.layoutId)}, {"inputMode",id(p.inputMode)},
        {"packId",id(p.packId)}, {"packVersion",id(p.packVersion)},
        {"swipeCapable",p.capabilities.swipe}, {"contextRankingCapable",p.capabilities.contextRanking},
        {"nextWordCapable",p.capabilities.nextWord},
        {"unavailableReason",reasons.contains(p.unavailableReason) ? p.unavailableReason : QString("Unavailable reason not recognized")}};
}
}
