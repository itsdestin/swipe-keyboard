# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""A fake Steam debug port for the helper tests (stdlib + Node).

It speaks just the parts of Chrome DevTools the helper uses, as measured in the SteamOS guest (plan M0):
  GET /json/version, GET /json/list, a browser websocket with Target.setDiscoverTargets (replay of existing
  targets, then targetCreated / targetInfoChanged / targetDestroyed), and one websocket per page with
  Runtime.evaluate and Page.addScriptToEvaluateOnNewDocument.
Page code runs in real Node vm contexts (fake_page.js). Like Chromium, a script registered with
addScriptToEvaluateOnNewDocument lives only as long as the websocket session that registered it.

The websocket framing here is written independently of the helper's client on purpose: a shared bug would
otherwise cancel itself out.
"""
import base64
import hashlib
import json
import os
import socket
import struct
import subprocess
import threading
import time

GUID = b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
PAGE_JS = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fake_page.js')


# ---- minimal server-side websocket framing -------------------------------------------------------------

def read_exact(sock, n):
    buf = b''
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError('closed')
        buf += chunk
    return buf


def read_frame(sock):
    """-> (fin, opcode, payload) of one client frame (clients must mask)."""
    b1, b2 = read_exact(sock, 2)
    fin, opcode = bool(b1 & 0x80), b1 & 0x0F
    n = b2 & 0x7F
    if n == 126:
        n = struct.unpack('>H', read_exact(sock, 2))[0]
    elif n == 127:
        n = struct.unpack('>Q', read_exact(sock, 8))[0]
    assert b2 & 0x80, 'client frames must be masked'
    mask = read_exact(sock, 4)
    data = bytearray(read_exact(sock, n))
    for i in range(n):
        data[i] ^= mask[i % 4]
    return fin, opcode, bytes(data)


def frame(opcode, payload, fin=True):
    head = bytes([(0x80 if fin else 0) | opcode])
    n = len(payload)
    if n < 126:
        head += bytes([n])
    elif n < 65536:
        head += b'\x7e' + struct.pack('>H', n)
    else:
        head += b'\x7f' + struct.pack('>Q', n)
    return head + payload


def handshake_reply(headers):
    key = headers['sec-websocket-key'].encode()
    acc = base64.b64encode(hashlib.sha1(key + GUID).digest()).decode()
    return ('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
            'Sec-WebSocket-Accept: %s\r\n\r\n' % acc).encode()


def read_http_request(sock):
    buf = b''
    while b'\r\n\r\n' not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError('closed')
        buf += chunk
    head = buf.split(b'\r\n\r\n')[0].decode('latin-1')
    lines = head.split('\r\n')
    method, path, _ = lines[0].split(' ', 2)
    headers = {}
    for ln in lines[1:]:
        k, _, v = ln.partition(':')
        headers[k.strip().lower()] = v.strip()
    return method, path, headers


# ---- one fake page --------------------------------------------------------------------------------------

class Page:
    def __init__(self, target_id, steam):
        self.id = target_id
        self.proc = subprocess.Popen(['node', PAGE_JS], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.lock = threading.Lock()
        self.seq = 0
        self.scripts = {}        # session id -> [script, ...]   (gone when that session closes)
        self.eval_log = []       # expressions received, for assertions
        self.bindings = {}       # binding name -> (session id, send function): where Runtime.bindingCalled goes
        self.alive = True
        # Failure injection (M9). eval_filter(expr) -> None, or the text of the error Chrome would give for a refused
        # evaluation (a blocked-eval/CSP page). refuse: CDP methods answered with an error object instead of a result.
        self.eval_filter = None
        self.refuse = set()
        self._rpc({'op': 'init', 'steam': steam})
        threading.Thread(target=self._drain, daemon=True).start()

    def _drain(self):
        """Chrome pushes a binding call to the session that registered it; here the page's calls are collected by polling."""
        while self.alive:
            time.sleep(0.004)
            try:
                got = self._rpc({'op': 'drain'}).get('out', [])
            except (ValueError, OSError):
                return
            for name, payload in got:
                b = self.bindings.get(name)
                if b:
                    try:
                        b[1]({'method': 'Runtime.bindingCalled', 'params': {'name': name, 'payload': payload, 'executionContextId': 1}})
                    except OSError:
                        pass

    def _rpc(self, msg):
        with self.lock:
            self.seq += 1
            msg['id'] = self.seq
            self.proc.stdin.write(json.dumps(msg) + '\n')
            self.proc.stdin.flush()
            return json.loads(self.proc.stdout.readline())

    def eval(self, expr):
        """-> (ok, value_or_error). Logs the expression."""
        self.eval_log.append(expr)
        try:
            r = self._rpc({'op': 'eval', 'expr': expr})
        except (ValueError, OSError):
            return False, 'page gone'      # the page process was killed while a client was still talking to it
        return r['ok'], r.get('value', r.get('error'))

    def reload(self, run_scripts=True):
        scripts = [s for lst in self.scripts.values() for s in lst] if run_scripts else []
        self._rpc({'op': 'reset', 'scripts': scripts})

    def drop_session(self, session):
        self.scripts.pop(session, None)
        self.bindings = {n: b for n, b in self.bindings.items() if b[0] != session}

    def kill(self):
        self.alive = False
        try:
            self.proc.stdin.close()
            self.proc.terminate()
            self.proc.wait(timeout=5)
            self.proc.stdout.close()
        except Exception:
            pass


