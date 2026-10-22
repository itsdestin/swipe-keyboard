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


def swipe_words(path, limit):
    """First `limit` scorable swipes of the bench corpus, as (target, points[[x,y,t]], context[])."""
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
            DAEMON, "--socket", self.sock, "--token-file", self.tokfile, "--data", ASSETS, *self.extra_args, *extra]
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
        for needle in [SENTINEL, "qxzvk", self.token(), *seen, "marmalade"]:
            self.assertNotIn(needle, log, f"typed text or secret leaked into the log: {needle!r}")
        self.assertNotRegex(log, re.escape(word) if len(word) > 4 else r"\A\Z")


class TestEquivalence(DaemonCase):
    def test_50_bench_swipes_match_swipekb_replay(self):
        for what, path in (("replay", REPLAY), ("layout", LAYOUT), ("bench corpus", BENCH),
                           ("models", os.path.join(ASSETS, "models"))):
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
            got = [(x["word"], x["score"]) for x in r["candidates"]]
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


@unittest.skipUnless(shutil.which("unshare"), "unshare not installed")
class TestNoNetwork(DaemonCase):
    wrapper = ["unshare", "-rn"]

    def test_daemon_works_in_a_namespace_with_no_network_and_cannot_connect_out(self):
        probe = subprocess.run(["unshare", "-rn", "true"], capture_output=True)
        if probe.returncode != 0:
            self.fail("this machine cannot create a network namespace (unshare -rn): " + probe.stderr.decode())
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
