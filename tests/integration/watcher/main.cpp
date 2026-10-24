// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: MIT

#include <KConfigGroup>
#include <KConfigWatcher>
#include <KSharedConfig>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QTextStream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // WHY logical name: this is the KWin subscription the absolute raw writer misses.
    auto config = KSharedConfig::openConfig(QStringLiteral("kwinrc"));
    auto watcher = KConfigWatcher::create(config);
    QObject::connect(watcher.data(), &KConfigWatcher::configChanged, &app,
                     [](const KConfigGroup &group, const QByteArrayList &keys) {
        if (group.name() == QStringLiteral("Wayland") && keys.contains("InputMethod")) {
            QTextStream(stdout) << "Wayland/InputMethod="
                                << group.readEntry("InputMethod", QStringLiteral("ABSENT"))
                                << Qt::endl;
        }
    });
    // Round trip on this connection orders AddMatch before readiness; no activation.
    if (!QDBusConnection::sessionBus().interface()->isServiceRegistered("org.freedesktop.DBus"))
        return 2;
    QTextStream(stdout) << "ready" << Qt::endl;
    return app.exec();
}
