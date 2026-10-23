"""Chrome DevTools Protocol over the hand-written websocket: one request at a time, events kept aside."""
import http.client
import json
import re

from . import config, ws

_ID = re.compile(r'^[A-Za-z0-9_-]{1,128}$')


class CdpError(Exception):
    pass


def http_json(port, path, timeout=1.0):
    """GET http://127.0.0.1:<port><path> -> parsed JSON. OSError when the port is closed."""
    config.check_port(port)
    c = http.client.HTTPConnection(config.HOST, port, timeout=timeout)
    try:
        c.request('GET', path)
        r = c.getresponse()
        body = r.read(4 * 1024 * 1024)
        if r.status != 200:
            raise OSError('HTTP %d' % r.status)
        return json.loads(body)
    except (http.client.HTTPException, ValueError) as e:
        raise OSError('bad answer: %s' % e)
    finally:
        c.close()


def page_path(target_id):
    """WHY built here and not copied from /json/list: the helper never follows an address Steam hands it."""
    if not _ID.match(target_id):
        raise CdpError('odd target id')
    return '/devtools/page/' + target_id


class Cdp:
    def __init__(self, websocket, timeout=2.0):
        self.ws = websocket
        self.timeout = timeout
        self.n = 0
        self.events = []

    @classmethod
    def open(cls, port, path, timeout=2.0):
        config.check_ws_url('ws://%s:%d%s' % (config.HOST, port, path), port)
        return cls(ws.connect(config.HOST, port, path, timeout=timeout), timeout)

    def call(self, method, params=None, timeout=None):
        self.n += 1
        mid = self.n
        self.ws.send_text(json.dumps({'id': mid, 'method': method, 'params': params or {}}))
        while True:
            msg = json.loads(self.ws.recv_text(timeout=timeout or self.timeout))
            if msg.get('id') == mid:
                if 'error' in msg:
                    raise CdpError('%s: %s' % (method, msg['error']))
                return msg.get('result', {})
            if 'method' in msg:
                self.events.append(msg)

    def evaluate(self, expression, timeout=None):
        """Runtime.evaluate -> the returned value. CdpError if the script threw."""
        r = self.call('Runtime.evaluate', {'expression': expression, 'returnByValue': True}, timeout)
        if 'exceptionDetails' in r:
            raise CdpError('script error: %s' % (r['exceptionDetails'].get('exception', {}).get('description') or 'exception'))
        return r.get('result', {}).get('value')

    def next_event(self, timeout):
        if self.events:
            return self.events.pop(0)
        return json.loads(self.ws.recv_text(timeout=timeout))

    def close(self):
        self.ws.close()
