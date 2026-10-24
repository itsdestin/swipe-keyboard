/*
 * SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include <QHash>
#include <QString>
#include <QtGlobal>

namespace swipekb {
struct VerifiedAsset { QString path, sha256, compatibility; };
struct ProfileCapabilities { bool swipe=false, contextRanking=false, nextWord=false; };
// WHY values only: consumers retain a snapshot across worker requests without QObject/GUI lifetimes.
struct ProfileSnapshot {
    QString profileId, requestedLocale, effectiveLanguage, layoutId, inputMode, packId, packVersion,
        textRulesVersion, regionalPolicyVersion, unavailableReason;
    ProfileCapabilities capabilities;
    QHash<QString, VerifiedAsset> assets;
    quint64 epoch=0;
};
QString canonicalLocale(const QString &locale);
// Path of <dir>/swipe-model.bin when its size and SHA-256 equal the pin in assets.lock.json ("trained_models.swipe"),
// else an empty string (the engine then runs geometry-only). resolveProfile() puts the verified file in the
// snapshot as asset "swipeModel" (English: <modelRoot>/language-packs, then <modelRoot>; other languages: the pack folder).
QString verifiedSwipeModel(const QString &modelRoot);
ProfileSnapshot resolveProfile(const QString &locale, const QString &layoutId, const QString &inputMode,
                               const QString &modelRoot, quint64 epoch=0);
// Serialized owner-thread state; async loaders pass their captured epoch to publish().
class ProfileEpoch {
public:
    quint64 invalidate(const QString &locale, const QString &layoutId, const QString &inputMode);
    bool publish(const ProfileSnapshot &result);
    ProfileSnapshot snapshot() const { return current_; }
private:
    ProfileSnapshot current_;
};
} // namespace swipekb
