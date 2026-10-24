#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Guest-side fake touchscreen for the Gaming Mode rig (adapted from the spike's touchd.py; runs as root in the guest).

One persistent uinput multi-touch device, driven over the unix socket /tmp/touchd.sock (mode 666), one command per line,
answer 'ok' (or 'err ...') per line. Screen is 1280x800.
  tap X Y [HOLD_MS]            finger down, hold (default 80 ms), up           (slot 0)
  down S X Y | move S X Y | up S    one finger by slot number (0-8): this is how a second finger is made
  drag MS X,Y X,Y ...          slot 0 goes down on the first point, glides along the rest taking MS in total
  sleep MS                     wait (so a whole gesture can be one request with exact timing)
WHY multi-slot (new in M4): the two-finger rule (Shift + letter) can only be tested with two real fingers.
WHY a long-lived device: Steam/gamescope pick a touch device up once; creating one per gesture loses events.
"""
import os
import socket
import time

from evdev import AbsInfo, UInput, ecodes as e

W, H = 1280, 800
cap = {e.EV_KEY: [e.BTN_TOUCH],
       e.EV_ABS: [(e.ABS_X, AbsInfo(0, 0, W - 1, 0, 0, 0)), (e.ABS_Y, AbsInfo(0, 0, H - 1, 0, 0, 0)),
                  (e.ABS_MT_SLOT, AbsInfo(0, 0, 9, 0, 0, 0)), (e.ABS_MT_TRACKING_ID, AbsInfo(0, 0, 65535, 0, 0, 0)),
                  (e.ABS_MT_POSITION_X, AbsInfo(0, 0, W - 1, 0, 0, 0)), (e.ABS_MT_POSITION_Y, AbsInfo(0, 0, H - 1, 0, 0, 0))]}
ui = UInput(cap, name='swipe-test-touch', vendor=0x1234, product=0x5678, input_props=[e.INPUT_PROP_DIRECT])
tid = [10]
down_slots = set()


def down(s, x, y):
    tid[0] += 1
    ui.write(e.EV_ABS, e.ABS_MT_SLOT, s)
    ui.write(e.EV_ABS, e.ABS_MT_TRACKING_ID, tid[0])
    ui.write(e.EV_ABS, e.ABS_MT_POSITION_X, x)
    ui.write(e.EV_ABS, e.ABS_MT_POSITION_Y, y)
    ui.write(e.EV_ABS, e.ABS_X, x)
    ui.write(e.EV_ABS, e.ABS_Y, y)
    if not down_slots:
        ui.write(e.EV_KEY, e.BTN_TOUCH, 1)
    down_slots.add(s)
    ui.syn()


def move(s, x, y):
    ui.write(e.EV_ABS, e.ABS_MT_SLOT, s)
    ui.write(e.EV_ABS, e.ABS_MT_POSITION_X, x)
    ui.write(e.EV_ABS, e.ABS_MT_POSITION_Y, y)
    ui.write(e.EV_ABS, e.ABS_X, x)
    ui.write(e.EV_ABS, e.ABS_Y, y)
    ui.syn()


def up(s):
    ui.write(e.EV_ABS, e.ABS_MT_SLOT, s)
    ui.write(e.EV_ABS, e.ABS_MT_TRACKING_ID, -1)
    down_slots.discard(s)
    if not down_slots:
        ui.write(e.EV_KEY, e.BTN_TOUCH, 0)
    ui.syn()


def run(line):
    p = line.split()
    if not p:
        return
    c = p[0]
    if c == 'tap':
        x, y = int(p[1]), int(p[2])
        down(0, x, y)
        time.sleep((int(p[3]) if len(p) > 3 else 80) / 1000)
        up(0)
    elif c == 'down':
        down(int(p[1]), int(p[2]), int(p[3]))
    elif c == 'move':
        move(int(p[1]), int(p[2]), int(p[3]))
    elif c == 'up':
        up(int(p[1]))
    elif c == 'sleep':
        time.sleep(int(p[1]) / 1000)
    elif c == 'drag':
        ms = float(p[1])
        pts = [tuple(map(int, a.split(','))) for a in p[2:]]
        segs = [(pts[i], pts[i + 1]) for i in range(len(pts) - 1)]
        lens = [max(1, ((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2) ** .5) for a, b in segs]
        tot = sum(lens)
        down(0, *pts[0])
        time.sleep(.03)
        for (a, b), ln in zip(segs, lens):
            n = max(1, int(ms * ln / tot / 8))
            for i in range(1, n + 1):
                move(0, round(a[0] + (b[0] - a[0]) * i / n), round(a[1] + (b[1] - a[1]) * i / n))
                time.sleep(ms * ln / tot / 1000 / n)
        time.sleep(.03)
        up(0)
    else:
        raise ValueError('unknown command ' + c)


P = '/tmp/touchd.sock'
if os.path.exists(P):
    os.unlink(P)
s = socket.socket(socket.AF_UNIX)
s.bind(P)
os.chmod(P, 0o666)
s.listen(4)
time.sleep(1.5)   # let Steam notice the new device before the first touch
while True:
    c, _ = s.accept()
    f = c.makefile('rw')
    for line in f:
        try:
            run(line.strip())
            f.write('ok\n')
        except Exception as ex:   # a bad command must not kill the daemon
            f.write('err %s\n' % ex)
        f.flush()
