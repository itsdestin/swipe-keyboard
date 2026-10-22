#!/usr/bin/env python3
"""End-to-end tests for swipekb-gamingd (Gaming Mode milestone M2).

Each test starts the real daemon binary on a private socket and talks the real wire protocol (gaming/daemon/
protocol.hpp). Environment (set by CTest, see gaming/daemon/CMakeLists.txt):
  SWIPEKB_GAMINGD       the daemon binary
  SWIPEKB_REPLAY_EXE    tools/swipekb-replay, the oracle for the equivalence test
  SWIPEKB_ASSETS        models + dictionary folder
  SWIPEKB_FUTO_LAYOUT   FUTO's en_qwerty.json (key geometry for the recorded bench swipes)
Missing models are a hard failure, not a skip: a skipped equivalence test would read as green.
"""
import json
import os
import random
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

DAEMON = os.environ.get("SWIPEKB_GAMINGD", "")
REPLAY = os.environ.get("SWIPEKB_REPLAY_EXE", "")
ASSETS = os.environ.get("SWIPEKB_ASSETS", "")
LAYOUT = os.environ.get("SWIPEKB_FUTO_LAYOUT", "")
BENCH = os.path.join(ASSETS, "eval", "bench-2000.jsonl")
# M7 (multilingual): the authored swipes, and what the REAL plugin's guess bar shows for them (plugin_languageoracle).
LANG_DATASET = os.environ.get("SWIPEKB_LANG_DATASET", "")
ORACLE_JSON = os.environ.get("SWIPEKB_ORACLE_JSON", "")
KEY_SCALE = 1000.0   # same scaling swipekb-bench uses: bench traces are 0..1 of the letter area

# A word nothing in the product could produce by accident. Used to prove typed text never reaches logs.
SENTINEL = "zebracornflux"


def keys_from_futo(path):
    """FUTO's centre+half-size layout -> the daemon's pixel rectangles (same maths as tools/benchlayout.h)."""
    out = []
    for k in json.load(open(path))["keys"]:
        out.append({"c": k["letter"], "x": (k["cx"] - k["rx"]) * KEY_SCALE, "y": (k["cy"] - k["ry"]) * KEY_SCALE,
                    "w": 2 * k["rx"] * KEY_SCALE, "h": 2 * k["ry"] * KEY_SCALE})
    return out


# WHY synthetic swipes exist: the recorded corpus (assets/eval/bench-2000.jsonl) is a developer-only download made from FUTO's
# test set; the package builds and CI never fetch it, so every test that needed it errored there. These are drawn on
# the layout itself: a path through the centres of the word's keys with a small repeatable wobble, like a careful swipe.
SYNTHETIC_WORDS = ["hello", "there", "people", "because", "should", "thanks", "going", "about", "would", "little",
                   "friend", "morning", "really", "before", "think", "great", "world", "house", "water", "right",
                   "other", "after", "first", "never", "where", "those", "found", "still", "every", "again",
                   "small", "large", "party", "story", "money", "night", "point", "thing", "place", "music",
                   "happy", "table", "green", "heart", "light", "paper", "start", "today", "voice", "write",
                   "plan", "good", "look", "come", "time", "work", "life", "name", "game", "back"]
SYNTHETIC_CONTEXT = [[], ["i", "said"], ["we", "can", "go"], ["thank", "you", "for"], ["it", "was", "a"]]


def synthetic_swipes(limit):
    """Deterministic swipes drawn on the en_qwerty layout: (target, points[[x,y,t]], context[]) like swipe_words."""
    centre = {k["c"]: (k["x"] + k["w"] / 2, k["y"] + k["h"] / 2, k["w"], k["h"]) for k in keys_from_futo(LAYOUT)}
    rng = random.Random(168)
    out = []
    for n, word in enumerate(SYNTHETIC_WORDS[:limit]):
        pts, t = [], 0
        for a, b in zip(word, word[1:] or word):
            (ax, ay, w, h), (bx, by, _, _) = centre[a][:4], centre[b][:4]
            for i in range(10):
                f = i / 10
                pts.append([ax + (bx - ax) * f + rng.uniform(-0.12, 0.12) * w, ay + (by - ay) * f + rng.uniform(-0.12, 0.12) * h, t])
                t += 8
        bx, by = centre[word[-1]][:2]
        pts.append([bx, by, t])
        out.append((word, pts, list(SYNTHETIC_CONTEXT[n % len(SYNTHETIC_CONTEXT)])))
    return out


def swipe_words(path, limit):
    """First `limit` scorable swipes of the bench corpus, as (target, points[[x,y,t]], context[]).
    When a developer has the recorded corpus the recorded swipes are used; otherwise (CI, package builds) synthetic ones."""
    if not os.path.exists(path):
        return synthetic_swipes(limit)
    out = []
    for line in open(path):
        o = json.loads(line)
        word = re.sub(r"[^a-z]", "", o["word"].lower())
        data = o.get("data")
        if len(word) < 2 or not isinstance(data, list) or len(data) <= 2:
            continue
        t0 = data[0]["t"]
        pts = [[p["x"] * KEY_SCALE, p["y"] * KEY_SCALE, p["t"] - t0] for p in data]
        words = o["sentence"].split()[: o["word_idx"]]
        ctx = [re.sub(r"^\W+|\W+$", "", w).lower() for w in words]
        ctx = [w for w in ctx if w][-20:]
        out.append((word, pts, ctx))
        if len(out) == limit:
            break
    return out


def without_event_names(log):
    """The daemon's own fixed log vocabulary ("hello", "commit", "layout", ...) removed, so a short guess such as "hell" is
    not mistaken for a leak just because it is a piece of the word "hello" in `swipekb-gamingd: hello 1 -1`.
    WHY: the leak check compares guesses against the log by substring; with the recorded corpus no guess happened to be that
    short, with the synthetic swipes one is. Only the event name after the prefix is removed; any typed text stays in view."""
    return re.sub(r"(swipekb-gamingd:) \w+", r"\1", log)


class Conn:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.settimeout(30)
        self.s.connect(path)

    def send_raw(self, data):
        self.s.sendall(data)

    def send(self, obj):
        body = obj if isinstance(obj, bytes) else json.dumps(obj).encode()
        self.s.sendall(struct.pack(">I", len(body)) + body)

    def recv(self):
        head = self._exact(4)
        if head is None:
            return None
        body = self._exact(struct.unpack(">I", head)[0])
        return None if body is None else json.loads(body)

    def _exact(self, n):
        buf = b""
        while len(buf) < n:
            try:
                chunk = self.s.recv(n - len(buf))
            except (ConnectionResetError, socket.timeout):
                return None
            if not chunk:
                return None
            buf += chunk
        return buf

    def call(self, op, **kw):
        self.send({"v": 1, "op": op, **kw})
        return self.recv()

    def closed_by_peer(self):
        """True when the daemon has hung up (reads EOF/reset)."""
        self.s.settimeout(5)
        try:
            return self.s.recv(1) == b""
        except ConnectionResetError:
            return True
        except socket.timeout:
            return False

    def close(self):
        self.s.close()