# ---- the fake debug port --------------------------------------------------------------------------------

class FakeSteam:
    def __init__(self):
        self.port = None
        self.browser_string = 'Chrome/126.0.0.0'
        self.targets = {}        # id -> {'info': {...}, 'page': Page}
        self.browser_sessions = []
        self.conns = []
        self.lock = threading.RLock()
        self.listener = None
        self.next_id = 0
        self.connections_seen = 0
        self.page_sessions = 0
        self.listing_url_override = None

    # -- listening
    def open(self):
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(('127.0.0.1', self.port or 0))
        s.listen(16)
        self.port = s.getsockname()[1]
        self.listener = s
        threading.Thread(target=self._accept, args=(s,), daemon=True).start()

    def _accept(self, s):
        while True:
            try:
                c, _ = s.accept()
            except OSError:
                return
            self.connections_seen += 1
            with self.lock:
                self.conns.append(c)
            threading.Thread(target=self._serve, args=(c,), daemon=True).start()

    def close_port(self):
        """Steam is down: nothing listens and every open websocket dies."""
        if self.listener:
            self.listener.close()
            self.listener = None
        with self.lock:
            for c in self.conns:
                try:
                    c.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                c.close()
            self.conns = []
            self.browser_sessions = []

    def stop(self):
        self.close_port()
        for t in list(self.targets.values()):
            t['page'].kill()
        self.targets = {}

    # -- targets
    def add_target(self, title, ttype='page', steam=None, announce=True, url=None):
        with self.lock:
            self.next_id += 1
            tid = 'T%04d%s' % (self.next_id, os.urandom(4).hex().upper())
            info = {'targetId': tid, 'type': ttype, 'title': title,
                    'url': url or 'https://steamloopback.host/index.html', 'attached': False}
            self.targets[tid] = {'info': info, 'page': Page(tid, steam)}
        if announce:
            self.broadcast('Target.targetCreated', {'targetInfo': dict(info)})
        return tid

    def set_title(self, tid, title):
        with self.lock:
            self.targets[tid]['info']['title'] = title
            info = dict(self.targets[tid]['info'])
        self.broadcast('Target.targetInfoChanged', {'targetInfo': info})

    def remove_target(self, tid):
        with self.lock:
            t = self.targets.pop(tid)
        t['page'].kill()
        self.broadcast('Target.targetDestroyed', {'targetId': tid})

    def page(self, tid):
        return self.targets[tid]['page']

    def eval(self, tid, expr):
        return self.page(tid).eval(expr)

    def reload(self, tid, run_scripts=True):
        self.page(tid).reload(run_scripts)
        with self.lock:
            info = dict(self.targets[tid]['info'])
        self.broadcast('Target.targetInfoChanged', {'targetInfo': info})

    def find(self, title):
        with self.lock:
            return [t for t, v in self.targets.items() if v['info']['title'] == title]

    # -- events
    def broadcast(self, method, params):
        msg = json.dumps({'method': method, 'params': params}).encode()
        with self.lock:
            sessions = list(self.browser_sessions)
        for s in sessions:
            try:
                s.sendall(frame(1, msg))
            except OSError:
                pass

    # -- per-connection server
    def _serve(self, c):
        try:
            method, path, headers = read_http_request(c)
            if headers.get('upgrade', '').lower() == 'websocket':
                c.sendall(handshake_reply(headers))
                if path.startswith('/devtools/browser/'):
                    self._browser_ws(c)
                elif path.startswith('/devtools/page/'):
                    self._page_ws(c, path.rsplit('/', 1)[1])
                return
            host = headers.get('host', '')
            assert host.startswith('127.0.0.1:'), host
            if path == '/json/version':
                body = {'Browser': self.browser_string, 'Protocol-Version': '1.3',
                        'webSocketDebuggerUrl': 'ws://127.0.0.1:%d/devtools/browser/BROWSER' % self.port}
            elif path in ('/json/list', '/json'):
                with self.lock:
                    body = [{'id': t, 'type': v['info']['type'], 'title': v['info']['title'],
                             'url': v['info']['url'],
                             'webSocketDebuggerUrl': (self.listing_url_override or 'ws://127.0.0.1:%d/devtools/page/%%s' % self.port) % t}
                            for t, v in self.targets.items()]
            else:
                c.sendall(b'HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n')
                return
            data = json.dumps(body).encode()
            c.sendall(b'HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n'
                      b'Connection: close\r\n\r\n' % len(data) + data)
        except (ConnectionError, OSError):
            pass
        finally:
            try:
                c.close()
            except OSError:
                pass

    def _browser_ws(self, c):
        while True:
            fin, op, payload = read_frame(c)
            if op == 8:
                return
            if op != 1:
                continue
            m = json.loads(payload)
            if m['method'] == 'Target.setDiscoverTargets':
                c.sendall(frame(1, json.dumps({'id': m['id'], 'result': {}}).encode()))
                with self.lock:
                    self.browser_sessions.append(c)
                    infos = [dict(v['info']) for v in self.targets.values()]
                for i in infos:
                    c.sendall(frame(1, json.dumps({'method': 'Target.targetCreated',
                                                   'params': {'targetInfo': i}}).encode()))
            else:
                c.sendall(frame(1, json.dumps({'id': m['id'], 'result': {}}).encode()))

    def _page_ws(self, c, tid):
        with self.lock:
            t = self.targets.get(tid)
        if not t:
            return
        page = t['page']
        session = id(c)
        wlock = threading.Lock()

        def send(obj):
            # one writer at a time: answers and pushed events share this socket
            with wlock:
                c.sendall(frame(1, json.dumps(obj).encode()))
        self.page_sessions += 1
        try:
            while True:
                fin, op, payload = read_frame(c)
                if op == 8:
                    return
                if op == 9:
                    with wlock:
                        c.sendall(frame(10, payload))
                    continue
                if op != 1:
                    continue
                m = json.loads(payload)
                meth, p = m['method'], m.get('params', {})
                if meth in page.refuse:
                    send({'id': m['id'], 'error': {'code': -32000, 'message': 'Refused: %s is not allowed on this page' % meth}})
                elif meth == 'Runtime.evaluate' and page.eval_filter and page.eval_filter(p['expression']):
                    page.eval_log.append(p['expression'])
                    send({'id': m['id'], 'result': {'result': {'type': 'object'}, 'exceptionDetails': {
                        'text': 'Uncaught', 'exception': {'description': 'EvalError: ' + page.eval_filter(p['expression'])}}}})
                elif meth == 'Runtime.evaluate':
                    ok, val = page.eval(p['expression'])
                    if ok:
                        res = {'result': {'type': type(val).__name__, 'value': val}}
                        if val is None:
                            res = {'result': {'type': 'undefined'}}
                    else:
                        res = {'result': {'type': 'object'}, 'exceptionDetails': {'text': 'Uncaught', 'exception': {'description': str(val)}}}
                    send({'id': m['id'], 'result': res})
                elif meth == 'Page.addScriptToEvaluateOnNewDocument':
                    page.scripts.setdefault(session, []).append(p['source'])
                    send({'id': m['id'], 'result': {'identifier': '1'}})
                elif meth == 'Runtime.addBinding':
                    page._rpc({'op': 'binding', 'name': p['name']})
                    page.bindings[p['name']] = (session, send)
                    page.binding_sessions = getattr(page, 'binding_sessions', 0) + 1
                    send({'id': m['id'], 'result': {}})
                else:
                    send({'id': m['id'], 'result': {}})
        finally:
            page.drop_session(session)


def wait_for(cond, timeout=10.0, step=0.01, what='condition'):
    """Wait on real state (never a fixed sleep standing in for a signal)."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        v = cond()
        if v:
            return v
        time.sleep(step)
    raise AssertionError('timed out waiting for ' + what)
