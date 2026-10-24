# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Client for the engine daemon (swipekb-gamingd): length-prefixed JSON over a Unix socket (gaming/daemon/protocol.hpp).

WHY this file is small and dumb: the daemon owns every decision about words. We only open the socket, show the token
(a file only our user can read), and relay requests. No text is ever written anywhere here; the socket is the only place a
word travels, and it is closed when the keyboard session ends so the daemon forgets the layout and the candidate ids.
"""
import json
import os
import socket
import stat
import struct

MAX_FRAME = 256 * 1024     # same limit as the daemon; a bigger frame is refused before it is sent


class EngineError(Exception):
    """The daemon answered {"ok": false, "error": CODE}. `code` is the daemon's own word for what went wrong."""

    def __init__(self, code):
        super().__init__(code)
        self.code = code


class EngineGone(Exception):
    """The daemon is not there (no socket, refused, hung up, too slow). The caller gives up quietly: Steam's own keyboard is unaffected."""


def _check_private(socket_path, token_path):
    """Refuse an engine folder that someone else could have made or written to (review fix 7).

    The token is a password for the engine, and the helper sends it to whatever listens on the socket. Both live in a folder the
    engine makes for our user alone (mode 0700); if the folder or the token belongs to another user, or others can read or write
    them, whatever is there is not the engine we installed."""
    me = os.getuid()
    try:
        folder = os.stat(os.path.dirname(str(socket_path)) or '.')
        token = os.stat(str(token_path))
        sock = os.stat(str(socket_path))
    except OSError as e:
        raise EngineGone('cannot reach the engine: %s' % e.__class__.__name__)
    ok = (folder.st_uid == me and not folder.st_mode & 0o077 and stat.S_ISDIR(folder.st_mode)
          and token.st_uid == me and not token.st_mode & 0o077 and sock.st_uid == me)
    if not ok:
        raise EngineGone('the engine folder is not private to this user')


class EngineConn:
    def __init__(self, socket_path, token_path, mode, timeout=30.0):
        _check_private(socket_path, token_path)
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
