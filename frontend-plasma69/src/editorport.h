// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// EditorPort — what the swipe controller needs from the text field, and the one class that talks to KDE's new
// keyboard core (plasma-keyboard 6.9 `InputEngine`).
//
// WHY an interface: the controller's rules are tested with a fake field (tests/tst_controller.cpp), and every
// dependency on KDE's internal InputEngine sits in InputEngineEditor below. It reaches the engine only through Qt's
// property/method system (no linking: KDE builds that class into its executable, hidden). Each property or method
// it needs is listed in requiredMembers(), which the startup check and the compatibility test verify.
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

class EditorPort : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    virtual bool active() const = 0; // a text field is focused
    virtual QString text() const = 0; // surrounding text ("" when the app sends none)
    virtual int cursor() const = 0; // UTF-16 index into text()
    virtual int anchor() const = 0;
    virtual Qt::InputMethodHints hints() const = 0;
    virtual bool preedit() const = 0; // a composer holds an underlined word
    virtual bool uppercase() const = 0; // the next letter is a capital (Shift or Caps Lock)
    virtual bool capsLock() const = 0;
    virtual QString locale() const = 0; // the layout's primary locale ("en_US")

    // Deletes `deleteBytesLength` bytes starting `deleteBytesFrom` bytes from the cursor (UTF-8, negative = before it)
    // and then types `text`. Same contract as Wayland input-method-v1, which KDE's engine forwards unchanged.
    virtual void commit(const QString &text, int deleteBytesFrom = 0, int deleteBytesLength = 0) = 0;
    virtual void releaseShift() = 0; // one-shot Shift is used up (Caps Lock stays)
    virtual bool sendKey(int key, const QString &text = {}) = 0; // a key through KDE's normal key path (composer)

Q_SIGNALS:
    void textChanged(); // surrounding text, cursor or anchor
    void hintsChanged(); // field type, or focus moved to another field
    void localeChanged();
};

class InputEngineEditor : public EditorPort
{
    Q_OBJECT
public:
    explicit InputEngineEditor(QObject *inputEngine, QObject *parent = nullptr);

    // Names this class reads from KDE's InputEngine. WHY public: the compatibility check fails loudly on a rename.
    static QStringList requiredMembers();
    // Members of `engine` that are missing (empty = compatible).
    static QStringList missingMembers(const QObject *engine);

    bool active() const override;
    QString text() const override;
    int cursor() const override;
    int anchor() const override;
    Qt::InputMethodHints hints() const override;
    bool preedit() const override;
    bool uppercase() const override;
    bool capsLock() const override;
    QString locale() const override;
    void commit(const QString &text, int deleteBytesFrom, int deleteBytesLength) override;
    void releaseShift() override;
    bool sendKey(int key, const QString &text = {}) override;

    QObject *engine() const
    {
        return m_engine;
    }

private:
    QPointer<QObject> m_engine;
};