class DaemonCase(unittest.TestCase):
    extra_args = []
    wrapper = []   # e.g. ["unshare", "-rn"]

    @classmethod
    def setUpClass(cls):
        assert DAEMON and os.path.exists(DAEMON), "SWIPEKB_GAMINGD must name the built daemon"

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="gamingd-")
        self.sock = os.path.join(self.tmp, "run", "engine.sock")
        self.tokfile = os.path.join(self.tmp, "run", "engine.token")
        self.procs = []
        self.conns = []
        # WHY a private settings file for every test: the daemon reads the user's enabled languages from the desktop
        # keyboard's settings file; tests must never depend on (or read) the real one on this machine.
        self.settings = os.path.join(self.tmp, "plasmakeyboardrc")
        self.write_settings("en_US,es_ES,fr_FR,de_DE")

    def write_settings(self, locales):
        with open(self.settings, "w") as f:
            f.write("[General]\nsoundEnabled=false\n" + (f"enabledLocales={locales}\n" if locales is not None else ""))

    def tearDown(self):
        for c in self.conns:
            c.close()
        for p in self.procs:
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
                try:
                    p.wait(10)
                except subprocess.TimeoutExpired:
                    p.kill()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def start(self, extra=(), wrapper=None):
        log = open(os.path.join(self.tmp, "daemon.log"), "ab")
        cmd = list(self.wrapper if wrapper is None else wrapper) + [
            DAEMON, "--socket", self.sock, "--token-file", self.tokfile, "--data", ASSETS,
            "--settings-file", self.settings, *self.extra_args, *extra]
        p = subprocess.Popen(cmd, stderr=log, stdout=log)
        self.procs.append(p)
        deadline = time.monotonic() + 20
        while True:   # a successful connect IS the ready signal (a stale socket file may already exist)
            self.assertIsNone(p.poll(), "daemon exited early: " + self.log())
            self.assertLess(time.monotonic(), deadline, "daemon never accepted a connection")
            probe = socket.socket(socket.AF_UNIX)
            try:
                probe.connect(self.sock)
                probe.close()
                return p
            except OSError:
                probe.close()
                time.sleep(0.02)

    def log(self):
        try:
            return open(os.path.join(self.tmp, "daemon.log"), errors="replace").read()
        except OSError:
            return ""

    def token(self):
        return open(self.tokfile).read().strip()

    def connect(self, mode="unknown", hello=True):
        c = Conn(self.sock)
        self.conns.append(c)
        if hello:
            r = c.call("hello", token=self.token(), mode=mode)
            self.assertTrue(r and r["ok"], r)
        return c


class TestLogCheckHelper(unittest.TestCase):
    """The leak check must still see a leak: only event names are hidden from it."""
    def test_event_names_are_hidden_but_a_leaked_word_is_not(self):
        log = "swipekb-gamingd: hello 1 -1\nswipekb-gamingd: commit -1 -1\nswipekb-gamingd: typed hell 2 3\n"
        cleaned = without_event_names(log)
        self.assertNotIn("hello", cleaned)
        self.assertNotIn("commit", cleaned)
        self.assertIn("hell", cleaned)   # the word a leaking daemon would have written after its event name


class TestSecurity(DaemonCase):
    def test_socket_is_0600_and_dir_0700_and_token_file_0600(self):
        self.start()
        self.assertEqual(os.stat(self.sock).st_mode & 0o777, 0o600)
        self.assertEqual(os.stat(self.tokfile).st_mode & 0o777, 0o600)
        self.assertEqual(os.stat(os.path.dirname(self.sock)).st_mode & 0o777, 0o700)
        self.assertGreaterEqual(len(self.token()), 32)

    def test_token_is_fresh_each_start(self):
        p = self.start()
        first = self.token()
        p.send_signal(signal.SIGTERM)
        p.wait(10)
        self.assertFalse(os.path.exists(self.sock), "clean shutdown must remove the socket")
        self.start()
        self.assertNotEqual(first, self.token())

    def test_no_request_works_before_hello(self):
        self.start()
        c = self.connect(hello=False)
        r = c.call("ping")
        self.assertEqual(r["error"], "unauthorized")
        self.assertTrue(c.closed_by_peer())

    def test_wrong_token_is_refused_and_closed(self):
        self.start()
        c = self.connect(hello=False)
        r = c.call("hello", token="0" * 64)
        self.assertEqual(r["error"], "unauthorized")
        self.assertTrue(c.closed_by_peer())

    def test_second_instance_refuses_but_stale_socket_is_replaced(self):
        p = self.start()
        second = subprocess.run([DAEMON, "--socket", self.sock, "--token-file", self.tokfile, "--data", ASSETS],
                                capture_output=True, timeout=20)
        self.assertNotEqual(second.returncode, 0)
        self.assertIsNone(p.poll(), "the running daemon must be undisturbed")
        p.kill()
        p.wait(10)
        self.assertTrue(os.path.exists(self.sock), "kill -9 leaves a stale socket")
        self.start()   # must replace it
        self.connect()

    def test_connection_limit(self):
        self.start()
        held = [self.connect(hello=False) for _ in range(8)]
        extra = Conn(self.sock)
        self.conns.append(extra)
        r = extra.recv()
        self.assertEqual(r["error"], "too_many_connections")
        self.assertTrue(held)


