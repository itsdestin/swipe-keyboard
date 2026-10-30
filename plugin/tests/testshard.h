// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Splits one slow QtTest program into SHARDS that CTest runs side by side (SWIPEKB_TEST_SHARD="i/n").
//
// WHY: the two slowest suites (the Plasma 6.9 controller and the real stock layout) build a fresh keyboard with the
// real English files for every test (about 1.5-2.5 s each, CPU-bound). Run as one program they took 75 s and 51 s
// alone and over 120 s on a busy machine, so they timed out or failed under `ctest -j`. Split in three, each part is
// about a third as long, the parts run in parallel, and every test still runs exactly once.
//
// Rule: test function number k (in declaration order, the ones QtTest would run) belongs to shard k % n. A
// data-driven function named in `rowSharded` runs in every shard instead, and its _data function keeps only the rows
// whose number r has r % n == i (inShard(r)). Without SWIPEKB_TEST_SHARD, or when test names are given on the
// command line, everything runs as before.
#pragma once

#include <QMetaMethod>
#include <QStringList>
#include <QTest>

namespace swipekbtest {

struct Shard {
    int index = 0, count = 1;
};

inline Shard shard()
{
    const auto parts = qEnvironmentVariable("SWIPEKB_TEST_SHARD").split(QLatin1Char('/'));
    bool okIndex = false, okCount = false;
    Shard s;
    if (parts.size() == 2) {
        s.index = parts[0].toInt(&okIndex);
        s.count = parts[1].toInt(&okCount);
    }
    if (!okIndex || !okCount || s.count < 1 || s.index < 0 || s.index >= s.count) return {};
    return s;
}

// For a row-sharded _data function: does row number `row` belong to this shard?
inline bool inShard(int row)
{
    const auto s = shard();
    return row % s.count == s.index;
}

inline int runShard(QObject *test, int argc, char **argv, const QStringList &rowSharded = {})
{
    const auto s = shard();
    if (s.count == 1 || argc > 1) return QTest::qExec(test, argc, argv);
    const QStringList special{QStringLiteral("initTestCase"), QStringLiteral("cleanupTestCase"), QStringLiteral("init"),
                              QStringLiteral("cleanup")};
    QStringList args{QString::fromLocal8Bit(argv[0])};
    const QMetaObject *mo = test->metaObject();
    int k = 0;
    for (int m = mo->methodOffset(); m < mo->methodCount(); ++m) {
        const QMetaMethod method = mo->method(m);
        const QString name = QString::fromLatin1(method.name());
        // The same functions QtTest runs: private slots without arguments, minus the fixture and _data functions.
        if (method.methodType() != QMetaMethod::Slot || method.access() != QMetaMethod::Private || method.parameterCount() != 0
            || special.contains(name) || name.endsWith(QLatin1String("_data")))
            continue;
        if (rowSharded.contains(name) || k++ % s.count == s.index) args << name;
    }
    // WHY run initTestCase alone for an empty shard: no names would mean "run everything".
    if (args.size() == 1) args << QStringLiteral("initTestCase");
    return QTest::qExec(test, args);
}

} // namespace swipekbtest
