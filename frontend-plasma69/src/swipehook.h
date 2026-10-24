// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// SwipeHook — how Swipe Keyboard gets inside Plasma 6.9's keyboard without patching it.
//
// Our launcher puts generated copies of KDE's English/Spanish/French/German keyboard packages first in the data
// path (integration/plasma69.py). Each copy's letter layout is a four-line wrapper around KDE's own file:
//     import SwipeKeyboard69
//     MobileQwerty { id: swipeKeyboardRoot; Component.onCompleted: SwipeHook.watch(swipeKeyboardRoot) }
// watch() then finds KDE's Keyboard item around that layout (by its properties, not by class name) and, for every
// layout of our packages that it shows (letters and symbol pages), adds our suggestion row as the layout's first row
// and our swipe layer above the keys. When anything it relies on is missing, it stays out of the way and says why:
// the keyboard then simply works as KDE's stock keyboard.
#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QStringList>

#include <QQuickItem>
class KeyboardAttachment;

class SwipeHook : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit SwipeHook(QObject *parent = nullptr);

    // Called by our generated layout wrappers. Defers one event-loop turn (KDE may still be building the layout).
    Q_INVOKABLE void watch(QObject *layout);
    // KDE internals this front end needs that are missing around `anchor` (empty = compatible). Logged by watch().
    Q_INVOKABLE QStringList missingInternals(QObject *anchor) const;
    // The swipe layer calls this once a finger has moved far enough: the key under it forgets the press (types nothing).
    Q_INVOKABLE void cancelKeyPress(QQuickItem *key) const;
    // True when KDE's own Space key already moves the cursor (KDE MR !69 or a successor): our slide then steps aside.
    Q_INVOKABLE bool kdeSlidesCursor(QObject *spaceKey) const;
    // Tests: the controller attached to the keyboard around `anchor`, or null.
    Q_INVOKABLE QObject *controllerFor(QObject *anchor) const;

    // The finding rules, public for the compatibility test.
    static QQuickItem *keyboardRoot(QObject *anchor);
    static QQuickItem *layoutLoader(QQuickItem *keyboard);

private:
    void attach(QObject *layout);
    QHash<QQuickItem *, QPointer<KeyboardAttachment>> m_attached;
};

// One per KDE Keyboard item: the controller, and our row + layer on whatever layout is shown.
class KeyboardAttachment : public QObject
{
    Q_OBJECT
public:
    KeyboardAttachment(QQuickItem *keyboard, QQuickItem *loader, const QStringList &packages, QObject *parent);
    ~KeyboardAttachment() override;
    QObject *controller() const;
    void addPackage(const QString &packageId);

public Q_SLOTS:
    void decorate();
    void rebuild();

private:
    void undecorate();
    QPointer<QQuickItem> m_keyboard, m_loader, m_item, m_column, m_strip, m_layer, m_watchedPages;
    QPointer<QObject> m_editor, m_controller;
    QStringList m_packages;
};
