#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""VM-only touchscreen acceptance input, adapted from touch-profile/tests/vtouch.py.

Never use host input injection. This tool refuses a non-QEMU machine and requires
an explicit test marker inside the guest before it opens that guest's /dev/uinput.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import sys
import time

SOCKET = '/run/swipekb-vm-touch/control.sock'
LIMIT = 65536


def guard():
    if (os.geteuid() != 0 or not Path('/etc/swipekb-full-vm-acceptance').is_file() or
            Path('/sys/class/dmi/id/sys_vendor').read_text().strip() != 'QEMU'):
        raise PermissionError('Refusing touch injection outside the marked disposable QEMU guest')


def validate(data, width, height):
    if (type(width) is not int or type(height) is not int or
            not (320 <= width <= 4096 and 240 <= height <= 4096)):
        raise ValueError('Unexpected test display dimensions')
    if not isinstance(data, dict) or data.get('action') not in ('tap', 'swipe', 'stop'):
        raise ValueError('Unknown request action')
    if data['action'] == 'stop':
        return
    if (type(data.get('width')) is not int or type(data.get('height')) is not int or
            (data['width'], data['height']) != (width, height)):
        raise ValueError('Request dimensions must match the daemon display')
    points = data.get('points')
    if not isinstance(points, list) or not 1 <= len(points) <= 128:
        raise ValueError('Use 1..128 coordinate pairs')
    for p in points:
        if (not isinstance(p, (list, tuple)) or len(p) != 2 or
                any(type(v) is not int for v in p) or
                not (0 <= p[0] < width and 0 <= p[1] < height)):
            raise ValueError('Coordinate outside the guest display')
    if (data['action'] == 'tap' and len(points) != 1 or
            data['action'] == 'swipe' and len(points) < 2):
        raise ValueError('Tap takes one coordinate; swipe takes at least two')


def create_device(width, height):
    guard()  # Even direct callers cannot import evdev before the guest guard.
    from evdev import UInput, AbsInfo, ecodes as e
    cap = {e.EV_KEY: [e.BTN_TOUCH], e.EV_ABS: [
        (e.ABS_MT_SLOT, AbsInfo(0, 0, 9, 0, 0, 0)),
        (e.ABS_MT_TRACKING_ID, AbsInfo(0, 0, 65535, 0, 0, 0)),
        *[(code, AbsInfo(0, 0, maximum-1, 0, 0, 12)) for code, maximum in
          [(e.ABS_MT_POSITION_X, width), (e.ABS_MT_POSITION_Y, height),
           (e.ABS_X, width), (e.ABS_Y, height)]]]}
    return UInput(cap, name='swipe-keyboard-disposable-vm-touch', input_props=[e.INPUT_PROP_DIRECT]), e


def release(ui, e):
    ui.write(e.EV_ABS, e.ABS_MT_SLOT, 0)
    ui.write(e.EV_ABS, e.ABS_MT_TRACKING_ID, -1)
    ui.write(e.EV_KEY, e.BTN_TOUCH, 0)
    ui.syn()


def gesture(ui, e, action, points):
    def position(x, y):
        for code, value in [(e.ABS_MT_POSITION_X, x), (e.ABS_MT_POSITION_Y, y),
                            (e.ABS_X, x), (e.ABS_Y, y)]:
            ui.write(e.EV_ABS, code, value)
    try:
        x, y = points[0]
        ui.write(e.EV_ABS, e.ABS_MT_SLOT, 0)
        ui.write(e.EV_ABS, e.ABS_MT_TRACKING_ID, 1)
        position(x, y)
        ui.write(e.EV_KEY, e.BTN_TOUCH, 1)
        ui.syn()
        for nx, ny in points[1:]:
            for step in range(1, 9):
                position(round(x + (nx-x)*step/8), round(y + (ny-y)*step/8))
                ui.syn()
                time.sleep(0.016)
            x, y = nx, ny
        time.sleep(0.06)
    finally:
        release(ui, e)
    time.sleep(0.4)


def receive(connection):
    data = bytearray()
    while len(data) <= LIMIT:
        chunk = connection.recv(min(4096, LIMIT + 1 - len(data)))
        if not chunk:
            raise ValueError('Incomplete JSON message')
        data.extend(chunk)
        if len(data) > LIMIT:
            break
        if data.endswith(b'\n'):
            return json.loads(data)
    raise ValueError('JSON message too large')


def request(data, path=SOCKET):
    guard()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(30)
        connection.connect(path)
        connection.sendall(json.dumps(data).encode() + b'\n')
        response = receive(connection)
    if 'error' in response:
        raise ValueError(response['error'])
    return response


def serve(width, height, path=SOCKET, ready=None):
    guard()
    validate({'action': 'stop'}, width, height)
    Path(path).parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(Path(path).parent, 0o700)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
        server.bind(path)  # Refuse an existing socket rather than stealing a running daemon's path.
        ui = None
        try:
            os.chmod(path, 0o600)
            ui, e = create_device(width, height)
            time.sleep(1.5)  # Discover this one persistent device, once.
            server.listen(4)
            if ready is not None:
                ready.set()
            while True:
                connection, _ = server.accept()
                with connection:
                    connection.settimeout(2)
                    try:
                        data = receive(connection)
                        validate(data, width, height)
                    except (ValueError, UnicodeError, TimeoutError) as error:
                        connection.sendall(json.dumps({'error': str(error)}).encode() + b'\n')
                        continue
                    if data['action'] != 'stop':
                        gesture(ui, e, data['action'], data['points'])
                    else:
                        release(ui, e)
                    connection.sendall(b'{"ok": true}\n')
                    if data['action'] == 'stop':
                        break
        finally:
            try:
                if ui is not None:
                    try:
                        release(ui, e)
                    finally:
                        ui.close()
            finally:
                Path(path).unlink()


def main():
    try:
        guard()
    except PermissionError as error:
        print(error, file=sys.stderr)
        return 2
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('serve', 'tap', 'swipe', 'stop'))
    parser.add_argument('points', nargs='*', help='Physical guest-display pixels as x,y')
    parser.add_argument('--width', type=int, default=1920)
    parser.add_argument('--height', type=int, default=1080)
    args = parser.parse_args()
    try:
        data = dict(action=args.action, width=args.width, height=args.height,
                    points=[tuple(map(int, p.split(','))) for p in args.points])
        if args.action in ('serve', 'stop') and args.points:
            raise ValueError('Serve/stop take no coordinates')
        validate(dict(data, action='stop') if args.action == 'serve' else data, args.width, args.height)
        if args.action == 'serve':
            serve(args.width, args.height)
        else:
            request(data)
    except ValueError as error:
        parser.error(str(error))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
