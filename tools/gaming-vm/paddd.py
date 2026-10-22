#!/usr/bin/env python3
"""Guest-side fake Xbox-style gamepad for the Gaming Mode rig (runs as root in the guest; M6 tries the real Steam+X chord with it).

One persistent uinput gamepad (Xbox 360 identifiers, so Steam's driver list has a name for it), driven over the unix socket /tmp/padd.sock,
one command per line, answer 'ok' (or 'err ...'):
  chord [LEAD_MS]       the Guide (Steam) button goes down, X goes down and up, Guide goes up: the Steam+X chord
  press NAME [MS]       one button (guide, x, y, a, b, start, select, lb, rb) down for MS (default 120) milliseconds
WHY a long-lived device: Steam picks a controller up once when it appears; creating one per press loses events.
WHY it may not work: Steam on a Deck reads its own controller through a private device; whether it adopts a generic uinput pad is an
experiment, and the M6 report says what happened.
"""
import os
import socket
import time

from evdev import AbsInfo, UInput, ecodes as e

# WHY X is BTN_NORTH: Linux names the Xbox X button BTN_X, which is the same code as BTN_NORTH (and Y is BTN_WEST); xpad and Steam's mapping use that
BTN = {'a': e.BTN_SOUTH, 'b': e.BTN_EAST, 'x': e.BTN_NORTH, 'y': e.BTN_WEST, 'lb': e.BTN_TL, 'rb': e.BTN_TR,
       'select': e.BTN_SELECT, 'start': e.BTN_START, 'guide': e.BTN_MODE}
cap = {e.EV_KEY: list(BTN.values()) + [e.BTN_THUMBL, e.BTN_THUMBR],
       e.EV_ABS: [(a, AbsInfo(0, -32768, 32767, 16, 128, 0)) for a in (e.ABS_X, e.ABS_Y, e.ABS_RX, e.ABS_RY)]
       + [(a, AbsInfo(0, 0, 255, 0, 0, 0)) for a in (e.ABS_Z, e.ABS_RZ)]
       + [(a, AbsInfo(0, -1, 1, 0, 0, 0)) for a in (e.ABS_HAT0X, e.ABS_HAT0Y)]}
ui = UInput(cap, name='Microsoft X-Box 360 pad', vendor=0x045e, product=0x028e, version=0x110, bustype=e.BUS_USB)


def key(name, down):
    ui.write(e.EV_KEY, BTN[name], 1 if down else 0)
    ui.syn()


def handle(line):
    p = line.split()
    if not p:
        return 'ok'
    if p[0] == 'chord':
        lead = int(p[1]) / 1000 if len(p) > 1 else 0.25      # how long Guide is held before X goes down (ms on the command line)
        key('guide', True); time.sleep(lead)
        key('x', True); time.sleep(0.3)
        key('x', False); time.sleep(0.2)
        key('guide', False)
        return 'ok'
    if p[0] == 'press' and len(p) >= 2 and p[1] in BTN:
        key(p[1], True); time.sleep((int(p[2]) if len(p) > 2 else 120) / 1000)
        key(p[1], False)
        return 'ok'
    return 'err bad command'


SOCK = '/tmp/padd.sock'
if os.path.exists(SOCK):
    os.unlink(SOCK)
srv = socket.socket(socket.AF_UNIX)
srv.bind(SOCK)
os.chmod(SOCK, 0o666)
srv.listen(4)
while True:
    c, _ = srv.accept()
    f = c.makefile('rw')
    for line in f:
        try:
            f.write(handle(line) + '\n')
        except Exception as ex:
            f.write('err %s\n' % ex)
        f.flush()
    c.close()
