#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Record peak memory use of a CI build (evidence for parallelism changes).

`sample FILE` appends "epoch used_kib available_kib" every few seconds until killed;
`summary FILE` prints the peak. Reads /proc/meminfo only; changes nothing.
"""
import json
import sys
import time


def read_meminfo(path='/proc/meminfo'):
    values = {}
    for line in open(path):
        key, _, rest = line.partition(':')
        values[key] = int(rest.split()[0])
    return values['MemTotal'] - values['MemAvailable'], values['MemAvailable']


def summarize(lines):
    rows = [line.split() for line in lines if line.strip()]
    used = [int(row[1]) for row in rows]
    available = [int(row[2]) for row in rows]
    return {'samples': len(rows),
            'peak_used_kib': max(used) if used else None,
            'min_available_kib': min(available) if available else None}


def main(argv):
    command, path = argv[1], argv[2]
    if command == 'sample':
        interval = float(argv[3]) if len(argv) > 3 else 5
        with open(path, 'a', buffering=1) as out:
            while True:
                used, available = read_meminfo()
                out.write(f'{int(time.time())} {used} {available}\n')
                time.sleep(interval)
    elif command == 'summary':
        print(json.dumps(summarize(open(path).read().splitlines())))
    else:
        raise SystemExit('usage: ci-memwatch.py sample|summary FILE')


if __name__ == '__main__':
    main(sys.argv)
