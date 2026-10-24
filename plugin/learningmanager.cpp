// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "learningmanager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

static bool usableLanguage(const QString &language)
{
    // WHY a strict whitelist: the language becomes part of a file name.
    static const QRegularExpression ok(QStringLiteral("^[a-z]{2,3}(-[A-Za-z0-9]{2,8})?$"));
    return ok.match(language).hasMatch();
}

LearningManager::LearningManager(std::function<QString()> dataDir, QObject *parent)
    : QObject(parent), m_dataDir(std::move(dataDir))
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] { flush(); });
}

LearningManager::~LearningManager()
{
    // WHY write on the way out: closing the keyboard within a few seconds of a word must not lose it.
    flush();
}

QString LearningManager::filePathFor(const QString &language) const
{
    return m_dataDir() + QLatin1Char('/') + storeDirName() + QLatin1Char('/') + language + QStringLiteral(".swklearn");
}

void LearningManager::dropAll()
{
    m_timer.stop();
    m_slots.clear();
}

void LearningManager::checkDisk()
{
    const QString dir = m_dataDir();
    if (!m_noCache && m_checked.isValid() && m_checked.elapsed() < 1000 && dir == m_lastDir) return;
    m_checked.start();
    // WHY a different data folder means a different person/profile: forget what the old one held.
    if (dir != m_lastDir) { dropAll(); m_lastDir = dir; m_resetSeen = 0; m_resetKnown = false; }
    const bool on = !dir.isEmpty() && QFileInfo::exists(dir + QLatin1Char('/') + flagFile());
    if (!on) {
        // WHY not written: switching learning off means no more learning, and nothing pending is kept.
        if (m_enabled) dropAll();
        m_enabled = false;
        return;
    }
    m_enabled = true;
    // The reset marker: a newer one than we have seen means "forget everything" was requested from outside.
    const QFileInfo marker(dir + QLatin1Char('/') + resetFile());
    const qint64 stamp = marker.exists() ? marker.lastModified().toMSecsSinceEpoch() : 0;
    if (!m_resetKnown) { m_resetKnown = true; m_resetSeen = stamp; }   // first look: whatever exists now is history
    else if (stamp > m_resetSeen) { m_resetSeen = stamp; dropAll(); }
}

bool LearningManager::enabled()
{
    checkDisk();
    return m_enabled;
}

swipekb::PersonalModel *LearningManager::model(const QString &language)
{
    checkDisk();
    if (!m_enabled || !usableLanguage(language)) return nullptr;
    auto it = m_slots.find(language);
    if (it != m_slots.end()) return it->second.model.get();
    Slot slot;
    slot.dir = m_lastDir;
    slot.model = std::make_unique<swipekb::PersonalModel>();
    QFile file(filePathFor(language));
    // WHY read at most one byte over the model's own ceiling: a huge file is refused, not loaded.
    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = file.read((4 << 20) + 1);
        // A damaged file leaves the model empty (parse() clears it); the next save replaces the file.
        slot.model->parse(bytes.toStdString());
    }
    auto *raw = slot.model.get();
    m_slots.emplace(language, std::move(slot));
    return raw;
}

void LearningManager::changed()
{
    m_timer.start(m_saveDelay);
}

bool LearningManager::saveSlot(const QString &language, Slot &slot)
{
    if (!slot.model || !slot.model->dirty()) return true;
    const QString dir = slot.dir + QLatin1Char('/') + storeDirName();
    if (!QDir().mkpath(dir)) return false;
    // WHY 0700: the words typed are private; other accounts must not even list the folder.
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QSaveFile out(dir + QLatin1Char('/') + language + QStringLiteral(".swklearn"));
    if (!out.open(QIODevice::WriteOnly)) return false;
    out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const std::string text = slot.model->serialize();
    if (out.write(text.data(), qint64(text.size())) != qint64(text.size())) return false;
    // commit() renames the finished temp file over the old one, so a crash never leaves half a file.
    if (!out.commit()) return false;
    QFile::setPermissions(out.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    slot.model->markClean();
    return true;
}

void LearningManager::flush()
{
    m_timer.stop();
    // WHY re-check first: if learning was switched off or reset meanwhile, nothing may be written back.
    checkDisk();
    if (!m_enabled) return;
    for (auto &entry : m_slots) saveSlot(entry.first, entry.second);
}

void LearningManager::forgetAll()
{
    const QString dir = m_dataDir();
    m_timer.stop();
    m_slots.clear();
    if (dir.isEmpty()) return;
    QDir(dir + QLatin1Char('/') + storeDirName()).removeRecursively();
    // WHY also touch the marker: a keyboard running in another process learns of the reset within a second.
    QFile marker(dir + QLatin1Char('/') + resetFile());
    if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate)) marker.write(QByteArray::number(now()));
    m_resetSeen = QFileInfo(dir + QLatin1Char('/') + resetFile()).lastModified().toMSecsSinceEpoch();
    m_resetKnown = true;
}
