"""Client for the engine daemon (swipekb-gamingd): length-prefixed JSON over a Unix socket (gaming/daemon/protocol.hpp).

WHY this file is small and dumb: the daemon owns every decision about words. We only open the socket, show the token
(a file only our user can read), and relay requests. No text is ever written anywhere here; the socket is the only place a
word travels, and it is closed when the keyboard session ends so the daemon forgets the layout and the candidate ids.
"""
import json
import socket
import struct

MAX_FRAME = 256 * 1024     # same limit as the daemon; a bigger frame is refused before it is sent


class EngineError(Exception):
    """The daemon answered {"ok": false, "error": CODE}. `code` is the daemon's own word for what went wrong."""

    def __init__(self, code):
        super().__init__(code)
        self.code = code


class EngineGone(Exception):
    """The daemon is not there (no socket, refused, hung up, too slow). The caller gives up quietly: Steam's own keyboard is unaffected."""


class EngineConn:
    def __init__(self, socket_path, token_path, mode, timeout=30.0):
        self.sock = socket.socket(socket.AF_UNIX)
        try:
            with open(token_path) as f:
                token = f.read().strip()
            self.sock.settimeout(timeout)
            self.sock.connect(str(socket_path))
        except OSError as e:
            self.sock.close()
            raise EngineGone('cannot reach the engine: %s' % e.__class__.__name__)
        try:
            self.call('hello', token=token, mode=mode)
        except Exception:
            self.close()
            raise
        self.mode = mode

    def call(self, op, **fields):
        body = json.dumps(dict(v=1, op=op, **fields)).encode()
        if len(body) > MAX_FRAME:
            raise EngineGone('request too large')
        try:
            self.sock.sendall(struct.pack('>I', len(body)) + body)
            head = self._exact(4)
            n = struct.unpack('>I', head)[0]
            if n == 0 or n > MAX_FRAME:
                raise EngineGone('bad frame from the engine')
            reply = json.loads(self._exact(n))
        except (OSError, ValueError):
            raise EngineGone('the engine stopped answering')
        if not isinstance(reply, dict):
            raise EngineGone('odd answer from the engine')
        if not reply.get('ok'):
            raise EngineError(str(reply.get('error', 'error'))[:40])
        return reply

    def _exact(self, n):
        buf = b''
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise OSError('closed')
            buf += chunk
        return buf

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