class TestMalformed(DaemonCase):
    def test_bad_requests_get_errors_and_the_connection_survives(self):
        self.start()
        c = self.connect()
        c.call("layout", keys=keys_from_futo(LAYOUT))
        bad = [
            b"not json", b"[]", b"{}", b'{"v":1}', b'{"v":2,"op":"ping"}', b'{"v":1,"op":42}',
            b'{"v":1,"op":"nope"}', b'{"v":1,"op":"swipe"}', b'{"v":1,"op":"swipe","points":"x"}',
            b'{"v":1,"op":"swipe","points":[[1,2]]}',
            json.dumps({"v": 1, "op": "swipe", "points": [[0, 0, 0]] * 3000}).encode(),
            json.dumps({"v": 1, "op": "swipe", "points": [[1e30, 0, 0], [0, 0, 1]]}).encode(),
            json.dumps({"v": 1, "op": "swipe", "points": [["a", 0, 0], [0, 0, 1]]}).encode(),
            json.dumps({"v": 1, "op": "swipe", "points": [[0, 0, 0], [5, 5, 9]], "top_k": 99}).encode(),
            json.dumps({"v": 1, "op": "swipe", "points": [[0, 0, 0], [5, 5, 9]], "context": ["x" * 500]}).encode(),
            json.dumps({"v": 1, "op": "swipe", "points": [[0, 0, 0], [5, 5, 9]], "context": ["a\u0000b"]}).encode(),
            json.dumps({"v": 1, "op": "swipe", "points": [[0, 0, 0], [5, 5, 9]], "context": ["w"] * 500}).encode(),
            json.dumps({"v": 1, "op": "layout", "keys": []}).encode(),
            json.dumps({"v": 1, "op": "layout", "keys": [{"c": "ab", "x": 0, "y": 0, "w": 1, "h": 1}]}).encode(),
            json.dumps({"v": 1, "op": "layout", "keys": [{"c": "a", "x": 0, "y": 0, "w": 0, "h": 1}]}).encode(),
            json.dumps({"v": 1, "op": "layout", "keys": [{"c": "a", "x": 0, "y": 0, "w": 1, "h": 1}] * 2}).encode(),
            json.dumps({"v": 1, "op": "layout", "keys": [{"c": "a", "x": 0, "y": 0, "w": 1, "h": 1}] * 500}).encode(),
            json.dumps({"v": 1, "op": "commit", "req": "x", "cand": 5}).encode(),
            json.dumps({"v": 1, "op": "reset", "mode": "bogus"}).encode(),
            json.dumps({"v": 1, "op": "next_words", "words": "no"}).encode(),
            b"[" * 5000 + b"]" * 5000,
            b'{"v":1,"op":"ping","x":NaN}',
            b"\xff\xfe\x00garbage",
        ]
        for body in bad:
            c.send(body)
            r = c.recv()
            self.assertIsNotNone(r, f"daemon dropped the connection on {body[:60]!r}")
            self.assertFalse(r["ok"], body[:60])
        self.assertTrue(c.call("ping")["ok"])

    def test_oversize_zero_and_truncated_frames_close_the_connection_without_crashing(self):
        p = self.start()
        for header in (struct.pack(">I", 256 * 1024 + 1), struct.pack(">I", 0x7FFFFFFF), struct.pack(">I", 0),
                       struct.pack(">I", 0xFFFFFFFF)):
            c = self.connect()
            c.send_raw(header)
            r = c.recv()
            self.assertEqual(r["error"], "bad_frame")
            self.assertTrue(c.closed_by_peer())
        c = self.connect()
        c.send_raw(struct.pack(">I", 100) + b'{"v":1')   # promise 100 bytes, send 6, vanish
        c.close()
        self.assertIsNone(p.poll())
        self.assertTrue(self.connect().call("ping")["ok"])

    def test_stalled_partial_frame_is_dropped(self):
        self.start(["--frame-timeout-ms", "300"])
        c = self.connect()
        c.send_raw(struct.pack(">I", 100) + b"{")
        self.assertTrue(c.closed_by_peer(), "a client that stalls mid-frame must not hold a slot forever")

    def test_random_fuzz_never_kills_the_daemon(self):
        p = self.start()
        rng = random.Random(1234)   # fixed seed: a failure must be replayable
        good = {"v": 1, "op": "swipe", "points": [[10, 10, 0], [200, 50, 30], [300, 90, 60]], "context": ["the"], "top_k": 3}
        layout = {"v": 1, "op": "layout", "keys": keys_from_futo(LAYOUT)}
        c = self.connect()
        for i in range(400):
            base = json.dumps(rng.choice([good, layout, {"v": 1, "op": "commit", "req": 1, "cand": "c0"},
                                         {"v": 1, "op": "next_words", "words": ["a", "b"], "partial": "he"}])).encode()
            kind = i % 4
            if kind == 0:
                body = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 200)))
            elif kind == 1:
                b = bytearray(base)
                for _ in range(rng.randrange(1, 6)):
                    b[rng.randrange(len(b))] = rng.randrange(256)
                body = bytes(b)
            elif kind == 2:
                body = base[: rng.randrange(1, len(base))]
            else:
                body = base
            try:
                c.send(body)
                r = c.recv()
            except OSError:
                r = None
            if r is None:   # the daemon may legitimately hang up on abuse; it must stay alive for others
                self.assertIsNone(p.poll(), f"daemon died on fuzz case {i}")
                c = self.connect()
        self.assertIsNone(p.poll())
        self.assertTrue(self.connect().call("ping")["ok"])


