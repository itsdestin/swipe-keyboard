#!/usr/bin/env python3
"""Offline tests for h2h.py: mapping math, touch scripts, dump parsing, calibration and scoring.
Synthetic data only (never the owner's swipes). Run: python3 -I tools/gboard-h2h/test_h2h.py"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import h2h  # noqa: E402


def linux_keys():
    """Same shape as the tablet's recorded key rects (148x79 keys, rows offset 74 and 185)."""
    keys = []
    for r, (row, off) in enumerate(zip(h2h.ROWS, (0, 74, 185))):
        for i, c in enumerate(row):
            keys.append({"letter": c, "x": off + i * 148, "y": r * 79, "w": 148, "h": 79})
    return keys


class MappingTest(unittest.TestCase):
    def setUp(self):
        self.src = h2h.Grid(linux_keys())
        self.dst = h2h.Grid(h2h.synthetic_phone_keys(left=12, top=1600, width=1056, row_h=160))

    def test_identity(self):
        for x, y in [(10, 5), (700, 120), (1400, 230), (-30, -20), (1600, 300)]:
            X, Y = h2h.map_point(self.src, self.src, x, y)
            self.assertAlmostEqual(X, x, places=6)
            self.assertAlmostEqual(Y, y, places=6)

    def test_key_centres_map_to_key_centres(self):
        for c in "abcdefghijklmnopqrstuvwxyz":
            X, Y = h2h.map_point(self.src, self.dst, *self.src.center(c))
            ex, ey = self.dst.center(c)
            self.assertAlmostEqual(X, ex, places=6, msg=c)
            self.assertAlmostEqual(Y, ey, places=6, msg=c)

    def test_sixty_percent_g_to_h(self):
        gx, gy = self.src.center("g")
        hx, _ = self.src.center("h")
        X, Y = h2h.map_point(self.src, self.dst, gx + 0.6 * (hx - gx), gy)
        Gx, Gy = self.dst.center("g")
        Hx, _ = self.dst.center("h")
        self.assertAlmostEqual((X - Gx) / (Hx - Gx), 0.6, places=6)
        self.assertAlmostEqual(Y, Gy, places=6)

    def test_halfway_between_rows(self):
        # Halfway between the centres of 't' (row 0) and 'g' (row 1) lands halfway on the phone.
        tx, ty = self.src.center("t")
        gx, gy = self.src.center("g")
        X, Y = h2h.map_point(self.src, self.dst, (tx + gx) / 2, (ty + gy) / 2)
        Tx, Ty = self.dst.center("t")
        Gx, Gy = self.dst.center("g")
        self.assertAlmostEqual(Y, (Ty + Gy) / 2, places=6)
        self.assertAlmostEqual(X, (Tx + Gx) / 2, places=6)

    def test_outside_extrapolates(self):
        # Left of 'q' by half a key stays left of the phone's 'q' by half a phone key.
        qx, qy = self.src.center("q")
        X, Y = h2h.map_point(self.src, self.dst, qx - 74, qy - 39.5)
        Qx, Qy = self.dst.center("q")
        self.assertAlmostEqual(X, Qx - 105.6 / 2, places=4)
        self.assertAlmostEqual(Y, Qy - 80, places=4)

    def test_continuous_across_middle_row(self):
        _, ay = self.src.center("a")
        for x in (100, 500, 900, 1300):
            a = h2h.map_point(self.src, self.dst, x, ay - 1e-6)
            b = h2h.map_point(self.src, self.dst, x, ay + 1e-6)
            self.assertAlmostEqual(a[0], b[0], places=3)

    def test_non_uniform_phone_grid(self):
        # A phone with wider middle-row gaps: per-key mapping still hits each centre.
        keys = h2h.synthetic_phone_keys()
        for k in keys:
            if k["letter"] in "jkl":
                k["x"] += 20
        dst = h2h.Grid(keys)
        for c in "hjkl":
            X, _ = h2h.map_point(self.src, dst, *self.src.center(c))
            self.assertAlmostEqual(X, dst.center(c)[0], places=6)

    def test_bad_grid(self):
        keys = linux_keys()[:-1]
        with self.assertRaises(ValueError):
            h2h.Grid(keys)

    def test_map_swipe_keeps_time(self):
        sw = {"keys": linux_keys(),
              "points": [{"t": 0, "x": 74, "y": 39.5}, {"t": 17, "x": 222, "y": 39.5}]}
        pts = h2h.map_swipe(sw, self.dst)
        self.assertEqual([p[0] for p in pts], [0, 17])
        self.assertEqual((pts[0][1], pts[0][2]), tuple(int(round(v)) for v in self.dst.center("q")))


