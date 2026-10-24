# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Minimal WebSocket client (RFC 6455) for Steam's debug port. Standard library only.

WHY hand-written: the KDE runtime's Python has no aiohttp (plan M0 #13). Only what Chrome DevTools needs:
text messages, fragments, ping/pong, close. Connects to loopback addresses chosen by the caller; the address
policy itself lives in config.py.
"""
import base64
import hashlib
import os
import select
import socket
import struct

GUID = b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11'
MAX_MESSAGE = 16 * 1024 * 1024


class WsError(Exception):
    pass


class ConnectionClosed(WsError):
    pass


class ProtocolError(WsError):
    pass


class Timeout(WsError):
    pass


class WebSocket:
    def __init__(self, sock, max_message=MAX_MESSAGE):
        self.sock = sock
        self.max_message = max_message
        self.buf = b''
        self.closed = False

    # -- low level
    def fileno(self):
        return self.sock.fileno()

    def _fill(self, n, deadline_s):
        """Make sure `n` bytes are buffered; wait at most deadline_s for each chunk."""
        while len(self.buf) < n:
            r, _, _ = select.select([self.sock], [], [], deadline_s)
            if not r:
                raise Timeout('no data')
            chunk = self.sock.recv(65536)
            if not chunk:
                self.closed = True
                raise ConnectionClosed('connection closed')
            self.buf += chunk

    def _take(self, n, t):
        self._fill(n, t)
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def has_buffered(self):
        return bool(self.buf)

    def _send_frame(self, opcode, payload):
        n = len(payload)
        head = bytes([0x80 | opcode])
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack('>H', n)
        else:
            head += bytes([0x80 | 127]) + struct.pack('>Q', n)
        mask = os.urandom(4)
        # WHY masking: required of every client frame by the protocol.
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload)) if n < 4096 else _mask_big(payload, mask)
        try:
            self.sock.sendall(head + mask + masked)
        except OSError as e:
            self.closed = True
            raise ConnectionClosed(str(e))

    # -- public
    def send_text(self, text):
        self._send_frame(1, text.encode('utf-8'))

    def recv_text(self, timeout=None):
        """Next whole text message. Answers pings; raises ConnectionClosed on a close frame."""
        parts = []
        total = 0
        started = False
        while True:
            t = timeout if not started else max(timeout or 5, 5)
            b1, b2 = self._take(2, t)
            fin, op = bool(b1 & 0x80), b1 & 0x0F
            n = b2 & 0x7F
            if n == 126:
                n = struct.unpack('>H', self._take(2, t))[0]
            elif n == 127:
                n = struct.unpack('>Q', self._take(8, t))[0]
            if b2 & 0x80:
                raise ProtocolError('server frames must not be masked')
            if n > self.max_message or total + n > self.max_message:
                self.closed = True
                raise ProtocolError('message too large')
            payload = self._take(n, max(t or 5, 5))
            if op == 9:
                self._send_frame(10, payload)
                continue
            if op == 10:
                continue
            if op == 8:
                self.closed = True
                try:
                    self._send_frame(8, payload[:2])
                except WsError:
                    pass
                raise ConnectionClosed('close frame')
            if op in (1, 0):
                if op == 1 and started:
                    raise ProtocolError('new message inside fragments')
                started = True
                parts.append(payload)
                total += n
                if fin:
                    return b''.join(parts).decode('utf-8')
                continue
            raise ProtocolError('unsupported opcode %d' % op)

    def close(self):
        if not self.closed:
            try:
                self._send_frame(8, b'\x03\xe8')
            except WsError:
                pass
        self.closed = True
        try:
            self.sock.close()
        except OSError:
            pass


def _mask_big(payload, mask):
    # WHY int xor: byte-by-byte Python is slow for the occasional multi-hundred-KB message.
    n = len(payload)
    key = (mask * (n // 4 + 1))[:n]
    return (int.from_bytes(payload, 'big') ^ int.from_bytes(key, 'big')).to_bytes(n, 'big')


def connect(host, port, path, timeout=3.0, max_message=MAX_MESSAGE):
    """Open a websocket to host:port/path. The caller has already vetted host and port."""
    sock = socket.create_connection((host, port), timeout=timeout)
    sock.settimeout(None)
    key = base64.b64encode(os.urandom(16)).decode()
    # WHY Host with the real loopback address: Steam answers any other Host header with HTTP 500 (plan Section 9).
    req = ('GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
           'Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n' % (path, host, port, key))
    try:
        sock.sendall(req.encode())
        head = b''
        sock.settimeout(timeout)
        while b'\r\n\r\n' not in head:
            chunk = sock.recv(4096)
            if not chunk:
                raise ProtocolError('closed during handshake')
            head += chunk
            if len(head) > 65536:
                raise ProtocolError('handshake too long')
        sock.settimeout(None)
    except (OSError, ProtocolError) as e:
        sock.close()
        raise ProtocolError('handshake failed: %s' % e)
    raw, rest = head.split(b'\r\n\r\n', 1)
    lines = raw.decode('latin-1').split('\r\n')
    if ' 101 ' not in lines[0]:
        sock.close()
        raise ProtocolError('handshake refused: ' + lines[0])
    headers = {k.strip().lower(): v.strip() for k, _, v in (ln.partition(':') for ln in lines[1:])}
    want = base64.b64encode(hashlib.sha1(key.encode() + GUID).digest()).decode()
    if headers.get('sec-websocket-accept') != want:
        sock.close()
        raise ProtocolError('bad accept key')
    ws = WebSocket(sock, max_message)
    ws.buf = rest
    return ws