class TestPrivacy(DaemonCase):
    def test_commit_accepts_only_candidate_ids_the_daemon_issued(self):
        self.start()
        c = self.connect()
        c.call("layout", keys=keys_from_futo(LAYOUT))
        word, pts, ctx = swipe_words(BENCH, 1)[0]
        r1 = c.call("swipe", points=pts, context=ctx)
        self.assertTrue(r1["ok"], r1)
        cand = r1["candidates"][0]
        # a made-up id, a made-up request, text smuggled in as an id: all refused
        for req, cid in ((r1["req"], "c0000000000000000"), (r1["req"], cand["word"]), (r1["req"] + 7, cand["id"]),
                         (r1["req"], cand["id"][:-1]), (r1["req"], cand["id"] + "0")):
            r = c.call("commit", req=req, cand=cid)
            self.assertEqual(r.get("error"), "unknown_candidate", (req, cid))
        ok = c.call("commit", req=r1["req"], cand=cand["id"])
        self.assertEqual(ok, {"ok": True, "op": "commit", "text": cand["word"]})
        # a newer swipe invalidates the old ids
        r2 = c.call("swipe", points=pts, context=ctx)
        self.assertEqual(c.call("commit", req=r1["req"], cand=cand["id"])["error"], "unknown_candidate")
        self.assertTrue(c.call("commit", req=r2["req"], cand=r2["candidates"][0]["id"])["ok"])

    def test_ids_do_not_cross_connections_and_die_with_reset_and_close(self):
        self.start()
        a = self.connect()
        a.call("layout", keys=keys_from_futo(LAYOUT))
        word, pts, ctx = swipe_words(BENCH, 1)[0]
        r = a.call("swipe", points=pts)
        cid, req = r["candidates"][0]["id"], r["req"]
        b = self.connect()
        self.assertEqual(b.call("commit", req=req, cand=cid)["error"], "unknown_candidate")
        self.assertTrue(a.call("reset")["ok"])
        self.assertEqual(a.call("commit", req=req, cand=cid)["error"], "unknown_candidate")
        self.assertEqual(a.call("swipe", points=pts)["error"], "no_layout", "reset must also forget the layout")
        self.assertTrue(a.call("close")["ok"])
        self.assertTrue(a.closed_by_peer())

    def test_unknown_mode_keeps_nothing_between_requests(self):
        self.start()
        a = self.connect("unknown")
        a.call("layout", keys=keys_from_futo(LAYOUT))
        word, pts, ctx = swipe_words(BENCH, 1)[0]
        with_ctx = a.call("swipe", points=pts, context=["i", "want", "to"])
        later_plain = a.call("swipe", points=pts)   # no context given: must not remember the previous request's
        fresh = self.connect("unknown")
        fresh.call("layout", keys=keys_from_futo(LAYOUT))
        baseline = fresh.call("swipe", points=pts)
        words = lambda r: [(c["word"], c["score"]) for c in r["candidates"]]
        self.assertEqual(words(later_plain), words(baseline))
        self.assertTrue(with_ctx["ok"])

    def test_unknown_mode_refuses_secret_looking_text_but_plain_mode_does_not(self):
        self.start()
        secret = "Xk9#mQ2$vLp"
        u = self.connect("unknown")
        r = u.call("next_words", words=[secret])
        self.assertEqual((r["ok"], r["candidates"]), (True, []))
        r = u.call("next_words", partial=secret)
        self.assertEqual(r["candidates"], [])
        p = self.connect("plain")
        r = p.call("next_words", words=["the", "quick"])
        self.assertTrue(r["ok"] and r["candidates"], r)
        # in unknown mode a secret in swipe context is dropped (swipe itself still answers, context-free)
        u.call("layout", keys=keys_from_futo(LAYOUT))
        word, pts, ctx = swipe_words(BENCH, 1)[0]
        with_secret = u.call("swipe", points=pts, context=[secret])
        plain_none = u.call("swipe", points=pts)
        self.assertEqual([c["word"] for c in with_secret["candidates"]], [c["word"] for c in plain_none["candidates"]])

    def test_unknown_mode_context_cap_is_smaller_than_plain(self):
        self.start()
        words = ["the"] * 15
        self.assertEqual(self.connect("unknown").call("next_words", words=words)["error"], "bad_request")
        self.assertTrue(self.connect("plain").call("next_words", words=words)["ok"])

    def test_next_words_partial_completes_and_ids_commit(self):
        self.start()
        c = self.connect("plain")
        r = c.call("next_words", partial="hel", top_k=5)
        self.assertTrue(r["ok"] and r["candidates"], r)
        self.assertTrue(all(x["word"].startswith("hel") for x in r["candidates"]))
        got = c.call("commit", req=r["req"], cand=r["candidates"][0]["id"])
        self.assertEqual(got["text"], r["candidates"][0]["word"])

    def test_logs_never_contain_typed_text(self):
        self.start()
        c = self.connect("plain")
        c.call("layout", keys=keys_from_futo(LAYOUT))
        word, pts, ctx = swipe_words(BENCH, 1)[0]
        seen = set()
        r = c.call("swipe", points=pts, context=[SENTINEL, "marmalade"])
        seen |= {x["word"] for x in r["candidates"]} | {x["id"] for x in r["candidates"]}
        r = c.call("next_words", words=[SENTINEL], partial="qxzvk")
        c.call("commit", req=1, cand="c" + SENTINEL)
        # a SUCCESSFUL commit is the most tempting place to log the word, so exercise it too
        swiped = c.call("swipe", points=pts)
        seen |= {x["word"] for x in swiped["candidates"]}
        committed = c.call("commit", req=swiped["req"], cand=swiped["candidates"][0]["id"])
        self.assertTrue(committed["ok"])
        c.send(('{"v":1,"op":"%s","note":"%s"}' % (SENTINEL, SENTINEL)).encode())   # malformed-op path too
        c.recv()
        c.send(b'{"v":1,"op":"swipe","points":[["%s",0,0]]}' % SENTINEL.encode())
        c.recv()
        c.call("close")
        time.sleep(0.2)
        log = self.log()
        self.assertTrue(log.strip(), "the log should record counts and timings")
        log = without_event_names(log)
        for needle in [SENTINEL, "qxzvk", self.token(), *seen, "marmalade"]:
            self.assertNotIn(needle, log, f"typed text or secret leaked into the log: {needle!r}")
        self.assertNotRegex(log, re.escape(word) if len(word) > 4 else r"\A\Z")


class TestEquivalence(DaemonCase):
    def test_50_bench_swipes_match_swipekb_replay(self):
        for what, path in (("replay", REPLAY), ("layout", LAYOUT), ("models", os.path.join(ASSETS, "models"))):
            self.assertTrue(path and os.path.exists(path), f"{what} missing: {path!r}")
        swipes = swipe_words(BENCH, 50)
        self.assertEqual(len(swipes), 50)
        # 1. the oracle: write the swipes in the keyboard's recording format and replay them offline
        keys = keys_from_futo(LAYOUT)
        rec = os.path.join(self.tmp, "swipes.jsonl")
        with open(rec, "w") as f, open(os.path.join(self.tmp, "expect.txt"), "w") as e:
            for word, pts, ctx in swipes:
                f.write(json.dumps({"ts": 1, "type": "swipe", "context": ctx, "typed": word,
                                    "points": [{"x": x, "y": y, "t": t} for x, y, t in pts],
                                    "keys": [{"letter": k["c"], "x": k["x"], "y": k["y"], "w": k["w"], "h": k["h"]}
                                             for k in keys]}) + "\n")
                e.write(word + "\n")
        out = os.path.join(self.tmp, "oracle.json")
        run = subprocess.run([REPLAY, "--data", ASSETS, "--swipes", rec, "--expect", os.path.join(self.tmp, "expect.txt"),
                              "--candidates-json", out], capture_output=True, text=True, timeout=600)
        self.assertEqual(run.returncode, 0, run.stderr)
        oracle = json.load(open(out))
        self.assertEqual(len(oracle), 50)
        # 2. the daemon, same input
        self.start()
        c = self.connect("plain")
        self.assertTrue(c.call("layout", keys=keys)["ok"])
        mismatches = []
        for i, (word, pts, ctx) in enumerate(swipes):
            r = c.call("swipe", points=pts, context=ctx, top_k=3)
            self.assertTrue(r["ok"], r)
            # WHY "alt" entries are set aside: since M7 the daemon also offers the plugin's "other spelling" of the best
            # guess (its / it's), marked "alt"; swipekb-replay prints only the ranked guesses.
            got = [(x["word"], x["score"]) for x in r["candidates"] if not x.get("alt")]
            want = [(w, s) for w, s in oracle[i]]
            if got != want:
                mismatches.append((i, got, want))
        self.assertEqual(mismatches, [], "daemon candidates differ from swipekb-replay")
        self.assertTrue(any(oracle), "oracle produced no candidates at all")


