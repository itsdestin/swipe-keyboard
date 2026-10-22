"""The go-between for typing (plan Sections 3, 7.4 and 8): page -> helper -> engine -> Steam's own text call -> page.

The page layer reports a finished swipe (or a tap on a suggestion, or "keyboard closed") through the `__swipekb` binding.
This file checks every message like an untrusted stranger's, asks the engine for words, types the best one through
Steam's own `ControllerKeyboardSendText`, and tells the page what happened so it can fill its strip.

Rules that matter (each has a test in tests/gaming/test_commit.py):
  * Only text the ENGINE issued is ever typed. The page names a candidate by an id the engine handed out; the engine hands
    back the word only for an id it issued for the latest request, so a page (or anything pretending to be one) cannot make
    us type words of its own.
  * The "erase this many characters" count the page sends must equal what we typed last; anything else is refused.
  * Nothing is written to disk, nothing is logged but counts. Typed words live in this process's memory only for the
    moment a message is being handled, plus one integer (the length of the last word) per keyboard page.
  * Any trouble (engine down, Steam changed, odd message) means: do nothing and tell the page "no". Steam's own keyboard
    is never involved in our failures.
"""
import json
import math
import queue
import re
import threading
import time

from .engine import EngineConn, EngineError, EngineGone

MAX_MESSAGE = 200_000          # bytes of JSON from the page; a 2,000-point swipe is about 60 KB
MAX_POINTS, MAX_KEYS, MAX_CTX = 2000, 128, 20
MAX_COORD, MAX_TIME_MS = 1e5, 1e7          # same bounds as the engine's own checks
MAX_ERASE = 64
STALE_S = 10.0                  # a request that waited longer than this is dropped; the user has moved on
_ID = re.compile(r'[A-Za-z0-9_-]{1,64}')
_LAYOUT = re.compile(r'[a-z][a-z0-9_]{0,47}')
_CTXWORD = re.compile(r"[^\W\d_](?:[^\W\d_]|['’-])*")
_CONTROL = re.compile(r'[\x00-\x1f\x7f-\x9f  ]')


def _num(v, lo, hi):
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v) and lo <= v <= hi


def _int(v, lo, hi):
    return isinstance(v, int) and not isinstance(v, bool) and lo <= v <= hi


def validate(raw):
    """-> a clean message dict, or None. The page is not trusted: shape, sizes and ranges are all checked."""
    if not isinstance(raw, str) or len(raw) > MAX_MESSAGE:
        return None
    try:
        m = json.loads(raw)
    except ValueError:
        return None
    if not isinstance(m, dict) or m.get('v') != 1:
        return None
    t = m.get('type')
    if t == 'closed':
        return {'type': 'closed'}
    if t == 'pick':
        if not (_int(m.get('id'), 0, 2**31) and _int(m.get('req'), 0, 2**53) and isinstance(m.get('cand'), str)
                and _ID.fullmatch(m['cand']) and _int(m.get('erase'), 1, MAX_ERASE)):
            return None
        return {'type': 'pick', 'id': m['id'], 'req': m['req'], 'cand': m['cand'], 'erase': m['erase']}
    if t != 'stroke':
        return None
    tr, keys, ctx = m.get('trace'), m.get('keys'), m.get('ctx', [])
    if not (_int(m.get('id'), 0, 2**31) and isinstance(tr, list) and 2 <= len(tr) <= MAX_POINTS and isinstance(keys, list)
            and 1 <= len(keys) <= MAX_KEYS and isinstance(ctx, list) and len(ctx) <= MAX_CTX):
        return None
    pts = []
    for p in tr:
        if not (isinstance(p, list) and len(p) == 3 and _num(p[0], -MAX_COORD, MAX_COORD) and _num(p[1], -MAX_COORD, MAX_COORD)
                and _num(p[2], 0, MAX_TIME_MS)):
            return None
        pts.append([p[0], p[1], p[2]])
    ks, seen = [], set()
    for k in keys:
        if not (isinstance(k, dict) and isinstance(k.get('k'), str) and len(k['k']) == 1 and k['k'].isalpha()
                and all(_num(k.get(f), -MAX_COORD, MAX_COORD) for f in 'xy') and all(_num(k.get(f), 0, MAX_COORD) and k[f] > 0 for f in 'wh')):
            return None
        if k['k'].lower() in seen:
            return None
        seen.add(k['k'].lower())
        ks.append({'k': k['k'], 'x': k['x'], 'y': k['y'], 'w': k['w'], 'h': k['h']})
    words = []
    for w in ctx:
        if not (isinstance(w, str) and 0 < len(w) <= 60 and _CTXWORD.fullmatch(w)):
            return None
        words.append(w)
    mode, layout, caps = m.get('mode'), m.get('layout', 'qwerty'), m.get('caps', 'none')
    if mode not in ('plain', 'unknown') or caps not in ('none', 'first', 'all') or not (isinstance(layout, str) and _LAYOUT.fullmatch(layout)):
        return None
    return {'type': 'stroke', 'id': m['id'], 'trace': pts, 'keys': ks, 'ctx': words, 'mode': mode, 'layout': layout, 'caps': caps}


