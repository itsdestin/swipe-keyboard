// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#include "editorport.h"

#include <QMetaMethod>
#include <QVariant>

namespace
{
// WHY one list for reads, signals and invokables: the startup check (SwipeHook) and the compatibility test both use it,
// so a KDE rename cannot slip past one of them.
const char *const kProperties[] = {"surroundingText", "cursorPosition", "anchorPosition", "inputMethodHints", "preeditText",
                                   "uppercase", "capsLockActive", "shiftActive", "locale"};
const char *const kSignals[] = {"surroundingTextChanged()", "cursorPositionChanged()", "anchorPositionChanged()", "inputMethodHintsChanged()",
                                "localeChanged()"};
const char *const kMethods[] = {"commit(QString,int,int)", "sendTextComposerKey(int,QString)"};
} // namespace

InputEngineEditor::InputEngineEditor(QObject *inputEngine, QObject *parent)
    : EditorPort(parent)
    , m_engine(inputEngine)
{
    if (!m_engine) {
        return;
    }
    // WHY string-based connections: the engine's class is not visible to us at compile time (it lives inside
    // KDE's executable); its signals are found by name at run time, like QML does.
    connect(m_engine, SIGNAL(surroundingTextChanged()), this, SIGNAL(textChanged()));
    connect(m_engine, SIGNAL(cursorPositionChanged()), this, SIGNAL(textChanged()));
    connect(m_engine, SIGNAL(anchorPositionChanged()), this, SIGNAL(textChanged()));
    connect(m_engine, SIGNAL(inputMethodHintsChanged()), this, SIGNAL(hintsChanged()));
    connect(m_engine, SIGNAL(localeChanged()), this, SIGNAL(localeChanged()));
}

QStringList InputEngineEditor::requiredMembers()
{
    QStringList out;
    for (const char *p : kProperties) {
        out << QStringLiteral("property ") + QLatin1String(p);
    }
    for (const char *s : kSignals) {
        out << QStringLiteral("signal ") + QLatin1String(s);
    }
    for (const char *m : kMethods) {
        out << QStringLiteral("method ") + QLatin1String(m);
    }
    return out;
}

QStringList InputEngineEditor::missingMembers(const QObject *engine)
{
    if (!engine) {
        return {QStringLiteral("InputEngine object")};
    }
    const QMetaObject *meta = engine->metaObject();
    QStringList missing;
    for (const char *p : kProperties) {
        if (meta->indexOfProperty(p) < 0) {
            missing << QStringLiteral("property ") + QLatin1String(p);
        }
    }
    for (const char *s : kSignals) {
        if (meta->indexOfSignal(s) < 0) {
            missing << QStringLiteral("signal ") + QLatin1String(s);
        }
    }
    for (const char *m : kMethods) {
        if (meta->indexOfMethod(m) < 0) {
            missing << QStringLiteral("method ") + QLatin1String(m);
        }
    }
    return missing;
}

bool InputEngineEditor::active() const
{
    return m_engine;
}

QString InputEngineEditor::text() const
{
    return m_engine ? m_engine->property("surroundingText").toString() : QString();
}

int InputEngineEditor::cursor() const
{
    return m_engine ? m_engine->property("cursorPosition").toInt() : -1;
}

int InputEngineEditor::anchor() const
{
    return m_engine ? m_engine->property("anchorPosition").toInt() : -1;
}

Qt::InputMethodHints InputEngineEditor::hints() const
{
    if (!m_engine) {
        // WHY fail closed: without an engine nothing may be read or learned.
        return Qt::ImhSensitiveData | Qt::ImhHiddenText;
    }
    const QVariant v = m_engine->property("inputMethodHints");
    if (v.canConvert<Qt::InputMethodHints>()) {
        return v.value<Qt::InputMethodHints>();
    }
    bool ok = false;
    const int raw = v.toInt(&ok);
    return ok ? Qt::InputMethodHints(raw) : Qt::InputMethodHints(Qt::ImhSensitiveData | Qt::ImhHiddenText);
}

bool InputEngineEditor::preedit() const
{
    return m_engine && !m_engine->property("preeditText").toString().isEmpty();
}

bool InputEngineEditor::uppercase() const
{
    return m_engine && m_engine->property("uppercase").toBool();
}

bool InputEngineEditor::capsLock() const
{
    return m_engine && m_engine->property("capsLockActive").toBool();
}

QString InputEngineEditor::locale() const
{
    return m_engine ? m_engine->property("locale").toString() : QString();
}

void InputEngineEditor::commit(const QString &text, int deleteBytesFrom, int deleteBytesLength)
{
    if (m_engine) {
        QMetaObject::invokeMethod(m_engine, "commit", Qt::DirectConnection, Q_ARG(QString, text), Q_ARG(int, deleteBytesFrom), Q_ARG(int, deleteBytesLength));
    }
}

void InputEngineEditor::releaseShift()
{
    // WHY by hand: KDE's engine drops one-shot Shift only for keys it typed itself (handleKeyCommit).
    if (m_engine && !capsLock() && m_engine->property("shiftActive").toBool()) {
        m_engine->setProperty("shiftActive", false);
    }
}

bool InputEngineEditor::sendKey(int key, const QString &text)
{
    bool ok = false;
    if (m_engine) {
        QMetaObject::invokeMethod(m_engine, "sendTextComposerKey", Qt::DirectConnection, Q_RETURN_ARG(bool, ok), Q_ARG(int, key), Q_ARG(QString, text));
    }
    return ok;
}
