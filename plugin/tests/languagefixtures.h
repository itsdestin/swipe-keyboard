/*
 * SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>

namespace languagefixtures {
inline QByteArray read(const QString &path) {
    QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
inline bool write(const QString &root,const QString &name,const QByteArray &data) {
    QFile f(QDir(root).filePath(name)); return f.open(QIODevice::WriteOnly) && f.write(data)==data.size();
}
inline QString hash(const QByteArray &data) { return QString::fromLatin1(QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex()); }
inline bool runtimeAvailable() {
    return !qEnvironmentVariable("SWIPEKB_TEST_ENGLISH_ASSETS").isEmpty() && !qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS").isEmpty();
}
// WHY LANGUAGE_PACKS instead of the old CC licence file: the fixture's licence texts are the real pinned ODC-By,
// Tatoeba and notice files of an installed pack folder, so the loader's pin comparison is exercised on real bytes.
inline bool available() {
    return !qEnvironmentVariable("SWIPEKB_TEST_ENGLISH_ASSETS").isEmpty() && !qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS").isEmpty();
}
// WHY actual pinned bytes, never a production bypass: supplied-but-missing/bad fixtures fail the tests;
// only an explicitly absent fixture setting is skipped by the caller. No mutable upstream file is touched.
// The pack is a minimal French one: the pinned word list and licence texts, no next-word files.
inline bool populate(const QString &root,QJsonObject &manifest) {
    const auto packs=qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS");
    const auto odc=read(packs+"/ODC-By-1.0.txt");
    const auto tatoeba=read(packs+"/TATOEBA-CC-BY-2.0-FR.txt");
    const auto notice=read(packs+"/LANGUAGE-FILES-NOTICE.md");
    const auto dictionary=read(packs+"/fr.combined");
    if (odc.isEmpty() || tatoeba.isEmpty() || notice.isEmpty() || dictionary.isEmpty()) return false;
    if (!write(root,"ODC-By-1.0.txt",odc) || !write(root,"TATOEBA-CC-BY-2.0-FR.txt",tatoeba) ||
        !write(root,"LANGUAGE-FILES-NOTICE.md",notice) || !write(root,"fr.combined",dictionary)) return false;
    auto asset=[&](const QString &path,const QString &compat) {
        const auto bytes=read(QDir(root).filePath(path));
        return QJsonObject{{"path",path},{"sha256",hash(bytes)},{"size",bytes.size()},{"compatibility",compat}};
    };
    manifest=QJsonObject{{"schema",3},{"id","fr-fixture"},{"version","1"},{"language","fr"},
        {"locales",QJsonArray{"fr","fr-*"}},{"layouts",QJsonArray{"azerty"}},{"modes",QJsonArray{"Latin"}},
        {"scripts",QJsonArray{"Latn"}},{"compatibility","swipekb-open-v1"},
        {"textRulesVersion","unicode-latin-v1"},{"transformationVersion","fineweb-counts-v1"},
        {"source",QJsonObject{{"url","https://github.com/itsdestin/swipe-keyboard/releases/download/language-files-2026-10-22-open-words/SHA256SUMS"},
            {"sha256","bd5786013571301098ad46a288e8cbcbdb8991c1b609c4f225e41312c1b6de85"}}},
        {"license",QJsonObject{{"id","ODC-By-1.0 AND CC-BY-2.0-FR"},{"path","LANGUAGE-FILES-NOTICE.md"},{"sha256",hash(notice)},
            {"termsPath","ODC-By-1.0.txt"},{"termsSha256",hash(odc)},
            {"tatoebaTermsPath","TATOEBA-CC-BY-2.0-FR.txt"},{"tatoebaTermsSha256",hash(tatoeba)},
            {"attribution","FineWeb text (ODC-By 1.0) and Tatoeba (CC BY 2.0 FR); fixture"}}},
        {"assets",QJsonObject{{"dictionary",asset("fr.combined","fr")}}}};
    return true;
}
}
