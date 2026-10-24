/*
 * SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

// LearningManager — the keyboard's on-disk side of "learn from my typing" (Phase 2 of the open-engine roadmap).
//
// It owns one swipekb::PersonalModel per language and answers three questions: is learning switched on, where is
// the file, and has the person asked to forget everything.
//
//   * ON/OFF is a plain file in the keyboard's data folder:
//     `learn-typing` present = on. Off (or absent) means nothing is learned AND nothing learned is used.
//   * The store is <data>/learning/<language>.swklearn, one small text file per language, written atomically
//     (temp file + rename), a few seconds after the last change, readable only by the owner (0600, folder 0700).
//   * "Forget everything" deletes those files and clears memory at once. A second keyboard process or the
//     command-line tool can ask the same by touching <data>/learning-reset; the running keyboard notices within a
//     second and drops what it holds without writing it back.
//   * A damaged file is ignored (the model starts empty); the keyboard keeps working.
//
// Never logs words. Nothing here ever touches the network.
#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <functional>
#include <map>
#include <memory>

#include "swipekb/learning.hpp"

class LearningManager : public QObject {
    Q_OBJECT
public:
    // `dataDir` is asked every time (the data folder can change between tests), never cached.
    explicit LearningManager(std::function<QString()> dataDir, QObject *parent = nullptr);
    ~LearningManager() override;

    // Whether the owner switched learning on. Looks at the disk at most once a second.
    bool enabled();
    // The model for this language, or nullptr when learning is off (or the language name is unusable).
    swipekb::PersonalModel *model(const QString &language);
    // Call after changing a model: the file is written a few seconds later (or at once by flush()).
    void changed();
    void flush();
    // Deletes every learned file and empties memory right now.
    void forgetAll();

    static QString flagFile() { return QStringLiteral("learn-typing"); }
    static QString storeDirName() { return QStringLiteral("learning"); }
    static QString resetFile() { return QStringLiteral("learning-reset"); }
    static qint64 now() { return QDateTime::currentSecsSinceEpoch(); }
    QString filePathFor(const QString &language) const;

    // Test hook: how long changed() waits before writing. Default 4 s.
    void setSaveDelayMs(int ms) { m_saveDelay = ms; }
    // Test hook: skip the once-a-second cache of the flag file and reset marker.
    void setNoCache(bool on) { m_noCache = on; }

private:
    struct Slot {
        std::unique_ptr<swipekb::PersonalModel> model;
        QString dir;   // the data folder this was read from
    };
    void checkDisk();
    void dropAll();
    bool saveSlot(const QString &language, Slot &slot);

    std::function<QString()> m_dataDir;
    // WHY std::map: the slots hold a unique_ptr, which QHash cannot copy.
    std::map<QString, Slot> m_slots;
    QTimer m_timer;
    QElapsedTimer m_checked;
    bool m_enabled = false, m_noCache = false, m_resetKnown = false;
    QString m_lastDir;
    qint64 m_resetSeen = 0;
    int m_saveDelay = 4000;
};
