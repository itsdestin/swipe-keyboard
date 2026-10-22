// ORACLE for the Gaming Mode daemon (plan milestone M7). It drives the REAL keyboard plugin (SwipeInputMethod)
// through the repository's authored multilingual swipes (bench/languages/synthetic-centered-v1.json) and writes
// the guess-bar words the plugin shows for each one. tests/gaming/test_daemon.py then asks the daemon the same
// questions and requires the same words in the same order. Nothing here judges accuracy: it only records what the
// plugin does, so the daemon can be held to it.
#include <QtTest>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtVirtualKeyboard/QVirtualKeyboardInputContext>
#include "languagefixtures.h"
#include "swipeinputmethod.h"

class LanguageOracleTest : public QObject {
    Q_OBJECT
private slots:
    void recordsPluginGuessBarForEveryAuthoredSwipe() {
        const auto assets = qEnvironmentVariable("SWIPEKB_TEST_ENGLISH_ASSETS"), packs = qEnvironmentVariable("SWIPEKB_TEST_LANGUAGE_PACKS");
        const auto out = qEnvironmentVariable("SWIPEKB_ORACLE_OUT");
        if (assets.isEmpty() || packs.isEmpty() || out.isEmpty()) QSKIP("Explicit English assets, language packs and SWIPEKB_ORACLE_OUT required");
        QFile dataset(SWIPEKB_ORACLE_DATASET);
        QVERIFY(dataset.open(QIODevice::ReadOnly));
        auto cases = QJsonDocument::fromJson(dataset.readAll()).object()["cases"].toArray();
        QVERIFY(cases.size() >= 30);
        // WHY extra swipes defined here: the authored set has no word whose dictionary spelling differs from its
        // swiped letters (its / it's), which is the plugin's "other spelling" rule the daemon must also follow.
        // They reuse the authored English keyboard so they need no new geometry.
        const auto english = cases.first().toObject();
        for (const char *word : {"its", "lets", "im", "dont", "were", "well"}) {
            auto extra = english;
            extra["id"] = QString("en-US-extra-") + word; extra["target"] = word; extra["traceKeys"] = word;
            cases.append(extra);
        }

        QTemporaryDir data;
        const QList<QByteArray> names{"SWIPEKB_MODEL_DATA", "SWIPEKB_LANGUAGE_PACKS", "SWIPEKB_DATA"};
        QHash<QByteArray, QByteArray> old; QHash<QByteArray, bool> existed;
        for (const auto &name : names) { old[name] = qgetenv(name); existed[name] = qEnvironmentVariableIsSet(name.constData()); }
        const auto restore = qScopeGuard([&] { for (const auto &name : names) { if (existed[name]) qputenv(name, old[name]); else qunsetenv(name); } });
        qputenv("SWIPEKB_MODEL_DATA", assets.toUtf8()); qputenv("SWIPEKB_LANGUAGE_PACKS", packs.toUtf8());
        qputenv("SWIPEKB_DATA", data.path().toUtf8());

        QQmlEngine qml;
        QQmlComponent component(&qml);
        component.setData(R"(
            import QtQuick
            import QtQuick.VirtualKeyboard
            Item {
                width: 1000; height: 400
                property QtObject context: InputContext
                property alias text: editor.text
                function focusEditor() { editor.forceActiveFocus() }
                TextInput { id: editor; width: 900; height: 100; focus: true; inputMethodHints: Qt.ImhNone }
            }
        )", QUrl());
        std::unique_ptr<QObject> root(component.create()); QVERIFY2(root, qPrintable(component.errorString()));
        auto *ic = qobject_cast<QVirtualKeyboardInputContext *>(root->property("context").value<QObject *>()); QVERIFY(ic);
        QQuickWindow window; window.resize(1000, 400);
        qobject_cast<QQuickItem *>(root.get())->setParentItem(window.contentItem()); window.show();
        QVERIFY(QMetaObject::invokeMethod(root.get(), "focusEditor")); QTRY_VERIFY(ic->inputItem());
        SwipeInputMethod im;
        ic->inputEngine()->setInputMethod(&im);
        const auto detach = qScopeGuard([&] { ic->inputEngine()->setInputMethod(nullptr); });

        QJsonArray results;
        for (const auto &value : cases) {
            const auto c = value.toObject();
            const auto locale = c["locale"].toString(), layout = c["layout"].toString();
            im.setPhysicalLayout(layout);
            im.setInputMode(locale, QVirtualKeyboardInputEngine::InputMode::Latin);
            QVERIFY2(im.isReady(), qPrintable(locale + ": " + im.profileSnapshot().unavailableReason));
            // Same key table and trace construction as tst_languageintegration (straight lines between key centres).
            QVariantList keys; QMap<QString, QVariantMap> byLetter;
            for (const auto &kv : c["keys"].toArray()) {
                const auto k = kv.toObject();
                QVariantMap m{{"letter", k["letter"].toString()}, {"x", k["x"].toDouble()}, {"y", k["y"].toDouble()},
                              {"w", k["w"].toDouble()}, {"h", k["h"].toDouble()}};
                keys.append(m); byLetter[k["letter"].toString()] = m;
            }
            QVariantList points; QPointF previous;
            for (const auto &letter : c["traceKeys"].toString()) {
                const auto key = byLetter.value(QString(letter));
                QVERIFY2(!key.isEmpty(), qPrintable(c["id"].toString()));
                const QPointF centre(key["x"].toDouble() + key["w"].toDouble() / 2, key["y"].toDouble() + key["h"].toDouble() / 2);
                if (points.isEmpty()) points.append(QVariantMap{{"x", centre.x()}, {"y", centre.y()}, {"t", 0}});
                else for (int step = 1; step <= 8; ++step) {
                    const auto p = previous + (centre - previous) * (step / 8.0);
                    points.append(QVariantMap{{"x", p.x()}, {"y", p.y()}, {"t", (points.size()) * 16}});
                }
                previous = centre;
            }
            root->setProperty("text", QString()); im.reset();
            const int token = im.beginSwipe(); QVERIFY(token >= 0);
            im.finishSwipe(points, keys, token);
            // WHY the committed word comes first: the plugin types the best guess into the field and shows only the
            // OTHER guesses in the bar, so the full ranked list is committed word + bar. (It also capitalises a
            // word at the start of an empty field; the daemon leaves capitals to its caller, so the daemon test
            // compares ignoring case.)
            QJsonArray words;
            words.append(root->property("text").toString().trimmed());
            const int n = im.selectionListItemCount(QVirtualKeyboardSelectionListModel::Type::WordCandidateList);
            for (int i = 0; i < n; ++i)
                words.append(im.selectionListData(QVirtualKeyboardSelectionListModel::Type::WordCandidateList, i,
                                                  QVirtualKeyboardSelectionListModel::Role::Display).toString());
            // The case itself is written out too, so the daemon test replays exactly what the plugin saw.
            results.append(QJsonObject{{"id", c["id"]}, {"locale", locale}, {"layout", layout}, {"words", words},
                                       {"keys", c["keys"]}, {"traceKeys", c["traceKeys"]}});
        }
        QFile file(out);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(QJsonObject{{"schema", 1}, {"cases", results}}).toJson());
    }
};
int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv); LanguageOracleTest test;
    const int result = QTest::qExec(&test, argc, argv);
    return result;
}
#include "tst_languageoracle.moc"
