// SwipeInputMethod — Plasma Keyboard's input method while swipe-keyboard is installed. Taps pass through
// untouched (keyEvent returns false, so Qt's normal handling types them). SwipeCapture.qml calls
// finishSwipe() with a finished path; the best guess is typed and the others go to the guess bar.
#pragma once

#include <QtQml/qqmlregistration.h>
#include <QtVirtualKeyboard/QVirtualKeyboardAbstractInputMethod>

#include "swipesession.h"

class SwipeInputMethod : public QVirtualKeyboardAbstractInputMethod {
    Q_OBJECT
    QML_ELEMENT

public:
    explicit SwipeInputMethod(QObject *parent = nullptr);

    QList<QVirtualKeyboardInputEngine::InputMode> inputModes(const QString &locale) override;
    bool setInputMode(const QString &locale, QVirtualKeyboardInputEngine::InputMode inputMode) override;
    bool setTextCase(QVirtualKeyboardInputEngine::TextCase textCase) override;
    bool keyEvent(Qt::Key key, const QString &text, Qt::KeyboardModifiers modifiers) override;

    QList<QVirtualKeyboardSelectionListModel::Type> selectionLists() override;
    int selectionListItemCount(QVirtualKeyboardSelectionListModel::Type type) override;
    QVariant selectionListData(QVirtualKeyboardSelectionListModel::Type type, int index,
                               QVirtualKeyboardSelectionListModel::Role role) override;
    void selectionListItemSelected(QVirtualKeyboardSelectionListModel::Type type, int index) override;

    void reset() override;
    void update() override;

    Q_INVOKABLE bool isReady() const;
    Q_INVOKABLE void finishSwipe(const QVariantList &points, const QVariantList &keys);

private:
    QString textBeforeCursor() const;
    void clearSession();
    void refreshGuessBar();

    void clearAfterSwipe();

    SwipeSession m_session;
    // True while the last thing typed was a swiped word and nothing was tapped since. WHY: apps that hide
    // their text (XWayland/Steam, some Chrome setups) report "" before the cursor, which looks like the
    // start of the field; this flag is how the next swiped word still gets its space.
    bool m_afterSwipe = false;
    // Where the cursor sat just before and just after that swiped word was typed. WHY: update() runs on any
    // text or cursor change, including the app echoing our own word back; only a move somewhere else
    // means the user went elsewhere and the flag no longer applies.
    int m_afterSwipeCursorBefore = -1;
    int m_afterSwipeCursorAfter = -1;
    QVirtualKeyboardInputEngine::TextCase m_textCase = QVirtualKeyboardInputEngine::TextCase::Lower;
};
