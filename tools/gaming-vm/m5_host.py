#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Host side of the M5 VM acceptance (plan Section 14, M5). Runs on the HOST, never in the guest.

WHY a host half: the engine daemon is built for this host (Qt, models), not for SteamOS, and the Flatpak that will carry it
to a Deck is M8's job. So in this rig the daemon runs here, in a sandbox, and the guest's helper reaches it through an SSH
forward of the daemon's Unix socket. This keeps the daemon exactly as shipped (no network code in it; the forward is the rig's).

  m5_host.py up        start the daemon (own sandbox folder, own home) + forward its socket into the guest
  m5_host.py down      stop both
  m5_host.py snap NAME  list every file the daemon side could write, with checksums, into the evidence folder
  m5_host.py cmp A B   compare two listings (exit 1 and print the differences if they differ)
  m5_host.py status    ask the daemon how many connections it has (the daemon forgets a keyboard session when its connection closes)
  m5_host.py save-kwrite   press Ctrl+S in the guest (through QEMU's keyboard) so the open editor writes its file

Everything lives under $LAB/m5 (a lab folder outside the repository); screenshots and listings can show private details.
Safety: only PIDs this script started are ever signalled. No sudo.
"""
import hashlib
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

LAB = Path('/home/destin/.cache/swipe-keyboard-platforms')
GUEST = LAB / 'guests' / 'steamos-gaming'
KEY = LAB / 'guests' / 'steamos' / 'id_ed25519'
# WHY env overrides (M6): the same rig serves the next milestones; evidence folder and enabled languages are chosen per run
STATE = LAB / 'evidence' / os.environ.get('SWIPEKB_EVIDENCE', 'gaming-m5')
WORK = Path('/tmp/swipekb-m5-host')            # the daemon's sandbox: run folder, home, xdg folders
ASSETS = os.environ.get('SWIPEKB_ASSETS', '')
DAEMON = os.environ.get('SWIPEKB_GAMINGD', '')
GUEST_RUN = '/home/deck/m5/run'                # where the guest sees the forwarded socket and token


def ssh_base():
    return ['ssh', '-F', '/dev/null', '-i', str(KEY), '-o', 'IdentitiesOnly=yes', '-o', 'ForwardAgent=no', '-o', 'BatchMode=yes',
            '-o', 'StrictHostKeyChecking=accept-new', '-o', 'UserKnownHostsFile=' + str(GUEST / 'known_hosts'), '-p', '22234']


def pidfile(name):
    return STATE / (name + '.pid')


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def up():
    assert DAEMON and ASSETS, 'set SWIPEKB_GAMINGD and SWIPEKB_ASSETS'
    STATE.mkdir(parents=True, exist_ok=True)
    for sub in ('run', 'home/.config', 'home/.cache', 'home/.local/share', 'home/.local/state'):
        (WORK / sub).mkdir(parents=True, exist_ok=True)
    os.chmod(WORK / 'run', 0o700)
    # Only the listed languages are enabled (default English), in a private settings file (the daemon reads the desktop keyboard's list).
    (WORK / 'plasmakeyboardrc').write_text('[General]\nenabledLocales=%s\n' % os.environ.get('SWIPEKB_LOCALES', 'en_US'))
    env = dict(os.environ, HOME=str(WORK / 'home'), XDG_CONFIG_HOME=str(WORK / 'home/.config'), XDG_CACHE_HOME=str(WORK / 'home/.cache'),
               XDG_DATA_HOME=str(WORK / 'home/.local/share'), XDG_STATE_HOME=str(WORK / 'home/.local/state'))
    log = open(STATE / 'daemon.log', 'ab')
    d = subprocess.Popen([DAEMON, '--socket', str(WORK / 'run/engine.sock'), '--token-file', str(WORK / 'run/engine.token'), '--data', ASSETS,
                          '--settings-file', str(WORK / 'plasmakeyboardrc')], env=env, stdout=log, stderr=log)
    pidfile('daemon').write_text(str(d.pid))
    end = time.time() + 20
    while not (WORK / 'run/engine.sock').exists() and time.time() < end:
        time.sleep(0.1)
    # Make the guest-side folder, copy the token (a file only the guest user can read), and forward the socket.
    token = (WORK / 'run/engine.token').read_text()
    subprocess.run(ssh_base() + ['deck@127.0.0.1', 'mkdir -p %s && chmod 700 %s && rm -f %s/engine.sock && cat > %s/engine.token && chmod 600 %s/engine.token'
                                 % ((GUEST_RUN,) * 5)], input=token.encode(), check=True, timeout=30)
    f = subprocess.Popen(ssh_base() + ['-N', '-o', 'ExitOnForwardFailure=yes', '-o', 'StreamLocalBindUnlink=yes', '-R',
                                       '%s/engine.sock:%s' % (GUEST_RUN, WORK / 'run/engine.sock'), 'deck@127.0.0.1'])
    pidfile('forward').write_text(str(f.pid))
    print(json.dumps({'daemon_pid': d.pid, 'forward_pid': f.pid}))


def down():
    for name in ('forward', 'daemon'):
        p = pidfile(name)
        if p.exists():
            pid = int(p.read_text())
            if alive(pid):
                os.kill(pid, signal.SIGTERM)
            p.unlink()
    subprocess.run(ssh_base() + ['deck@127.0.0.1', 'rm -rf %s' % GUEST_RUN], timeout=30)


def snapshot():
    """Everything the daemon side can write: its home, every XDG folder, its run folder. Name -> size and checksum."""
    out = {}
    for root in (WORK / 'home', WORK / 'run'):
        for p in sorted(root.rglob('*')):
            try:
                if p.is_socket():
                    out[str(p)] = ['socket']
                elif p.is_dir():
                    out[str(p)] = ['dir']
                elif p.is_file():
                    out[str(p)] = [p.stat().st_size, hashlib.sha256(p.read_bytes()).hexdigest()]
            except OSError:
                pass
    return out


def daemon_status():
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(10)
    s.connect(str(WORK / 'run/engine.sock'))

    def call(**kw):
        b = json.dumps(dict(v=1, **kw)).encode()
        s.sendall(struct.pack('>I', len(b)) + b)
        n = struct.unpack('>I', s.recv(4))[0]
        buf = b''
        while len(buf) < n:
            buf += s.recv(n - len(buf))
        return json.loads(buf)
    call(op='hello', token=(WORK / 'run/engine.token').read_text().strip(), mode='unknown')
    r = call(op='status')
    s.close()
    return r


def save_kwrite():
    """Ctrl+S through the VM's keyboard (QMP). Focus is whatever the guest has in front; the rig makes sure that is the editor."""
    sock = socket.socket(socket.AF_UNIX)
    sock.settimeout(30)
    sock.connect(str(GUEST / 'qmp.sock'))
    f = sock.makefile('rwb', buffering=0)
    f.readline()

    def call(n, a=None):
        q = {'execute': n}
        if a:
            q['arguments'] = a
        f.write((json.dumps(q) + '\n').encode())
        while True:
            p = json.loads(f.readline())
            if 'error' in p:
                raise RuntimeError(p)
            if 'return' in p:
                return p['return']
    call('qmp_capabilities')
    for key, down_ in (('ctrl', True), ('s', True), ('s', False), ('ctrl', False)):
        call('input-send-event', {'events': [{'type': 'key', 'data': {'down': down_, 'key': {'type': 'qcode', 'data': key}}}]})
        time.sleep(0.1)


def main():
    c = sys.argv[1]
    if c == 'up':
        up()
    elif c == 'down':
        down()
    elif c == 'snap':
        STATE.mkdir(parents=True, exist_ok=True)
        (STATE / ('snap-' + sys.argv[2] + '.json')).write_text(json.dumps(snapshot(), indent=1, sort_keys=True))
        print('files listed:', len(snapshot()))
    elif c == 'cmp':
        a = json.loads((STATE / ('snap-' + sys.argv[2] + '.json')).read_text())
        b = json.loads((STATE / ('snap-' + sys.argv[3] + '.json')).read_text())
        diff = {k: (a.get(k), b.get(k)) for k in set(a) | set(b) if a.get(k) != b.get(k)}
        print(json.dumps({'identical': not diff, 'files_compared': len(a), 'differences': diff}, indent=1))
        sys.exit(1 if diff else 0)
    elif c == 'status':
        print(json.dumps(daemon_status()))
    elif c == 'save-kwrite':
        save_kwrite()
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
