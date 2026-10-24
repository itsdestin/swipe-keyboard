// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Read-only ownership probe: no GUI, D-Bus, configuration writes or environment overrides.
#include <KConfig>
#include <KConfigGroup>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QRegularExpression>
#include <cstdio>

static QJsonValue entry(const KConfigGroup &group, const char *key)
{
    return group.hasKey(key) ? QJsonValue(group.readEntry(key, QString())) : QJsonValue(QJsonValue::Null);
}

static bool supportedUserSyntax(const QByteArray &bytes)
{
    // WHY a deliberately narrow whitelist, not a replacement KConfig parser:
    // public KConfig value APIs discard deletion, expansion and locale provenance.
    // Schema 2 can restore only plain values/absence. Unrelated ordinary nested
    // groups (notably KWin's default Tiling UUID groups) do not affect those values.
    // WHY classify only literal group identifiers: escaped/ambiguous headers might
    // alias Wayland, so refuse them instead of guessing KConfig's decoding rules.
    // KConfig remains authoritative for values, precedence and immutable flags.
    // WHY: KWin writes empty nested Tiling groups. KConfig keeps empty child
    // groups distinct from the root; require a named root but allow empty children.
    const QRegularExpression group(QStringLiteral(R"(^\[[A-Za-z0-9_ .:/-]+\](\[[A-Za-z0-9_ .:/-]*\])*(\[\$i\])?$)"));
    const QRegularExpression key(QStringLiteral(R"(^[A-Za-z0-9_ .:/-]+(\[\$i\])?$)"));
    bool wayland = false;
    for (const auto &rawLine : bytes.split('\n')) {
        const auto line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        if (!line.startsWith('[') && !wayland)
            continue; // unrelated entries remain opaque and are never restored by setup
        // Reject non-ASCII identifiers and control bytes rather than guessing
        // whether KConfig will normalize them into a different key or group.
        const auto identifier = line.startsWith('[') ? line : line.left(line.indexOf('='));
        for (const auto character : identifier) {
            if (character < 32 || character > 126)
                return false;
        }
        if (line == "[$i]")
            continue;
        if (line.startsWith('[')) {
            if (!group.match(QString::fromLatin1(line)).hasMatch())
                return false;
            wayland = line == "[Wayland]" || line == "[Wayland][$i]";
        } else if (!line.contains('=') || !key.match(QString::fromLatin1(identifier.trimmed())).hasMatch()) {
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // WHY fail closed: KConfig can treat an unreadable source as empty. Absence is
    // trustworthy only if every existing participating source can actually be read.
    for (const auto &directory : QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation)) {
        for (const char *name : {"kwinrc", "kdeglobals"}) {
            QFileInfo info(directory + QLatin1Char('/') + QString::fromLatin1(name));
            if (info.exists() || info.isSymbolicLink()) {
                QFile file(info.filePath());
                if (!info.isFile() || !file.open(QIODevice::ReadOnly)) {
                    std::fputs("Cannot read a KConfig source; ownership cannot be established.\n", stderr);
                    return 2;
                }
                if (directory == QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                    && QByteArray(name) == "kwinrc" && !supportedUserSyntax(file.readAll())) {
                    std::fputs("Unsupported user KConfig syntax: setup cannot preserve deletion, expansion, "
                               "localized or other non-plain entries. No ownership snapshot is available.\n", stderr);
                    return 2;
                }
            }
        }
    }
    // WHY two native views: an absolute SimpleConfig excludes inherited session defaults;
    // the full relative view follows KWin's KSharedConfig defaults, including Kiosk locks.
    // Checking BOTH prevents an effective immutable overlay from hiding raw user locks.
    KConfig raw(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                    + QStringLiteral("/kwinrc"), KConfig::SimpleConfig);
    KConfig effective(QStringLiteral("kwinrc"), KConfig::FullConfig);
    const KConfigGroup user(&raw, "Wayland");
    const KConfigGroup merged(&effective, "Wayland");
    QJsonObject result;
    for (const char *key : {"InputMethod", "VirtualKeyboardMode"}) {
        result.insert(QString::fromLatin1(key), QJsonObject{
            {QStringLiteral("raw"), entry(user, key)},
            {QStringLiteral("effective"), entry(merged, key)},
            {QStringLiteral("immutable"), user.isEntryImmutable(key) || merged.isEntryImmutable(key)}});
    }
    const auto json = QJsonDocument(result).toJson(QJsonDocument::Compact);
    return std::fwrite(json.constData(), 1, json.size(), stdout) == size_t(json.size()) ? 0 : 1;
}
