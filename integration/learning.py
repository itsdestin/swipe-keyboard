#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Switch "learn from my typing" on or off, and erase what was learned.

The keyboard learns only while the file <data>/learn-typing exists.
What it learned lives in <data>/learning/ (one small private file per language). Nothing here talks to the keyboard
directly: a running keyboard notices the flag within a second, and notices an erase through <data>/learning-reset.

Usage: learning.py on | off | forget | status [--data DIR]
  on      start learning (nothing is learned before this)
  off     stop learning and stop using what was learned; the files stay until `forget`
  forget  erase everything that was learned, now (a running keyboard drops it from memory within a second)
  status  say whether learning is on and how much is stored (sizes only, never words)
"""
import argparse
import os
from pathlib import Path
import shutil
import sys
import time

FLAG = 'learn-typing'
STORE = 'learning'
MARKER = 'learning-reset'


def default_data(env=None):
    env = os.environ if env is None else env
    # Same rule as the keyboard itself (SwipeRecorder::dataDir): SWIPEKB_DATA, else the user's data folder.
    if env.get('SWIPEKB_DATA'):
        return Path(env['SWIPEKB_DATA'])
    base = env.get('XDG_DATA_HOME') or str(Path(env.get('HOME', '~')).expanduser() / '.local/share')
    return Path(base) / 'swipe-keyboard'


def stored_files(data):
    store = Path(data) / STORE
    return sorted(p for p in store.glob('*.swklearn') if p.is_file()) if store.is_dir() else []


def run(action, data, out=print):
    data = Path(data)
    flag = data / FLAG
    if action == 'on':
        data.mkdir(parents=True, exist_ok=True)
        flag.touch()
        out('Learning from your typing is ON. The keyboard now remembers words you type or swipe, on this computer only.')
    elif action == 'off':
        flag.unlink(missing_ok=True)
        out('Learning is OFF. Nothing new is learned and nothing learned is used. '
            'Run "forget" to erase what is stored.')
    elif action == 'forget':
        shutil.rmtree(data / STORE, ignore_errors=True)
        data.mkdir(parents=True, exist_ok=True)
        # WHY a marker file: a keyboard that is running right now holds the words in memory and would write them
        # back; it sees this file's newer time stamp, empties its memory and writes nothing.
        (data / MARKER).write_text(str(time.time()))
        out('Everything the keyboard learned from your typing was erased.')
    elif action == 'status':
        files = stored_files(data)
        size = sum(f.stat().st_size for f in files)
        out('Learning is ' + ('ON' if flag.exists() else 'OFF') + f'. Stored: {len(files)} language file(s), {size} bytes.')
    else:
        raise ValueError(action)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('action', choices=('on', 'off', 'forget', 'status'))
    parser.add_argument('--data', help='the keyboard data folder (default: the one the keyboard uses)')
    args = parser.parse_args(argv)
    return run(args.action, Path(args.data) if args.data else default_data())


if __name__ == '__main__':
    sys.exit(main())