def apply_caps(text, caps):
    # WHY here and not in the engine: capital letters are the layer's business (it can see Steam's Shift state).
    if caps == 'all':
        return text.upper()
    if caps == 'first':
        return text[:1].upper() + text[1:]
    return text


def safe_text(s):
    """A word fit to be typed: printable, bounded, no control characters (a Backspace in it would delete text)."""
    return isinstance(s, str) and 0 < len(s) <= 64 and not _CONTROL.search(s) and s.strip() == s


class SendFailed(Exception):
    """Steam's text call could not be reached or refused. The word was not typed."""


class _Page:
    def __init__(self):
        self.conn = None
        self.layout_sig = None
        self.last = None            # {'req', 'len', 'caps', 'cands'} for the word typed last (no text, only its length)
        self.tokens = RATE_BURST
        self.refilled = time.monotonic()


RATE_BURST, RATE_PER_S = 6.0, 6.0   # swipes cannot physically come faster; a flood from a broken page is dropped


class Bridge:
    """send_text(str) types through Steam; deliver(page_id, javascript) runs a statement in that page. Both are supplied by the
    helper and may be called from this class's worker thread."""

    def __init__(self, cfg, send_text, deliver):
        self.cfg = cfg
        self.send_text = send_text
        self.deliver = deliver
        self.q = queue.Queue(maxsize=8)
        self.pages = {}
        self.stats = {'strokes': 0, 'picks': 0, 'typed': 0, 'rejected': 0, 'dropped': 0, 'engine_down': 0}
        self.rate = {}
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    # ---- called from the helper's main thread ----------------------------------------------------------------
    def submit(self, page_id, raw):
        m = validate(raw)
        if m is None:
            self.stats['rejected'] += 1
            return
        now = time.monotonic()
        r = self.rate.setdefault(page_id, [RATE_BURST, now])
        r[0] = min(RATE_BURST, r[0] + (now - r[1]) * RATE_PER_S)
        r[1] = now
        if m['type'] != 'closed':
            if r[0] < 1:
                self.stats['rejected'] += 1
                return
            r[0] -= 1
        self._put((page_id, m, now))

    def forget(self, page_id):
        """The page is gone: hang up on the engine for it."""
        self.rate.pop(page_id, None)
        self._put((page_id, {'type': 'drop'}, time.monotonic()))

    def stop(self):
        self._put(None, block=True)
        self._thread.join(timeout=5)

    def _put(self, item, block=False):
        try:
            self.q.put(item, block=block, timeout=1 if block else None)
        except queue.Full:
            self.stats['dropped'] += 1

    # ---- the worker ------------------------------------------------------------------------------------------
    def _run(self):
        while True:
            item = self.q.get()
            if item is None:
                break
            pid, m, t = item
            try:
                if m['type'] in ('closed', 'drop'):
                    self._hang_up(pid, forget_page=m['type'] == 'drop')
                elif time.monotonic() - t > STALE_S:
                    self.stats['dropped'] += 1
                elif m['type'] == 'stroke':
                    self._stroke(pid, m)
                else:
                    self._pick(pid, m)
            except Exception:
                # WHY catch all: nothing in here may kill the worker; the page just gets "no".
                self._answer(pid, m, error='internal')
        for pid in list(self.pages):
            self._hang_up(pid, forget_page=True)

    def _page(self, pid):
        return self.pages.setdefault(pid, _Page())

    def _hang_up(self, pid, forget_page):
        st = self.pages.get(pid)
        if not st:
            return
        if st.conn:
            st.conn.close()
        st.conn = None
        st.layout_sig = None
        st.last = None
        if forget_page:
            self.pages.pop(pid, None)

    def _conn(self, st, mode):
        if st.conn and st.conn.mode != mode:
            st.conn.close()
            st.conn = None
            st.layout_sig = None
        if not st.conn:
            st.conn = EngineConn(self.cfg.engine_socket, self.cfg.engine_token, mode)
            st.layout_sig = None
        return st.conn

    def _answer(self, pid, m, **fields):
        body = {'v': 1, 'kind': m.get('type', 'stroke'), 'id': m.get('id', -1)}
        body.update(fields)
        js = ('(function(){var L=window.__swipekbLayer;if(L&&L.session)L.session.result(%s);})()' % json.dumps(body))
        self.deliver(pid, js)

    def _stroke(self, pid, m):
        st = self._page(pid)
        self.stats['strokes'] += 1
        try:
            conn = self._conn(st, m['mode'])
            sig = (m['layout'], tuple(sorted((k['k'], round(k['x']), round(k['y']), round(k['w']), round(k['h'])) for k in m['keys'])))
            if st.layout_sig != sig:
                # the layer sends the centre of each key; the engine wants the top-left corner and the size
                conn.call('layout', steam_layout=m['layout'], keys=[
                    {'c': k['k'].lower(), 'x': k['x'] - k['w'] / 2, 'y': k['y'] - k['h'] / 2, 'w': k['w'], 'h': k['h']} for k in m['keys']])
                st.layout_sig = sig
            r = conn.call('swipe', points=m['trace'], context=m['ctx'], top_k=3)
            raw = [c for c in r.get('candidates', []) if isinstance(c, dict)]
            if not raw:
                return self._answer(pid, m, committed=False, error='no_candidates')
            # WHY the best one is judged before the others are filtered: if the engine's best answer is unusable we type
            # nothing, rather than quietly type its second choice.
            if not (isinstance(raw[0].get('id'), str) and _ID.fullmatch(raw[0]['id']) and safe_text(raw[0].get('word'))):
                return self._answer(pid, m, committed=False, error='bad_text')
            cands = [c for c in raw if isinstance(c.get('id'), str) and _ID.fullmatch(c['id']) and safe_text(c.get('word'))]
            word = conn.call('commit', req=r['req'], cand=cands[0]['id'])['text']
            if not safe_text(word):
                return self._answer(pid, m, committed=False, error='bad_text')
            typed = apply_caps(word, m['caps']) + ' '
            self.send_text(typed)
        except EngineError as e:
            st.layout_sig = None
            return self._answer(pid, m, committed=False, error=e.code)
        except SendFailed:
            return self._answer(pid, m, committed=False, error='steam')
        except EngineGone:
            self._hang_up(pid, forget_page=False)
            self.stats['engine_down'] += 1
            return self._answer(pid, m, committed=False, error='engine')
        shown = [{'id': c['id'], 'word': apply_caps(c['word'], m['caps'])} for c in cands]
        st.last = {'req': r['req'], 'len': len(typed), 'caps': m['caps'], 'cands': shown}
        self.stats['typed'] += 1
        self._answer(pid, m, committed=True, req=r['req'], typed=typed, cands=shown)

    def _pick(self, pid, m):
        st = self._page(pid)
        self.stats['picks'] += 1
        last = st.last
        # WHY both checks: the page may only replace the word we typed last, at exactly the length we typed.
        if not (last and st.conn and m['req'] == last['req'] and m['erase'] == last['len']):
            self.stats['rejected'] += 1
            return self._answer(pid, m, committed=False, error='stale')
        try:
            word = st.conn.call('commit', req=m['req'], cand=m['cand'])['text']
            if not safe_text(word):
                return self._answer(pid, m, committed=False, error='bad_text')
            typed = apply_caps(word, last['caps']) + ' '
            self.send_text('\b' * m['erase'] + typed)
        except EngineError as e:
            self.stats['rejected'] += 1        # the engine never issued that id for this request
            return self._answer(pid, m, committed=False, error=e.code)
        except SendFailed:
            return self._answer(pid, m, committed=False, error='steam')
        except EngineGone:
            self._hang_up(pid, forget_page=False)
            self.stats['engine_down'] += 1
            return self._answer(pid, m, committed=False, error='engine')
        last['len'] = len(typed)
        self.stats['typed'] += 1
        self._answer(pid, m, committed=True, req=last['req'], typed=typed, cands=last['cands'])
