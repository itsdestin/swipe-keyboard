// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Read-only loader probe: no GUI, QML engine, keyboard, models or user settings.
#include <QCoreApplication>
#include <QLibrary>
#include <QPluginLoader>
#include <QTextStream>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2)
        return 2;
    QPluginLoader plugin(app.arguments().at(1));
    // WHY eager binding: lazy dlopen can succeed with missing KF/Qt/C++ symbols,
    // then crash only when the keyboard calls them after settings have changed.
    plugin.setLoadHints(QLibrary::ResolveAllSymbolsHint);
    if (!plugin.load()) {
        QTextStream(stderr) << plugin.errorString() << '\n';
        return 1;
    }
    return 0;
}
