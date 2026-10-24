# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""A stand-in for swipekb-gamingd for the helper's typing tests (plan M5): same framing and same rules about candidate ids as the real
daemon (protocol.hpp): ids are issued per swipe request, valid for the latest request on that connection only, and `commit` answers
with the word only for an issued id. Every call is recorded so tests can see exactly what the helper asked for."""
import json
import os
import socket
import struct
import threading


class FakeDaemon:
    def __init__(self, tmp, words=None, token='t' * 32):
        self.dir = os.path.join(tmp, 'run')
        os.makedirs(self.dir, mode=0o700, exist_ok=True)
        self.sock_path = os.path.join(self.dir, 'engine.sock')
        self.token_path = os.path.join(self.dir, 'engine.token')
        self.token = token
        with open(self.token_path, 'w') as f:
            f.write(token + '\n')
        os.chmod(self.token_path, 0o600)       # the real daemon makes it owner-only; the helper refuses anything wider
        self.words = words or ['hello', 'hell', 'jello']      # what every swipe answers, best first
        self.calls = []              # (conn_no, op, fields)
        self.connections = 0
        self.open_conns = 0
        self.lock = threading.Lock()
        self.reply_delay = 0.0
        self.layout_error = None     # e.g. 'unsupported'
        self.raw_words = None        # replace the words in `commit` replies (to prove the helper distrusts the daemon)
        if os.path.exists(self.sock_path):
            os.unlink(self.sock_path)      # a restarted daemon replaces the old socket file
        self.server = socket.socket(socket.AF_UNIX)
        self.server.bind(self.sock_path)
        os.chmod(self.sock_path, 0o600)
        self.server.listen(8)
        self.alive = True
        self.conns = []
        threading.Thread(target=self._accept, daemon=True).start()

    def ops(self, op=None):
        with self.lock:
            return [(n, o, f) for n, o, f in self.calls if op is None or o == op]

    def stop(self):
        """The daemon dies: nothing listens and every open connection is cut."""
        self.alive = False
        try:
            self.server.close()
        except OSError:
            pass
        for c in list(self.conns):
            try:
                c.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def _accept(self):
        while self.alive:
            try:
                c, _ = self.server.accept()
            except OSError:
                return
            self.conns.append(c)
            with self.lock:
                self.connections += 1
                self.open_conns += 1
                n = self.connections
            threading.Thread(target=self._serve, args=(c, n), daemon=True).start()

    @staticmethod
    def _read(c, n):
        buf = b''
        while len(buf) < n:
            chunk = c.recv(n - len(buf))
            if not chunk:
                raise ConnectionError
            buf += chunk
        return buf

    def _serve(self, c, n):
        issued, req_no, authed = {}, 0, False
        try:
            while True:
                ln = struct.unpack('>I', self._read(c, 4))[0]
                m = json.loads(self._read(c, ln))
                op = m['op']
                with self.lock:
                    self.calls.append((n, op, {k: v for k, v in m.items() if k not in ('v', 'op')}))
                if self.reply_delay:
                    threading.Event().wait(self.reply_delay)
                if op == 'hello':
                    authed = m.get('token') == self.token
                    out = {'ok': True, 'op': 'hello', 'session': 1} if authed else {'ok': False, 'error': 'unauthorized'}
                elif not authed:
                    out = {'ok': False, 'error': 'unauthorized'}
                elif op == 'layout':
                    out = {'ok': False, 'error': self.layout_error} if self.layout_error else {'ok': True, 'op': 'layout', 'keys': len(m['keys']), 'language': 'en'}
                elif op == 'swipe':
                    req_no += 1
                    issued = {os.urandom(8).hex(): w for w in self.words}
                    out = {'ok': True, 'op': 'swipe', 'req': req_no, 'candidates': [{'id': i, 'word': w, 'score': 1.0} for i, w in issued.items()]}
                elif op == 'commit':
                    w = issued.get(m.get('cand')) if m.get('req') == req_no else None
                    out = {'ok': True, 'op': 'commit', 'text': (self.raw_words or {}).get(w, w)} if w is not None else {'ok': False, 'error': 'unknown_candidate'}
                elif op == 'close':
                    out = {'ok': True, 'op': 'close'}
                else:
                    out = {'ok': True, 'op': op}
                body = json.dumps(out).encode()
                c.sendall(struct.pack('>I', len(body)) + body)
        except (ConnectionError, OSError, ValueError):
            pass
        finally:
            with self.lock:
                self.open_conns -= 1
            try:
                c.close()
            except OSError:
                pass
