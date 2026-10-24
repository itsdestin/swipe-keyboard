// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Ordinary Wayland text/password fields for the disposable full-KDE VM acceptance test.
// D-Bus observes committed text; it never injects characters or loads the keyboard plugin into the client.
#include <QGuiApplication>
#include <QDBusConnection>
#include <QQuickWindow>
#include <QQuickItem>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QFile>
#include <QTimer>
#include <memory>
#include <cstdio>

class Acceptance : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.youcoded.KeyboardAcceptance")
public:
    explicit Acceptance(QQuickItem *root) : m_root(root) {}
public slots:
    QString text() const { return m_root->property("typedText").toString(); }
    int passwordLength() const { return m_root->property("passwordLength").toInt(); }
    bool focused() const { return m_root->property("editorFocused").toBool(); }
    void clear() {
        m_root->setProperty("typedText", QString());
        m_root->setProperty("passwordText", QString());
    }
    // WHY timed focus: reproduce an app moving focus while a real guest touch is still down.
    // These controls never inject text or call into the input method.
    void focusPasswordAfter(int milliseconds) {
        QTimer::singleShot(qBound(0, milliseconds, 10000), m_root, [this] {
            QMetaObject::invokeMethod(m_root, "focusPassword");
        });
    }
    void focusEditor() { QMetaObject::invokeMethod(m_root, "focusEditor"); }
    void selectFollowingText() { QMetaObject::invokeMethod(m_root, "selectFollowingText"); }
    QString selectedText() const { return m_root->property("selectedText").toString(); }
    void quit() { QGuiApplication::quit(); }
private:
    QQuickItem *m_root;
};

int main(int argc, char **argv)
{
    QFile vendor(QStringLiteral("/sys/class/dmi/id/sys_vendor"));
    if (!QFile::exists(QStringLiteral("/etc/swipekb-full-vm-acceptance")) ||
        !vendor.open(QIODevice::ReadOnly) || vendor.readAll().trimmed() != "QEMU") {
        std::fprintf(stderr, "Refusing to run outside the marked disposable QEMU guest\n");
        return 2;
    }
    QGuiApplication app(argc, argv);
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        Rectangle {
            width: parent ? parent.width : 1280
            height: parent ? parent.height : 900
            color: "#eef2f7"
            property alias typedText: editor.text
            property alias passwordText: secret.text
            readonly property int passwordLength: secret.text.length
            property bool editorFocused: editor.activeFocus
            readonly property string selectedText: editor.selectedText
            function focusPassword() { secret.forceActiveFocus() }
            function focusEditor() { editor.forceActiveFocus() }
            // Backward selection keeps its cursor after the first word; no text is synthesized.
            function selectFollowingText() {
                var start = editor.text.indexOf(" ")
                if (start > 0) editor.select(editor.text.length, start)
            }
            Text { x: 56; y: 36; text: "Swipe Keyboard · isolated KDE test"; font.pixelSize: 30; color: "#17243b" }
            Text { x: 56; y: 84; text: "Packaged keyboard → real KWin input method → ordinary Wayland fields"; font.pixelSize: 18; color: "#52647b" }
            Text { x: 56; y: 124; text: "Normal text"; font.pixelSize: 17; color: "#52647b" }
            Rectangle {
                x: 56; y: 154; width: parent.width - 112; height: 104
                color: "white"; border.color: "#9aaec8"; border.width: 2; radius: 10
                TextEdit {
                    id: editor; anchors.fill: parent; anchors.margins: 20
                    font.pixelSize: 28; color: "#17243b"; wrapMode: TextEdit.Wrap
                    selectByMouse: true
                }
            }
            Text { x: 56; y: 286; text: "Password field · synthetic test only"; font.pixelSize: 17; color: "#52647b" }
            Rectangle {
                x: 56; y: 316; width: parent.width - 112; height: 84
                color: "white"; border.color: "#9aaec8"; border.width: 2; radius: 10
                TextInput {
                    id: secret; anchors.fill: parent; anchors.margins: 20
                    font.pixelSize: 28; color: "#17243b"; echoMode: TextInput.Password
                    inputMethodHints: Qt.ImhHiddenText | Qt.ImhSensitiveData | Qt.ImhNoPredictiveText
                }
            }
            Text { x: 56; y: 430; text: "Touch the normal field to open the keyboard. No private text is used."; font.pixelSize: 18; color: "#52647b" }
        }
    )", QUrl());
    std::unique_ptr<QQuickItem> item(qobject_cast<QQuickItem *>(component.create()));
    if (!item) {
        std::fprintf(stderr, "Text client QML failed: %s\n", qPrintable(component.errorString()));
        return 1;
    }
    QQuickWindow view;
    view.setTitle(QStringLiteral("Swipe Keyboard — isolated acceptance"));
    view.resize(1280, 900);
    item->setParentItem(view.contentItem());
    Acceptance acceptance(item.get());
    if (!QDBusConnection::sessionBus().registerService(QStringLiteral("org.youcoded.KeyboardAcceptance")) ||
        !QDBusConnection::sessionBus().registerObject(QStringLiteral("/Acceptance"), &acceptance,
                                                     QDBusConnection::ExportAllSlots)) {
        std::fprintf(stderr, "Test-account D-Bus registration failed\n");
        return 1;
    }
    view.showFullScreen();
    return app.exec();
}

#include "session-client.moc"
