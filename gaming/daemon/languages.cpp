// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "languages.hpp"

#include <QLocale>
#include <QSettings>
#include <QStandardPaths>

#include "languageprofile.h"
#include "languageruntime.h"
#include "languagetextrules.h"

namespace gamingd {

std::optional<ProfileChoice> mapSteamLayout(const std::string &id)
{
    // WHY exact, case-sensitive ids: these are Steam's internal layout names and nothing else may match by accident.
    if (id == "qwerty") return ProfileChoice{"en", "qwerty"};
    if (id == "spanish") return ProfileChoice{"es", "qwerty"};
    if (id == "french") return ProfileChoice{"fr", "azerty"};
    if (id == "german") return ProfileChoice{"de", "qwertz"};
    return std::nullopt;
}

QString defaultSettingsFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/plasmakeyboardrc");
}

QStringList readEnabledLocales(const QString &settingsFile)
{
    // WHY QSettings and not KConfig: the daemon is Qt Core only, and this one plain key needs no KDE library.
    // The file is only ever read, never created or changed.
    QSettings settings(settingsFile, QSettings::IniFormat);
    QStringList out;
    // WHY no "General/" prefix: QSettings treats the [General] section as its top level.
    for (const QString &raw : settings.value(QStringLiteral("enabledLocales")).toStringList()) {
        // WHY the plugin's own parser: the same tag spelling and the same rejection of malformed tags.
        const QString tag = swipekb::canonicalLocale(raw.trimmed());
        if (!tag.isEmpty() && !out.contains(tag)) out.append(tag);
    }
    return out;
}

QString systemLocaleName() { return QLocale::system().name(); }

QStringList effectiveLocales(const QStringList &enabled, const QString &systemLocale)
{
    if (!enabled.isEmpty()) return enabled;
    // Same fallback as Plasma Keyboard's main.qml: no enabled locales -> the current system locale; "C" -> en_US.
    QString name = systemLocale.section('.', 0, 0).section('@', 0, 0);
    if (name.isEmpty() || name == QLatin1String("C") || name == QLatin1String("POSIX")) name = QStringLiteral("en_US");
    const QString tag = swipekb::canonicalLocale(name);
    return tag.isEmpty() ? QStringList{} : QStringList{tag};
}

QString pickLocale(const QStringList &enabled, const QString &language)
{
    for (const QString &tag : enabled)
        if (tag.section('-', 0, 0) == language) return tag;
    return {};
}

bool geometryCoversLanguage(const std::vector<swipekb::KeyRect> &keys, const QString &language)
{
    // The plugin's readiness check (SwipeInputMethod::isReadyForLayout): normalising a dummy swipe succeeds only if
    // every letter of the language, or its popup base key, has measured geometry.
    return swipekb::normalize({{0, 0, 0}, {1, 1, 1}}, keys, swipekb::languageAlphabet(language).toStdString(),
                              swipekb::popupAliasesFor(language)).has_value();
}

} // namespace gamingd