class TestResources(DaemonCase):
    extra_args = ["--idle-unload-ms", "700"]

    def rss_mb(self, pid):
        for line in open(f"/proc/{pid}/status"):
            if line.startswith("VmRSS"):
                return int(line.split()[1]) / 1024
        raise AssertionError("no VmRSS")

    def cpu_ticks(self, pid):
        f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
        return int(f[11]) + int(f[12])   # utime + stime

    def wait_status(self, c, key, value, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if c.call("status")[key] == value:
                return
            time.sleep(0.05)
        self.fail(f"status {key} never became {value}")

    def test_models_load_lazily_and_unload_after_idle(self):
        self.start()
        c = self.connect()
        s = c.call("status")
        self.assertEqual((s["recognizer_loaded"], s["predictor_loaded"]), (False, False))
        c.call("layout", keys=keys_from_futo(LAYOUT))
        self.assertFalse(c.call("status")["recognizer_loaded"], "layout alone must not load models")
        word, pts, ctx = swipe_words(BENCH, 1)[0]
        self.assertTrue(c.call("swipe", points=pts)["ok"])
        self.assertTrue(c.call("status")["recognizer_loaded"])
        c.call("next_words", words=["the"])
        self.assertTrue(c.call("status")["predictor_loaded"])
        self.wait_status(c, "recognizer_loaded", False)   # status polls must not count as activity
        self.wait_status(c, "predictor_loaded", False)
        self.assertTrue(c.call("swipe", points=pts)["ok"], "must reload transparently after unloading")

    def test_idle_memory_and_cpu_within_budget(self):
        p = self.start(["--idle-unload-ms", "400"])
        c = self.connect()
        BUDGET_MB = 30   # plan Section 5: engine under 30 MB idle
        before = self.rss_mb(p.pid)
        self.assertLess(before, BUDGET_MB, f"fresh daemon uses {before:.1f} MB")
        t0 = self.cpu_ticks(p.pid)
        time.sleep(3)   # settle: idle CPU measured over a quiet window
        ticks = self.cpu_ticks(p.pid) - t0
        hz = os.sysconf("SC_CLK_TCK")
        self.assertLessEqual(ticks / hz / 3, 0.01, f"idle CPU {ticks} ticks in 3 s exceeds 1%")
        c.call("layout", keys=keys_from_futo(LAYOUT))
        word, pts, ctx = swipe_words(BENCH, 1)[0]
        c.call("swipe", points=pts)
        loaded = self.rss_mb(p.pid)
        self.assertGreater(loaded, before, "sanity: loading models should grow memory")
        self.wait_status(c, "recognizer_loaded", False)
        after = self.rss_mb(p.pid)
        self.assertLess(after, BUDGET_MB, f"after unload still {after:.1f} MB (loaded was {loaded:.1f} MB)")
        print(f"\nidle RSS fresh={before:.1f} MB, loaded={loaded:.1f} MB, after unload={after:.1f} MB", file=sys.stderr)


def can_make_network_namespace():
    """True only when `unshare -rn` really works here. Build containers, the Flatpak build sandbox and Ubuntu runners with
    AppArmor's user-namespace restriction cannot make one; that is a limit of the place, not of the daemon."""
    if not shutil.which("unshare"):
        return False
    return subprocess.run(["unshare", "-rn", "true"], capture_output=True).returncode == 0


def internet_sockets_open(pid):
    """Sockets process `pid` holds that are not local Unix sockets (TCP, UDP, raw, any IPv4/IPv6): list of (kind, inode).
    WHY this exists beside the namespace test: it needs no privileges, so the 'the daemon never touches the network' proof
    still runs in the build containers where a network namespace cannot be created."""
    inet = {}
    for kind in ("tcp", "tcp6", "udp", "udp6", "udplite", "udplite6", "raw", "raw6"):
        try:
            with open(f"/proc/{pid}/net/{kind}") as f:
                next(f, None)
                for line in f:
                    inet[line.split()[9]] = kind
        except OSError:
            pass
    found = []
    for fd in os.listdir(f"/proc/{pid}/fd"):
        try:
            target = os.readlink(f"/proc/{pid}/fd/{fd}")
        except OSError:
            continue
        m = re.fullmatch(r"socket:\[(\d+)\]", target)
        if m and m.group(1) in inet:
            found.append((inet[m.group(1)], m.group(1)))
    return found


class TestNoInternetSockets(DaemonCase):
    def test_the_checker_sees_tcp_and_udp_sockets(self):
        # guard for the guard: a process holding an internet socket must be reported, or the real test below proves nothing
        holder = subprocess.Popen([sys.executable, "-I", "-c",
            "import socket,time\na=socket.socket();a.bind(('127.0.0.1',0));a.listen()\n"
            "b=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);b.bind(('127.0.0.1',0))\nprint('up',flush=True);time.sleep(60)"],
            stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(holder.stdout.readline().strip(), "up")
            self.assertEqual(sorted(k for k, _ in internet_sockets_open(holder.pid)), ["tcp", "udp"])
        finally:
            holder.kill()
            holder.wait()

    def test_the_daemon_holds_no_internet_socket_through_a_whole_session(self):
        p = self.start()
        self.assertEqual(internet_sockets_open(p.pid), [], "at start")
        c = self.connect()
        c.call("layout", keys=keys_from_futo(LAYOUT))
        word, pts, ctx = swipe_words(BENCH, 1)[0]
        self.assertTrue(c.call("swipe", points=pts, context=ctx)["candidates"])
        self.assertTrue(c.call("next_words", words=["the"])["ok"])
        self.assertEqual(internet_sockets_open(p.pid), [], "after a swipe and next-word guesses")
        # the one socket it does hold is the local Unix one it listens on
        held = [os.readlink(f"/proc/{p.pid}/fd/{fd}") for fd in os.listdir(f"/proc/{p.pid}/fd")]
        self.assertTrue(any(h.startswith("socket:") for h in held))


class TestNoNetwork(DaemonCase):
    wrapper = ["unshare", "-rn"]

    def test_daemon_works_in_a_namespace_with_no_network_and_cannot_connect_out(self):
        # WHY a skip here and not a failure: where no namespace can be made nothing about the daemon can be learned; the
        # reason is printed so a skip is never mistaken for a pass, and TestNoInternetSockets still runs everywhere.
        if not can_make_network_namespace():
            self.skipTest("this machine cannot create a network namespace (unshare -rn); see TestNoInternetSockets instead")
        # the namespace really has no network: a TCP connect attempt fails
        attempt = subprocess.run(["unshare", "-rn", sys.executable, "-c",
            "import socket,sys\ntry:\n socket.create_connection(('127.0.0.1',80),2)\nexcept OSError as e:\n sys.exit(0)\nsys.exit(1)"])
        self.assertEqual(attempt.returncode, 0, "a connect from inside the namespace unexpectedly succeeded")
        # and the daemon does its whole job in there
        self.start()
        c = self.connect()
        c.call("layout", keys=keys_from_futo(LAYOUT))
        word, pts, ctx = swipe_words(BENCH, 1)[0]
        self.assertTrue(c.call("swipe", points=pts, context=ctx)["candidates"])
        self.assertTrue(c.call("next_words", words=["the"])["ok"])

    def test_daemon_code_has_no_network_calls(self):
        # Defence in depth beyond the namespace: the daemon's own source never names an internet socket API.
        root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "gaming", "daemon")
        banned = re.compile(r"AF_INET|AF_INET6|getaddrinfo|gethostbyname|QTcpSocket|QNetwork|curl_|SOCK_DGRAM")
        for name in os.listdir(root):
            if name.endswith((".cpp", ".hpp")):
                text = open(os.path.join(root, name)).read()
                self.assertIsNone(banned.search(text), f"{name} mentions a network API")
        ldd = subprocess.run(["ldd", DAEMON], capture_output=True, text=True).stdout
        self.assertNotRegex(ldd, r"libcurl|libssl|libQt6Network|libQt6Gui")


# ---- Milestone M7: multilingual ---------------------------------------------------------------------------------

# Steam's layout id for each (language, physical keys) the plugin's profiles use.
STEAM_ID = {("en", "qwerty"): "qwerty", ("es", "qwerty"): "spanish", ("fr", "azerty"): "french", ("de", "qwertz"): "german"}


def dataset_cases():
    return json.load(open(LANG_DATASET))["cases"]


def case_keys(case):
    return [{"c": k["letter"], "x": k["x"], "y": k["y"], "w": k["w"], "h": k["h"]} for k in case["keys"]]


def case_points(case):
    """Straight lines between key centres, 8 steps 16 ms apart: the same construction the plugin oracle used."""
    centre = {k["letter"]: (k["x"] + k["w"] / 2, k["y"] + k["h"] / 2) for k in case["keys"]}
    pts, prev = [], None
    for ch in case["traceKeys"]:
        x, y = centre[ch]
        if not pts:
            pts.append([x, y, 0])
        else:
            for step in range(1, 9):
                pts.append([prev[0] + (x - prev[0]) * step / 8, prev[1] + (y - prev[1]) * step / 8, len(pts) * 16])
        prev = (x, y)
    return pts


def steam_id_for(case):
    return STEAM_ID[(case["locale"].split("-")[0], case["layout"])]


class TestMultilingual(DaemonCase):
    def setUp(self):
        super().setUp()
        for what, path in (("language dataset", LANG_DATASET), ("plugin oracle output", ORACLE_JSON),
                           ("language packs", os.path.join(ASSETS, "language-packs"))):
            self.assertTrue(path and os.path.exists(path), f"{what} missing: {path!r} (ctest runs plugin_languageoracle first)")

    def layout_for(self, c, case):
        return c.call("layout", keys=case_keys(case), steam_layout=steam_id_for(case))

    def test_candidates_match_the_real_plugins_guess_bar_for_en_es_fr_de(self):
        # The plugin's list (committed word + bar) for each authored swipe; compared ignoring case because the plugin
        # capitalises the first word of an empty field and the daemon leaves capitals to its caller.
        oracle = json.load(open(ORACLE_JSON))["cases"]
        self.assertGreaterEqual(len(oracle), len(dataset_cases()) + 6)
        self.start()
        mismatches, seen_lang, with_alt = [], set(), 0
        for case in oracle:
            locale = case["locale"]
            # WHY a settings rewrite per case: the daemon must re-read the user's choice, so en_GB vs en_US is decided
            # by which English the user listed first, exactly as it will be live.
            self.write_settings(",".join([locale.replace("-", "_")] + ["en_US", "es_ES", "fr_FR", "de_DE"]))
            c = self.connect("plain")
            r = c.call("layout", keys=case_keys(case), steam_layout=steam_id_for(case))
            self.assertTrue(r["ok"], (case["id"], r))
            self.assertEqual(r["locale"], locale)
            got = c.call("swipe", points=case_points(case), top_k=3)
            self.assertTrue(got["ok"], got)
            c.call("close")
            self.conns.remove(c)
            words = [x["word"].lower() for x in got["candidates"]]
            want = [w.lower() for w in case["words"]]
            seen_lang.add(locale.split("-")[0])
            with_alt += any(x.get("alt") for x in got["candidates"])
            if words != want:
                mismatches.append((case["id"], words, want))
        self.assertEqual(seen_lang, {"en", "es", "fr", "de"})
        self.assertGreaterEqual(with_alt, 1, "no swipe exercised the other-spelling rule")
        self.assertEqual(mismatches, [], "daemon candidates differ from the plugin's for the same swipes")

    def test_regional_spelling_follows_the_first_enabled_english(self):
        # color/colour: en-US shows "color" first, en-GB "colour" (the plugin's regional re-ranking, reused).
        case = next(c for c in dataset_cases() if c["id"] == "en-US-2")
        self.start()
        firsts = {}
        for first in ("en_US", "en_GB"):
            self.write_settings(f"{first},es_ES")
            c = self.connect("plain")
            self.assertTrue(self.layout_for(c, case)["ok"])
            firsts[first] = c.call("swipe", points=case_points(case), top_k=3)["candidates"][0]["word"].lower()
        self.assertEqual(firsts, {"en_US": "color", "en_GB": "colour"})

    def test_other_spelling_is_offered_second_and_marked(self):
        # "its" is swiped for it's too: the plugin inserts the dictionary's other spelling right after the best guess.
        case = next(c for c in json.load(open(ORACLE_JSON))["cases"] if c["id"] == "en-US-extra-its")
        self.assertEqual(len(case["words"]), 4, "the real plugin offers its + it's + two more")
        self.start()
        c = self.connect("plain")
        c.call("layout", keys=case_keys(case), steam_layout="qwerty")
        r = c.call("swipe", points=case_points(case), top_k=3)
        self.assertEqual(len(r["candidates"]), 4, r)   # top 3 + the inserted alternative
        alts = [x for x in r["candidates"] if x.get("alt")]
        self.assertEqual([x["word"] for x in alts], ["it's"])
        self.assertEqual(r["candidates"][1], alts[0])
        self.assertEqual(sum("alt" in x for x in r["candidates"]), 1)
        # the alternative is typeable like any other candidate, by id
        self.assertEqual(c.call("commit", req=r["req"], cand=alts[0]["id"])["text"], "it's")

    def test_unsupported_scripts_and_layouts_reply_unsupported_and_leave_the_layer_off(self):
        self.start()
        case = dataset_cases()[0]
        c = self.connect("plain")
        for sid in ("schinese", "tchinese", "tchinese_bopomofo", "japanese", "japanese_kana", "korean", "russian", "bulgarian",
                    "ukrainian", "greek", "vietnamese", "czech", "dvorak", "colemak", "someday_valves_new_layout"):
            r = c.call("layout", keys=case_keys(case), steam_layout=sid)
            self.assertEqual(r, {"ok": False, "error": "unsupported"}, sid)
            # off means off: nothing may be answered from a layout that was refused, and the connection survives
            self.assertEqual(c.call("swipe", points=case_points(case))["error"], "unsupported", sid)
            self.assertEqual(c.call("next_words", words=["the"])["error"], "unsupported", sid)
        self.assertTrue(c.call("ping")["ok"])
        self.assertEqual(c.call("status")["profiles_loaded"], 0, "refusing a layout must not load any model")

    def test_a_cyrillic_key_table_is_refused_even_under_a_latin_layout_name(self):
        # Belt and braces (plan Section 13: at least 24 of the language's letters must be present).
        self.start()
        c = self.connect("plain")
        cyr = [{"c": chr(0x430 + i), "x": i * 10, "y": 0, "w": 10, "h": 10} for i in range(26)]
        self.assertEqual(c.call("layout", keys=cyr, steam_layout="qwerty")["error"], "unsupported")
        short = [k for k in case_keys(dataset_cases()[0]) if k["c"] not in "abc"]
        self.assertEqual(c.call("layout", keys=short, steam_layout="qwerty")["error"], "unsupported")

    def test_spanish_needs_its_n_tilde_key(self):
        self.start()
        c = self.connect("plain")
        case = next(c for c in dataset_cases() if c["id"] == "es-ES-1")
        no_ntilde = [k for k in case_keys(case) if k["c"] != "ñ"]
        self.assertEqual(c.call("layout", keys=no_ntilde, steam_layout="spanish")["error"], "unsupported")
        self.assertTrue(c.call("layout", keys=case_keys(case), steam_layout="spanish")["ok"])

    def test_a_language_the_user_has_not_enabled_stays_off(self):
        self.write_settings("es_ES")
        self.start()
        c = self.connect("plain")
        en = dataset_cases()[0]
        self.assertEqual(c.call("layout", keys=case_keys(en), steam_layout="qwerty"), {"ok": False, "error": "language_not_enabled"})
        self.assertEqual(c.call("swipe", points=case_points(en))["error"], "language_not_enabled")
        self.assertEqual(c.call("next_words", partial="ho")["error"], "language_not_enabled")
        es = next(x for x in dataset_cases() if x["id"] == "es-ES-1")
        self.assertTrue(self.layout_for(c, es)["ok"])
        self.assertEqual(c.call("status")["profiles_loaded"], 0)

    def test_enabling_a_language_takes_effect_without_restarting_the_daemon(self):
        self.write_settings("en_US")
        self.start()
        c = self.connect("plain")
        fr = next(x for x in dataset_cases() if x["id"] == "fr-FR-1")
        self.assertEqual(self.layout_for(c, fr)["error"], "language_not_enabled")
        self.write_settings("en_US,fr_FR")     # the user ticks French in Desktop Mode
        self.assertTrue(self.layout_for(c, fr)["ok"])
        self.write_settings("en_US")           # and unticks it again
        self.assertEqual(self.layout_for(c, fr)["error"], "language_not_enabled")

    def test_no_enabled_languages_means_the_system_language_like_the_desktop_keyboard(self):
        self.write_settings(None)    # file exists, nothing enabled
        self.start(["--system-locale", "de_DE.UTF-8"])
        c = self.connect("plain")
        de = next(x for x in dataset_cases() if x["id"] == "de-DE-1")
        en = dataset_cases()[0]
        self.assertTrue(self.layout_for(c, de)["ok"])
        self.assertEqual(c.call("layout", keys=case_keys(en), steam_layout="qwerty")["error"], "language_not_enabled")
        os.unlink(self.settings)     # no file at all: same rule
        self.assertTrue(self.layout_for(c, de)["ok"])

    def test_a_missing_language_pack_turns_that_language_off_not_english(self):
        # WHY: the plugin never falls back to English silently, and neither may we.
        os.environ["SWIPEKB_LANGUAGE_PACKS"] = os.path.join(self.tmp, "no-such-packs")
        self.addCleanup(os.environ.pop, "SWIPEKB_LANGUAGE_PACKS", None)
        self.start()
        c = self.connect("plain")
        es = next(x for x in dataset_cases() if x["id"] == "es-ES-1")
        self.assertEqual(self.layout_for(c, es), {"ok": False, "error": "language_unavailable"})
        self.assertEqual(c.call("swipe", points=case_points(es))["error"], "language_unavailable")
        self.assertTrue(self.layout_for(c, dataset_cases()[0])["ok"], "English must be unaffected")

    def test_switching_layout_mid_session_swaps_language_geometry_and_drops_old_candidates(self):
        self.start()
        c = self.connect("plain")
        es = next(x for x in dataset_cases() if x["id"] == "es-ES-1")
        de = next(x for x in dataset_cases() if x["id"] == "de-DE-3")
        self.assertEqual(self.layout_for(c, es)["language"], "es")
        r1 = c.call("swipe", points=case_points(es))
        self.assertIn("casa", [x["word"] for x in r1["candidates"]], r1)
        r = self.layout_for(c, de)
        self.assertEqual((r["ok"], r["language"]), (True, "de"))
        # the old request's ids are gone: a switch must not let a Spanish word be typed into a German session
        self.assertEqual(c.call("commit", req=r1["req"], cand=r1["candidates"][0]["id"])["error"], "unknown_candidate")
        r2 = c.call("swipe", points=case_points(de), top_k=3)
        self.assertIn("straße", [x["word"].lower() for x in r2["candidates"]], r2)   # German geometry (QWERTZ) and German words
        # and back to a refused layout: layer off again, nothing left over from German
        self.assertEqual(c.call("layout", keys=case_keys(de), steam_layout="russian")["error"], "unsupported")
        self.assertEqual(c.call("commit", req=r2["req"], cand=r2["candidates"][0]["id"])["error"], "unknown_candidate")

    def test_n_tilde_acute_umlaut_and_eszett_words_round_trip_through_commit(self):
        self.start()
        c = self.connect("plain")
        seen = {}
        for cid, ch in (("es-ES-3", "í"), ("es-ES-4", "ñ"), ("fr-FR-1", "é"), ("de-DE-1", "ö"), ("de-DE-3", "ß")):
            case = next(x for x in dataset_cases() if x["id"] == cid)
            self.assertTrue(self.layout_for(c, case)["ok"])
            r = c.call("swipe", points=case_points(case), top_k=10)
            hit = [x for x in r["candidates"] if ch in x["word"]]
            self.assertTrue(hit, (cid, [x["word"] for x in r["candidates"]]))
            got = c.call("commit", req=r["req"], cand=hit[0]["id"])
            self.assertEqual(got["text"], hit[0]["word"])
            self.assertEqual(got["text"], __import__("unicodedata").normalize("NFC", got["text"]))
            seen[ch] = got["text"]
        self.assertEqual(set(seen), {"í", "ñ", "é", "ö", "ß"})

    def test_ue_oe_ss_words_and_decomposed_input_work_in_next_words(self):
        self.start()
        c = self.connect("plain")
        es = next(x for x in dataset_cases() if x["id"] == "es-ES-1")
        self.layout_for(c, es)
        composed = c.call("next_words", partial="pingü", top_k=5)
        decomposed = c.call("next_words", partial="pingu\u0308", top_k=5)   # u + combining diaeresis
        self.assertTrue(composed["ok"] and decomposed["ok"])
        self.assertEqual([x["word"] for x in composed["candidates"]], [x["word"] for x in decomposed["candidates"]])
        self.assertIn("pingüino", [x["word"] for x in composed["candidates"]])
        de = next(x for x in dataset_cases() if x["id"] == "de-DE-1")
        self.layout_for(c, de)
        for partial, want in (("Straß", "Straße"), ("schö", "schön"), ("über", "über")):
            words = [x["word"] for x in c.call("next_words", partial=partial, top_k=5)["candidates"]]
            self.assertIn(want, words, partial)

    def test_next_words_use_the_languages_own_model(self):
        self.start()
        c = self.connect("plain")
        out = {}
        for cid, ctx in (("es-ES-1", ["me", "gusta"]), ("fr-FR-1", ["je", "suis"]), ("de-DE-1", ["ich", "bin"])):
            case = next(x for x in dataset_cases() if x["id"] == cid)
            self.assertTrue(self.layout_for(c, case)["ok"])
            r = c.call("next_words", words=ctx, top_k=5)
            self.assertTrue(r["ok"] and r["candidates"], (cid, r))
            out[cid] = [x["word"] for x in r["candidates"]]
        self.assertEqual(len({tuple(v) for v in out.values()}), 3, out)

    def test_at_most_two_languages_stay_loaded_and_all_unload_when_idle(self):
        self.start(["--idle-unload-ms", "700"])
        c = self.connect("plain")
        for cid in ("en-US-1", "es-ES-1", "fr-FR-1"):
            case = next(x for x in dataset_cases() if x["id"] == cid)
            self.layout_for(c, case)
            self.assertTrue(c.call("swipe", points=case_points(case))["ok"])
            self.assertLessEqual(c.call("status")["profiles_loaded"], 2)
        self.assertEqual(c.call("status")["profiles_loaded"], 2)
        deadline = time.monotonic() + 30
        while c.call("status")["profiles_loaded"] and time.monotonic() < deadline:
            time.sleep(0.1)
        s = c.call("status")
        self.assertEqual((s["profiles_loaded"], s["recognizer_loaded"], s["predictor_loaded"]), (0, False, False))


class TestMultilingualPrivacy(DaemonCase):
    """The M2 privacy guarantees, repeated for accented, non-English text (plan Section 8)."""

    def setUp(self):
        super().setUp()
        self.assertTrue(LANG_DATASET and os.path.exists(LANG_DATASET), LANG_DATASET)

    def layout(self, c, cid):
        case = next(x for x in dataset_cases() if x["id"] == cid)
        r = c.call("layout", keys=case_keys(case), steam_layout=steam_id_for(case))
        self.assertTrue(r["ok"], r)
        return case

    def test_commit_by_id_only_with_accented_words(self):
        self.start()
        c = self.connect("plain")
        case = self.layout(c, "de-DE-3")
        r = c.call("swipe", points=case_points(case), top_k=5)
        word = next(x["word"] for x in r["candidates"] if "ß" in x["word"])
        for bad in (word, word.encode("utf-8").hex(), "c" + word, "Straße"):
            self.assertEqual(c.call("commit", req=r["req"], cand=bad).get("error"), "unknown_candidate", bad)

    def test_unknown_mode_stores_nothing_in_any_language(self):
        self.start()
        a = self.connect("unknown")
        case = self.layout(a, "es-ES-4")
        a.call("swipe", points=case_points(case), context=["el", "niño", "pequeño"])
        later = a.call("swipe", points=case_points(case))   # no context now: nothing may be remembered
        fresh = self.connect("unknown")
        self.layout(fresh, "es-ES-4")
        baseline = fresh.call("swipe", points=case_points(case))
        words = lambda r: [(x["word"], x["score"]) for x in r["candidates"]]
        self.assertEqual(words(later), words(baseline))
        a.call("next_words", words=["el", "niño"])
        again = a.call("next_words")   # nothing given this time: the earlier words must not come back
        fresh_again = fresh.call("next_words")
        self.assertEqual([x["word"] for x in again["candidates"]], [x["word"] for x in fresh_again["candidates"]])

    def test_unknown_mode_still_refuses_secret_looking_text_in_other_languages(self):
        self.start()
        u = self.connect("unknown")
        self.layout(u, "fr-FR-1")
        for secret in ("Xk9#mQ2$vLp", "Çà9é#Mq2$vLpé"):
            r = u.call("next_words", words=[secret])
            self.assertEqual((r["ok"], r["candidates"]), (True, []), secret)
            self.assertEqual(u.call("next_words", partial=secret)["candidates"], [])
        # while ordinary accented French is fine
        self.assertTrue(u.call("next_words", partial="éco", top_k=3)["candidates"])

    def test_logs_hold_no_words_locales_or_layout_names_in_any_language(self):
        self.start()
        c = self.connect("plain")
        needles = ["zebracornflux", "señorita", "überfluß", "français", "straße"]
        seen = set()
        for cid in ("es-ES-4", "fr-FR-1", "de-DE-3"):
            case = self.layout(c, cid)
            r = c.call("swipe", points=case_points(case), context=needles, top_k=5)
            seen |= {x["word"] for x in r["candidates"]} | {x["id"] for x in r["candidates"]}
            c.call("next_words", words=needles, partial="señ")
            c.call("commit", req=r["req"], cand=r["candidates"][0]["id"])
        c.call("layout", keys=case_keys(case), steam_layout="russian")
        c.call("layout", keys=case_keys(case), steam_layout="not a layout señor")
        c.call("close")
        time.sleep(0.2)
        log = self.log()
        self.assertTrue(log.strip())
        for needle in needles + ["señ", self.token(), *seen, "spanish", "french", "german", "russian", "es-ES", "fr-FR", "de-DE", "plasmakeyboardrc"]:
            self.assertNotIn(needle, log, f"typed text, language or secret leaked into the log: {needle!r}")

    def test_the_settings_file_is_only_read_never_changed_or_created(self):
        self.start()
        before = open(self.settings).read()
        mtime = os.stat(self.settings).st_mtime_ns
        c = self.connect("plain")
        self.layout(c, "es-ES-1")
        missing = os.path.join(self.tmp, "absent-rc")
        self.procs[0].send_signal(signal.SIGTERM)
        self.procs[0].wait(10)
        self.start(["--settings-file", missing])
        c2 = self.connect("plain")
        c2.call("layout", keys=case_keys(dataset_cases()[0]), steam_layout="qwerty")
        self.assertFalse(os.path.exists(missing), "reading a missing settings file must not create it")
        self.assertEqual((open(self.settings).read(), os.stat(self.settings).st_mtime_ns), (before, mtime))


if __name__ == "__main__":
    unittest.main(verbosity=2)