class SimplifyTest(unittest.TestCase):
    def test_line_collapses(self):
        pts = [(i * 10, i * 5, 100) for i in range(20)]
        s = h2h.simplify(pts, 2.0, min_events=2)
        self.assertEqual(s, [pts[0], pts[-1]])
        s = h2h.simplify(pts, 2.0)
        self.assertEqual(len(s), 6)
        self.assertEqual((s[0], s[-1]), (pts[0], pts[-1]))

    def test_corner_kept(self):
        pts = [(i, i * 10, 0) for i in range(10)] + [(10 + i, 90, i * 10) for i in range(1, 10)]
        s = h2h.simplify(pts, 2.0, min_events=2)
        self.assertIn((9, 90, 0), s)
        self.assertEqual(len(s), 3)

    def test_duplicates_dropped_end_time_kept(self):
        pts = [(0, 1, 1), (5, 1, 1), (9, 2, 2), (20, 2, 2)]
        s = h2h.simplify(pts, 0)
        self.assertEqual(s, [(0, 1, 1), (20, 2, 2)])  # floor can't exceed the distinct points


class ScriptTest(unittest.TestCase):
    def test_structure(self):
        pts = [(100, 1, 2), (110, 3, 4), (130, 5, 6)]
        s = h2h.motion_script(pts)
        ev = [l for l in s.splitlines() if l.startswith("input ")]
        self.assertEqual(ev, ["input motionevent DOWN 1 2", "input motionevent MOVE 3 4",
                              "input motionevent UP 5 6"])
        waits = [l for l in s.splitlines() if l.startswith("w ")]
        self.assertEqual(waits, ["w 0", "w 10000", "w 30000"])

    def test_single_point_is_tap(self):
        s = h2h.motion_script([(0, 7, 8)], pace=False)
        self.assertEqual(s.split("\n")[:2], ["input motionevent DOWN 7 8", "input motionevent UP 7 8"])

    @unittest.skipUnless(shutil.which("bash"), "needs bash")
    def test_pacing_runs_in_shell(self):
        # Run the generated script with `input` stubbed out: it must wait ~120 ms in total and
        # report a timing line per event (mksh on the phone and bash share this syntax).
        pts = [(0, 0, 0), (40, 1, 1), (80, 2, 2), (120, 3, 3)]
        script = "input() { :; }\n" + h2h.motion_script(pts)
        out = subprocess.run(["bash"], input=script, capture_output=True, text=True, timeout=10)
        self.assertEqual(out.returncode, 0, out.stderr)
        pairs = h2h.parse_timing(out.stdout)
        self.assertEqual([t for t, _ in pairs], [0, 40, 80, 120])
        st = h2h.timing_stats(pairs)
        self.assertGreaterEqual(st["achieved_ms"], 115)
        self.assertLess(st["achieved_ms"], 300)

    def test_timing_stats(self):
        st = h2h.timing_stats([(0, 0), (100, 150), (200, 400)])
        self.assertEqual(st["stretch"], 2.0)
        self.assertEqual(st["max_lag_ms"], 200)


DUMP = """<?xml version='1.0' encoding='UTF-8' standalone='yes' ?><hierarchy rotation="0">
<node index="0" text="" class="android.widget.FrameLayout" package="com.google.android.keep" bounds="[0,0][1080,2400]">
<node index="1" text="Note" class="android.widget.EditText" package="com.google.android.keep" focused="false" bounds="[0,100][1080,200]"/>
<node index="2" text="Hello" class="android.widget.EditText" package="com.google.android.keep" focused="true" bounds="[0,200][1080,400]"/>
</node>
<node index="0" class="android.widget.FrameLayout" package="com.google.android.inputmethod.latin" bounds="[0,1400][1080,2400]">
%s
<node text="hello" content-desc="" package="com.google.android.inputmethod.latin" bounds="[360,1420][720,1500]"/>
<node text="" content-desc="help" package="com.google.android.inputmethod.latin" bounds="[0,1420][360,1500]"/>
<node text="hell" content-desc="" package="com.google.android.inputmethod.latin" bounds="[720,1420][1080,1500]"/>
</node></hierarchy>"""


def dump_with_keys():
    nodes = []
    for k in h2h.synthetic_phone_keys():
        nodes.append('<node text="" content-desc="%s" package="com.google.android.inputmethod.latin" '
                     'bounds="[%d,%d][%d,%d]"/>' % (k["letter"].upper(), k["x"], k["y"],
                                                   k["x"] + k["w"], k["y"] + k["h"]))
    return DUMP % "\n".join(nodes)


