// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A text field for the controller tests. It applies edits like a Wayland app does: deletions are UTF-8 BYTES
// around the cursor (input-method-v1), and it reports the new text back (synchronously, or later when `async`).
#pragma once

#include "editorport.h"

#include <QTimer>

class FakeEditor : public EditorPort
{
    Q_OBJECT
public:
    QString m_text;
    int m_cursor = 0;
    Qt::InputMethodHints m_hints;
    bool m_upper = false, m_caps = false, m_hidden = false; // m_hidden: an app that never reports its text
    QString m_locale = QStringLiteral("en_US");
    QStringList log;
    int sentKeys = 0;

    bool active() const override
    {
        return true;
    }
    QString text() const override
    {
        return m_hidden ? QString() : m_text;
    }
    int cursor() const override
    {
        return m_hidden ? 0 : m_cursor;
    }
    int anchor() const override
    {
        return cursor();
    }
    Qt::InputMethodHints hints() const override
    {
        return m_hints;
    }
    bool preedit() const override
    {
        return false;
    }
    bool uppercase() const override
    {
        return m_upper || m_caps;
    }
    bool capsLock() const override
    {
        return m_caps;
    }
    QString locale() const override
    {
        return m_locale;
    }
    void commit(const QString &text, int from, int length) override
    {
        log << QStringLiteral("commit '%1' %2 %3").arg(text).arg(from).arg(length);
        QByteArray bytes = m_text.toUtf8();
        int cursorBytes = m_text.left(m_cursor).toUtf8().size();
        if (length) {
            const int start = cursorBytes + from;
            Q_ASSERT(start >= 0 && start + length <= bytes.size());
            bytes.remove(start, length);
            if (start < cursorBytes) {
                cursorBytes = qMax(start, cursorBytes - length);
            }
        }
        bytes.insert(cursorBytes, text.toUtf8());
        cursorBytes += text.toUtf8().size();
        m_text = QString::fromUtf8(bytes);
        m_cursor = QString::fromUtf8(bytes.left(cursorBytes)).size();
        if (m_upper && !m_caps && !text.isEmpty()) {
            m_upper = false; // like KDE's composer after one capital
        }
        Q_EMIT textChanged();
    }
    void releaseShift() override
    {
        if (!m_caps) {
            m_upper = false;
        }
    }
    bool sendKey(int key, const QString &text) override
    {
        ++sentKeys;
        if (key == Qt::Key_Backspace && m_cursor > 0) {
            m_text.remove(m_cursor - 1, 1);
            --m_cursor;
            Q_EMIT textChanged();
        } else if (!text.isEmpty()) {
            commit(text, 0, 0);
        }
        return true;
    }

    // What KDE's composer does for a tapped key: the press is reported, then the key types, then it reports clicked.
    void tapThrough(QObject *controller, int key, const QString &label)
    {
        QMetaObject::invokeMethod(controller, "keyPressed", Q_ARG(int, key), Q_ARG(QString, label));
        if (key == Qt::Key_Backspace) {
            sendKey(key, {});
        } else {
            const QString typed = uppercase() && label.size() == 1 ? label.toUpper() : label;
            commit(key == Qt::Key_Space ? QStringLiteral(" ") : typed, 0, 0);
        }
        QMetaObject::invokeMethod(controller, "keyClicked", Q_ARG(int, key), Q_ARG(QString, label));
    }
    void setField(const QString &text, int cursor = -1)
    {
        m_text = text;
        m_cursor = cursor < 0 ? int(text.size()) : cursor;
        Q_EMIT textChanged();
    }
    void moveCursor(int cursor)
    {
        m_cursor = cursor;
        Q_EMIT textChanged();
    }
    void setHints(Qt::InputMethodHints hints)
    {
        m_hints = hints;
        Q_EMIT hintsChanged();
    }
};
