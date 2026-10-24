#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""M3 acceptance rig. Runs INSIDE the SteamOS Gaming Mode guest as the plain user (plan Section 14, M3).

Deploy: the repo's gaming/helper, gaming/contract and this file go to ~/m3/ in the guest (same relative layout).
Usage:  python3 ~/m3/tools/gaming-vm/m3_acceptance.py <command> [N]
  helper-start | helper-stop        run the helper as a temporary user unit (systemd-run --user)
  kills N                           kill Steam by pid N times; time stub install from the page existing
  reloads N                         reload Steam's UI N times (RestartJSContext); time stub install from the new page
  restart-helper                    kill -9 the helper mid-session; it must re-attach without double install
  orphan                            kill -9 with no restart; the stub must remove itself
  marker                            marker deleted (port open), then marker deleted + Steam restarted without it
  status                            print the status file and the page's stub state

WHY it measures from the guest's own clock: the observer, the helper and the page share one clock, so there is
no host/guest skew in the numbers. "Page existing" = the first poll (20 ms) that lists the Big Picture page.
Nothing here types, clicks or changes the Steam account; it only restarts Steam and reloads its UI.
"""
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

HOME = Path.home()
sys.path.insert(0, str(HOME / 'm3' / 'gaming' / 'helper'))
from swipekb_helper import cdp, config, ws  # noqa: E402

PORT = 8080
BP, SJS = 'Steam Big Picture Mode', 'SharedJSContext'
UNIT = 'swipekb-m3-helper'
STATUS = HOME / '.local' / 'state' / 'swipekb-gaming' / 'status.json'
MARKERS = [HOME / '.steam' / 'steam' / '.cef-enable-remote-debugging']
LAYER_JS = ('JSON.stringify(window.__swipekbLayer ? {v: window.__swipekbLayer.version, n: window.__swipekbLayer.installs, '
            'at: window.__swipekbLayer.installedAt, attr: document.documentElement.getAttribute("data-swipekb-stub"), '
            'origin: performance.timeOrigin} : {none: true, origin: performance.timeOrigin})')


def sh(*a, check=False):
    return subprocess.run(a, capture_output=True, text=True, check=check)


def targets():
    try:
        return cdp.http_json(PORT, '/json/list', timeout=1.0)
    except OSError:
        return None


def find(title, lst=None):
    for t in (lst if lst is not None else targets()) or []:
        if t.get('type') == 'page' and t.get('title') == title:
            return t['id']


def steam_pid():
    r = sh('pgrep', '-x', 'steam')
    return int(r.stdout.split()[0]) if r.stdout.split() else None


def page_state(tid, timeout=1.5):
    c = cdp.Cdp.open(PORT, cdp.page_path(tid), timeout=timeout)
    try:
        return json.loads(c.evaluate(LAYER_JS))
    finally:
        c.close()


def status():
    try:
        return json.loads(STATUS.read_text())
    except (OSError, ValueError):
        return {}


def helper_pid():
    r = sh('systemctl', '--user', 'show', '-p', 'MainPID', '--value', UNIT)
    try:
        return int(r.stdout.strip()) or None
    except ValueError:
        return None


def helper_start(restart=True):
    sh('systemctl', '--user', 'reset-failed', UNIT)
    args = ['systemd-run', '--user', '--unit=' + UNIT, '--collect',
            '--setenv=PYTHONPATH=%s' % (HOME / 'm3' / 'gaming' / 'helper'), '--setenv=PYTHONDONTWRITEBYTECODE=1']
    if restart:
        args += ['-p', 'Restart=always', '-p', 'RestartSec=3']
    r = sh(*args, 'python3', '-m', 'swipekb_helper')
    if r.returncode:
        raise SystemExit('could not start helper unit: ' + r.stderr)


def helper_stop():
    sh('systemctl', '--user', 'stop', UNIT)


def wait_until(fn, timeout, step=0.02):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return None


def wait_stub(tid, timeout=15):
    """Poll the page until the stub shows. -> (state, time we saw it)."""
    end = time.time() + timeout
    while time.time() < end:
        try:
            s = page_state(tid)
            if not s.get('none'):
                return s, time.time()
        except (ws.WsError, cdp.CdpError, OSError, ValueError):
            pass
        time.sleep(0.02)
    return None, None


def cmd_kills(n):
    results = []
    for i in range(1, n + 1):
        old_bp = find(BP)
        old_ids = {t['id'] for t in (targets() or [])}
        pid = steam_pid()
        if not pid:
            print('cycle %d: no steam pid' % i)
            return 1
        t0 = time.time()
        os.kill(pid, signal.SIGKILL)               # kill by pid: the Steam client itself, no warning
        port_at = [None]

        def new_page():
            lst = targets()
            if lst is None:
                return None
            # WHY new ids only: the killed Steam's web helper answers for a moment after the kill; the port only
            # counts as reopened once a page that did not exist before shows up.
            if port_at[0] is None and any(t['id'] not in old_ids for t in lst):
                port_at[0] = time.time()
            tid = find(BP, lst)
            return tid if tid and tid != old_bp else None
        tid = wait_until(new_page, 600)
        seen = time.time()
        port_at[0] = port_at[0] or seen
        if not tid:
            print('cycle %d: Steam never came back' % i)
            return 1
        st, st_seen = wait_stub(tid)
        rec = {'cycle': i, 'killed_pid': pid, 'port_open_s': round(port_at[0] - t0, 3),
               'page_after_port_s': round(seen - port_at[0], 3)}
        if st:
            rec['install_after_page_s'] = round(max(0.0, st['at'] / 1000.0 - seen), 3)
            rec['seen_after_page_s'] = round(st_seen - seen, 3)
            rec['install_after_port_s'] = round(max(0.0, st['at'] / 1000.0 - port_at[0]), 3)
            rec['installs'] = st['n']
            rec['attr'] = st['attr']
            rec['ok'] = rec['install_after_page_s'] <= 2.0 and st['n'] == 1 and st['attr'] == st['v']
        else:
            rec['ok'] = False
        print(json.dumps(rec), flush=True)
        results.append(rec)
        # Let Steam settle so the next kill hits a normal running client, not one mid-start.
        wait_until(lambda: find(SJS) and find(BP) and steam_pid(), 60)
        time.sleep(5)
    ok = sum(1 for r in results if r['ok'])
    lat = sorted(r['install_after_page_s'] for r in results if 'install_after_page_s' in r)
    print('KILLS %d/%d ok; install-after-page max %.3f s, median %.3f s' % (ok, len(results), lat[-1] if lat else -1,
                                                                            lat[len(lat) // 2] if lat else -1))
    return 0 if ok == len(results) else 1


def cmd_reloads(n):
    """UI reload the way Steam itself does it (Decky's "reload Steam UI"): SteamClient.Browser.RestartJSContext().
    WHY not Page.reload: in the guest a raw CDP reload of the Big Picture page destroys it and Steam falls back
    to its desktop window until restarted; that is not a reload Steam supports, so it proves nothing."""
    results = []
    for i in range(1, n + 1):
        old_bp, sj = find(BP), find(SJS)
        c = cdp.Cdp.open(PORT, cdp.page_path(sj), timeout=3)
        t0 = time.time()
        try:
            c.call('Runtime.evaluate', {'expression': 'SteamClient.Browser.RestartJSContext()'}, timeout=3)
        except (ws.WsError, cdp.CdpError):
            pass                       # the page we are talking to is the one being replaced
        c.close()
        tid = wait_until(lambda: (lambda t: t if t and t != old_bp else None)(find(BP)), 60)
        seen = time.time()
        rec = {'cycle': i, 'ok': False}
        if tid:
            st, st_seen = wait_stub(tid, 10)
            if st:
                rec.update(page_after_reload_cmd_s=round(seen - t0, 3),
                           install_after_page_s=round(max(0.0, st['at'] / 1000.0 - seen), 3),
                           seen_after_page_s=round(st_seen - seen, 3), installs=st['n'], attr=st['attr'])
                rec['ok'] = rec['install_after_page_s'] <= 2.0 and st['n'] == 1 and st['attr'] == st['v']
        print(json.dumps(rec), flush=True)
        results.append(rec)
        time.sleep(6)    # let the reloaded UI finish loading before the next one
    ok = sum(1 for r in results if r['ok'])
    print('RELOADS %d/%d ok' % (ok, len(results)))
    return 0 if ok == len(results) else 1


def cmd_restart_helper():
    tid = find(BP)
    before = page_state(tid)
    pid = helper_pid()
    t0 = time.time()
    os.kill(pid, signal.SIGKILL)
    print('killed helper pid', pid, 'stub still there:', not page_state(tid).get('none'))
    new = wait_until(lambda: (helper_pid() not in (None, pid)) and helper_pid(), 20)
    wait_until(lambda: status().get('pid') == new and status().get('state') == 'active', 20)
    after = page_state(tid)
    print(json.dumps({'old_pid': pid, 'new_pid': new, 'back_in_s': round(time.time() - t0, 2), 'status': status().get('state'),
                      'before': before, 'after': after}))
    ok = new and status().get('state') == 'active' and after['n'] == 1 and after['at'] == before['at']
    print('RESTART-HELPER', 'ok' if ok else 'FAILED', '(re-attached, same install, no double install)')
    return 0 if ok else 1


def cmd_orphan():
    """kill -9 with nobody to restart the helper: the stub must take itself out (watchdog)."""
    tid = find(BP)
    helper_stop()
    helper_start(restart=False)
    wait_until(lambda: status().get('state') == 'active' and helper_pid() == status().get('pid'), 20)
    assert not page_state(tid).get('none'), 'stub missing before the test'
    os.kill(helper_pid(), signal.SIGKILL)
    t1 = time.time()
    gone = wait_until(lambda: page_state(tid).get('none'), 15, 0.1)
    print(json.dumps({'self_removed_after_s': round(time.time() - t1, 2) if gone else None}))
    helper_start()      # back to the normal restarting unit for the next test
    print('ORPHAN', 'ok' if gone else 'FAILED')
    return 0 if gone else 1


def cmd_marker():
    # Case 1: marker deleted while Steam is up (port open): recreated, still active.
    helper_stop()
    helper_start()
    wait_until(lambda: status().get('state') == 'active' and status().get('pid') == helper_pid(), 20)
    for m in MARKERS:
        m.unlink()
    t0 = time.time()
    back = wait_until(lambda: MARKERS[0].exists(), 10, 0.05)
    print('case1 recreated after %.2f s, status %s' % (time.time() - t0, status().get('state')))
    ok1 = bool(back) and status().get('state') == 'active'
    # Case 2: stop helper, delete marker, kill Steam so it starts without the switch; start helper.
    helper_stop()
    MARKERS[0].unlink()
    pid = steam_pid()
    os.kill(pid, signal.SIGKILL)
    up = wait_until(lambda: steam_pid() not in (None, pid), 90)
    time.sleep(3)
    port_closed = targets() is None
    print('case2 steam back (pid %s), port closed: %s' % (up, port_closed))
    helper_start()
    wait_until(lambda: status().get('state') in ('needs-steam-restart', 'active') and status().get('pid') == helper_pid(), 30)
    rec = MARKERS[0].exists()
    st = status().get('state')
    print('case2 marker recreated: %s, status: %s' % (rec, st))
    ok2 = rec and port_closed and st == 'needs-steam-restart'
    # Recovery: restart Steam; the port must come back and the layer install.
    old = steam_pid()
    os.kill(old, signal.SIGKILL)
    tid = wait_until(lambda: find(BP), 120)
    stub, _ = wait_stub(tid, 20) if tid else (None, None)
    wait_until(lambda: status().get('state') == 'active', 20)
    print('case2 after Steam restart: stub %s, status %s' % (bool(stub), status().get('state')))
    ok3 = bool(stub) and status().get('state') == 'active'
    print('MARKER', 'ok' if ok1 and ok2 and ok3 else 'FAILED', ok1, ok2, ok3)
    return 0 if ok1 and ok2 and ok3 else 1


def cmd_status():
    print(json.dumps(status()))
    tid = find(BP)
    print(json.dumps(page_state(tid)) if tid else 'no BP page')


if __name__ == '__main__':
    c = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    sys.exit({'helper-start': lambda: helper_start() or 0, 'helper-stop': lambda: helper_stop() or 0,
              'kills': lambda: cmd_kills(n), 'reloads': lambda: cmd_reloads(n), 'restart-helper': cmd_restart_helper,
              'orphan': cmd_orphan, 'marker': cmd_marker, 'status': lambda: cmd_status() or 0}[c]())