class DumpTest(unittest.TestCase):
    def test_keys(self):
        keys = h2h.keys_from_dump(dump_with_keys())
        self.assertEqual(len(keys), 26)
        h2h.Grid(keys)

    def test_no_keys(self):
        self.assertIsNone(h2h.keys_from_dump(DUMP % ""))

    def test_strip_and_field(self):
        xml = dump_with_keys()
        keys = h2h.keys_from_dump(xml)
        self.assertEqual(h2h.strip_from_dump(xml, keys), ["help", "hello", "hell"])
        self.assertEqual(h2h.field_text(xml, "com.google.android.keep"), "Hello")
        self.assertIsNone(h2h.field_text(xml, "com.samsung.android.app.notes"))


class PixelCalibrationTest(unittest.TestCase):
    def test_finds_keys(self):
        W, H = 600, 500
        frame = (0, 150, W, H)
        bg, key = (30, 30, 30), (70, 70, 70)
        img = bytearray(bytes(bg) + b"\xff") * (W * H)

        def rect(x0, y0, x1, y1):
            for y in range(y0, y1):
                for x in range(x0, x1):
                    o = (y * W + x) * 4
                    img[o:o + 3] = bytes(key)

        kw, gap = 60, 6
        rows_y = [(200, 260), (270, 330), (340, 400), (410, 470)]  # 3 letter rows + space row
        for i in range(10):
            rect(i * kw + 3, 200, i * kw + kw - 3, 260)
        for i in range(9):
            rect(30 + i * kw + 3, 270, 30 + i * kw + kw - 3, 330)
        rect(3, 340, 87, 400)                                       # shift
        for i in range(7):
            rect(90 + i * kw + 3, 340, 90 + i * kw + kw - 3, 400)
        rect(513, 340, 597, 400)                                    # backspace
        rect(3, 410, 120, 470); rect(126, 410, 474, 470); rect(480, 410, 597, 470)
        keys = h2h.keys_from_pixels(W, H, bytes(img), frame)
        g = h2h.Grid(keys)
        self.assertAlmostEqual(g.center("q")[0], 30, delta=1)
        self.assertAlmostEqual(g.center("a")[0], 60, delta=1)
        self.assertAlmostEqual(g.center("z")[0], 120, delta=1)
        self.assertAlmostEqual(g.center("m")[1], 370, delta=1)
        del rows_y, gap


REPLAY_OUT = """settings: decoder=on
#    expected      guess1        guess2        guess3        result  live-typed
1    in            in            on            inn           first   In
2    to            it            its                         MISS    it
3    -             (skipped)
4    hello         help          hello         hell          top3    help

summary: 3 scored
"""


class ScoreTest(unittest.TestCase):
    def test_parse_replay(self):
        t = h2h.parse_replay_table(REPLAY_OUT)
        self.assertEqual(t[1], ["in", "on", "inn"])
        self.assertEqual(t[2], ["it", "its"])
        self.assertIsNone(t[3])
        self.assertEqual(t[4], ["help", "hello", "hell"])

    def test_score_and_summary(self):
        ours = h2h.parse_replay_table(REPLAY_OUT)
        res = [
            {"index": 1, "expected": "in", "gboard": {"first": "In", "strip": ["on", "inn"]},
             "ours": ours[1]},
            {"index": 2, "expected": "to", "gboard": {"first": "to", "strip": []}, "ours": ours[2]},
            {"index": 3, "expected": "-"},
            {"index": 4, "expected": "hello", "gboard": {"first": "hell", "strip": ["hello."]},
             "ours": ours[4]},
        ]
        s = h2h.summarize(res)
        self.assertEqual(s["gboard"], {"scored": 3, "first": 2, "top3": 3,
                                       "first_pct": 66.7, "top3_pct": 100.0})
        self.assertEqual((s["ours"]["first"], s["ours"]["top3"]), (1, 2))
        tbl = h2h.table(res)
        self.assertIn("(skipped)", tbl)
        self.assertIn("*In", tbl)


class LoadTest(unittest.TestCase):
    def test_outcome_lines_and_context_dropped(self):
        d = tempfile.mkdtemp()
        p = os.path.join(d, "s.jsonl")
        sw = {"points": [{"t": 0, "x": 1, "y": 1}], "keys": linux_keys(), "context": "secret"}
        with open(p, "w") as f:
            f.write(json.dumps(sw) + "\n")
            f.write(json.dumps({"type": "pick", "word": "x"}) + "\n\n")
            f.write(json.dumps({"type": "erase"}) + "\n")
            f.write(json.dumps(sw) + "\n")
        s = h2h.load_swipes(p)
        self.assertEqual(len(s), 2)
        self.assertNotIn("context", s[0])
        shutil.rmtree(d)


if __name__ == "__main__":
    unittest.main()
