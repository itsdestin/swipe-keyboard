#!/usr/bin/env python3
"""Head-to-head benchmark: replay the owner's recorded swipes into Gboard on his phone.

WHY this exists: our engine's score on the owner's swipes means little on its own. This tool
types the very same swipes into Gboard (black-box: we only send touches and read back the text it
types), then scores both against the same answer key, side by side.

Run with `python3 -I tools/gboard-h2h/h2h.py <command>`. Stdlib only. Commands, in the order the
operator normally uses them (each one stops on anything unexpected; none of them taps unless named):

  check        no touches: phone connected and unlocked? notes app + Gboard in front? screenshot
  open-note    no touches: launch the notes app (creating the blank note is left to the operator)
  geometry     no touches: find Gboard's 26 letter keys; saves phone-keys.json
  verify       taps 3 letters, reads them back, deletes them
  run          the benchmark: swipes, reads, clears, scores; writes results-<date>.json
  plan         offline: show the touch script one swipe would produce (no phone needed)
  report       offline: reprint the table from a results JSON
  clear        deletes all text in the focused note field

Everything written goes to /tmp/claude-1000/h2h/, never the repo: the swipes are private data.
"""
import argparse
import datetime
import json
import os
import re
import socket
import struct
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

WORK = "/tmp/claude-1000/h2h"
DEFAULT_SWIPES = os.path.join(WORK, "swipes.jsonl")
DEFAULT_EXPECT = "/tmp/claude-1000/replay/expect.txt"
DEFAULT_REPLAY = "/home/destin/swipe-keyboard-m0/build/tools/swipekb-replay"
DEFAULT_DATA = os.path.expanduser("~/.local/share/swipe-keyboard")
ADB = "/usr/bin/adb"
GBOARD_PKG = "com.google.android.inputmethod.latin"
NOTES_PKGS = ["com.samsung.android.app.notes", "com.google.android.keep"]
ROWS = ["qwertyuiop", "asdfghjkl", "zxcvbnm"]
OUTCOME_TYPES = {"pick", "erase", "keep"}
DEVICE_XML = "/data/local/tmp/h2h-ui.xml"


class Stop(Exception):
    """Something unexpected: stop and tell the operator, never click through it."""


# ---------------------------------------------------------------------------------------------
# Swipe file
# ---------------------------------------------------------------------------------------------

def load_swipes(path):
    """Replayable swipes in file order. Outcome lines (pick/erase/keep) are skipped.
    WHY drop `context`: it is the owner's private text and nothing here needs it."""
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            d = json.loads(line)
            if d.get("type") in OUTCOME_TYPES:
                continue
            if not d.get("points") or not d.get("keys"):
                continue
            d.pop("context", None)
            out.append(d)
    return out


def load_expect(path):
    with open(path, encoding="utf-8") as f:
        return [l.strip() for l in f]  # one per swipe, in order; "-" = skip


# ---------------------------------------------------------------------------------------------
# Key grids and the Linux -> phone mapping
# ---------------------------------------------------------------------------------------------

def interp(v, xs, ys):
    """Piecewise-linear map with straight-line extrapolation past both ends.
    WHY extrapolate: swipes often start or end past the outer keys; they must stay outside."""
    if len(xs) == 1:
        return ys[0] + (v - xs[0])
    if v <= xs[0]:
        i = 0
    elif v >= xs[-1]:
        i = len(xs) - 2
    else:
        i = 0
        while not (xs[i] <= v <= xs[i + 1]):
            i += 1
    x0, x1, y0, y1 = xs[i], xs[i + 1], ys[i], ys[i + 1]
    if x1 == x0:
        return y0
    return y0 + (v - x0) * (y1 - y0) / (x1 - x0)


class Grid:
    """The 26 letter keys as three rows of key centres. Only centres matter: a point is described
    as "row 1.3, 60% of the way from g to h", which means the same thing on any keyboard."""

    def __init__(self, keys):
        by = {}
        for k in keys:
            by[k["letter"].lower()] = k
        missing = [c for c in "abcdefghijklmnopqrstuvwxyz" if c not in by]
        if missing:
            raise ValueError("key grid is missing letters: " + "".join(missing))
        self.keys = by
        self.row_x = []   # per row: key-centre x in letter order
        self.row_y = []   # per row: mean centre y
        for row in ROWS:
            cx = [by[c]["x"] + by[c]["w"] / 2.0 for c in row]
            cy = [by[c]["y"] + by[c]["h"] / 2.0 for c in row]
            self.row_x.append(cx)
            self.row_y.append(sum(cy) / len(cy))
        for cx in self.row_x:
            if any(b <= a for a, b in zip(cx, cx[1:])):
                raise ValueError("key centres in a row are not left-to-right")
        if not (self.row_y[0] < self.row_y[1] < self.row_y[2]):
            raise ValueError("letter rows are not top-to-bottom")

    def to_rc(self, x, y):
        """Pixel -> (row coordinate, column coordinate in each row)."""
        r = interp(y, self.row_y, [0.0, 1.0, 2.0])
        cols = [interp(x, cx, list(range(len(cx)))) for cx in self.row_x]
        return r, cols

    def from_rc(self, r, cols):
        """(row coordinate, per-row column coordinates) -> pixel. Between two rows, the x from
        each row is blended by how far between them the point is, so motion stays continuous."""
        y = interp(r, [0.0, 1.0, 2.0], self.row_y)
        i = 0 if r < 1.0 else 1
        f = min(max(r - i, 0.0), 1.0)
        xa = interp(cols[i], list(range(len(self.row_x[i]))), self.row_x[i])
        xb = interp(cols[i + 1], list(range(len(self.row_x[i + 1]))), self.row_x[i + 1])
        return xa * (1 - f) + xb * f, y

    def center(self, letter):
        k = self.keys[letter]
        return k["x"] + k["w"] / 2.0, k["y"] + k["h"] / 2.0


def map_point(src, dst, x, y):
    r, cols = src.to_rc(x, y)
    return dst.from_rc(r, cols)


def map_swipe(swipe, dst):
    src = Grid(swipe["keys"])
    out = []
    for p in swipe["points"]:
        X, Y = map_point(src, dst, p["x"], p["y"])
        out.append((int(round(p["t"])), int(round(X)), int(round(Y))))
    return out


def simplify(pts, tol, min_events=6):
    """Drop points that sit within `tol` px of the straight line through their neighbours
    (Ramer-Douglas-Peucker). WHY: each `input` call costs ~100 ms on the phone, so sending fewer
    moves keeps the swipe's speed closer to the original while keeping its shape. Timestamps of
    kept points are untouched. tol <= 0 keeps everything except exact repeats. At least
    `min_events` evenly spread points are always kept."""
    ded = [pts[0]]
    for p in pts[1:]:
        if (p[1], p[2]) != (ded[-1][1], ded[-1][2]):
            ded.append(p)
    if ded[-1] is not pts[-1]:
        ded[-1] = (pts[-1][0], ded[-1][1], ded[-1][2])  # keep the true end time
    if tol <= 0 or len(ded) < 3:
        return ded

    def dist(p, a, b):
        ax, ay, bx, by, px, py = a[1], a[2], b[1], b[2], p[1], p[2]
        dx, dy = bx - ax, by - ay
        L = dx * dx + dy * dy
        if L == 0:
            return ((px - ax) ** 2 + (py - ay) ** 2) ** 0.5
        u = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / L))
        return ((px - ax - u * dx) ** 2 + (py - ay - u * dy) ** 2) ** 0.5

    keep = [False] * len(ded)
    keep[0] = keep[-1] = True
    stack = [(0, len(ded) - 1)]
    while stack:
        a, b = stack.pop()
        best, bi = -1.0, -1
        for i in range(a + 1, b):
            d = dist(ded[i], ded[a], ded[b])
            if d > best:
                best, bi = d, i
        if bi >= 0 and best > tol:
            keep[bi] = True
            stack += [(a, bi), (bi, b)]
    # WHY a floor: a straight swipe would shrink to DOWN + UP, which a keyboard can read as a tap.
    want = min(min_events, len(ded))
    if sum(keep) < want:
        for j in range(want):
            keep[round(j * (len(ded) - 1) / (want - 1))] = True
    return [p for p, k in zip(ded, keep) if k]


# ---------------------------------------------------------------------------------------------
# Touch scripts
# ---------------------------------------------------------------------------------------------

# WHY this preamble: Android's shell (mksh) has $EPOCHREALTIME, a microsecond clock that costs no
# extra process. Each event waits until its recorded time measured from the swipe start, so delays
# don't pile up; when the phone is behind it simply doesn't sleep. Each event's real start time is
# printed so the tool can measure how faithful the timing was. Written to run the same in mksh
# and bash, so the unit tests can execute it on the PC.
_SH_PREAMBLE = r"""e=$EPOCHREALTIME; S0=${e%.*}; U0=${e#*.}
w() { e=$EPOCHREALTIME; N=$(( (${e%.*}-S0)*1000000 + 10#${e#*.} - 10#$U0 )); d=$(( $1 - N ));
 if [ $d -gt 1500 ]; then z=$(( d % 1000000 + 1000000 )); sleep $(( d / 1000000 )).${z#1}; fi;
 e=$EPOCHREALTIME; N=$(( (${e%.*}-S0)*1000000 + 10#${e#*.} - 10#$U0 )); echo "@ $1 $N"; }
"""


def motion_script(pts, pace=True):
    """One shell script that performs the whole swipe as a single touch: DOWN, MOVEs, UP."""
    t0 = pts[0][0]
    lines = [_SH_PREAMBLE] if pace else []
    n = len(pts)
    for i, (t, x, y) in enumerate(pts):
        act = "DOWN" if i == 0 else ("UP" if i == n - 1 else "MOVE")
        if n == 1:
            act = "DOWN"
        if pace:
            lines.append("w %d" % ((t - t0) * 1000))
        lines.append("input motionevent %s %d %d" % (act, x, y))
    if n == 1:
        lines.append("input motionevent UP %d %d" % (pts[0][1], pts[0][2]))
    return "\n".join(lines) + "\n"


def parse_timing(output):
    """'@ target_us actual_us' lines -> list of (target_ms, actual_ms)."""
    res = []
    for line in output.splitlines():
        m = re.match(r"^@ (\d+) (-?\d+)$", line.strip())
        if m:
            res.append((int(m.group(1)) / 1000.0, int(m.group(2)) / 1000.0))
    return res


def timing_stats(pairs):
    if not pairs:
        return None
    errs = [a - t for t, a in pairs]
    rec = pairs[-1][0] - pairs[0][0]
    got = pairs[-1][1] - pairs[0][1]
    return {
        "events": len(pairs),
        "recorded_ms": round(rec, 1),
        "achieved_ms": round(got, 1),
        "stretch": round(got / rec, 2) if rec > 0 else None,
        "mean_lag_ms": round(sum(errs) / len(errs), 1),
        "max_lag_ms": round(max(errs), 1),
    }


# ---------------------------------------------------------------------------------------------
# Scoring and our engine
# ---------------------------------------------------------------------------------------------

def norm(w):
    return (w or "").strip().lower().strip(".,!?;:'\"")


def score(rows):
    """rows: dicts with expected, first, top3 -> counts. '-' answers are skipped."""
    n = f = t = 0
    for r in rows:
        exp = norm(r["expected"])
        if not exp or exp == "-":
            continue
        n += 1
        if norm(r["first"]) == exp:
            f += 1
        if exp in [norm(w) for w in r["top3"]]:
            t += 1
    return {"scored": n, "first": f, "top3": t,
            "first_pct": round(100.0 * f / n, 1) if n else None,
            "top3_pct": round(100.0 * t / n, 1) if n else None}


def parse_replay_table(text):
    """swipekb-replay's table -> {index: [guess1, guess2, guess3]} (1-based like the tool)."""
    out = {}
    cols = None
    for line in text.splitlines():
        if line.startswith("#") and "guess1" in line:
            cols = [line.index(h) for h in ("expected", "guess1", "guess2", "guess3", "result")]
            continue
        if cols is None:
            continue
        m = re.match(r"^(\d+)\s", line)
        if not m:
            continue
        idx = int(m.group(1))
        if "(skipped)" in line:
            out[idx] = None
            continue
        g = [line[cols[k]:cols[k + 1]].strip() for k in (1, 2, 3)]
        out[idx] = [w for w in g if w]
    return out


def run_ours(args):
    cmd = [args.replay, "--data", args.data, "--swipes", args.swipes, "--expect", args.expect]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if p.returncode != 0:
        raise Stop("our engine's replay failed: " + p.stderr[-500:])
    return parse_replay_table(p.stdout)


def table(results):
    lines = ["%-3s %-12s | %-12s %-30s | %-12s %-26s" % (
        "#", "expected", "gboard", "gboard strip", "ours", "ours 2-3")]
    for r in results:
        if r["expected"] in ("-", ""):
            lines.append("%-3d %-12s | (skipped)" % (r["index"], "-"))
            continue
        g = r.get("gboard") or {}
        o = r.get("ours") or []
        gm = "*" if norm(g.get("first")) == norm(r["expected"]) else " "
        om = "*" if o and norm(o[0]) == norm(r["expected"]) else " "
        lines.append("%-3d %-12s |%s%-12s %-30s |%s%-12s %-26s" % (
            r["index"], r["expected"], gm, g.get("first") or "(none)",
            " ".join(g.get("strip") or [])[:30], om, o[0] if o else "(none)", " ".join(o[1:3])))
    return "\n".join(lines)


def summarize(results):
    g_rows, o_rows = [], []
    for r in results:
        g = r.get("gboard") or {}
        o = r.get("ours") or []
        g_rows.append({"expected": r["expected"], "first": g.get("first"),
                       "top3": [g.get("first")] + list(g.get("strip") or [])[:3]})
        o_rows.append({"expected": r["expected"], "first": o[0] if o else None, "top3": o[:3]})
    return {"gboard": score(g_rows), "ours": score(o_rows)}


# ---------------------------------------------------------------------------------------------
# Phone access
# ---------------------------------------------------------------------------------------------

def adb(*args, input=None, timeout=60, check=True, binary=False):
    p = subprocess.run([ADB] + list(args), input=input, capture_output=True, timeout=timeout,
                       text=not binary)
    if check and p.returncode != 0:
        raise Stop("adb %s failed: %s" % (" ".join(args[:3]), (p.stderr or "")[-400:]))
    return p.stdout


def sh(cmd, **kw):
    return adb("shell", cmd, **kw)


def device_connected():
    out = subprocess.run([ADB, "devices"], capture_output=True, text=True).stdout
    devs = [l.split() for l in out.splitlines()[1:] if l.strip()]
    ok = [d for d in devs if len(d) > 1 and d[1] == "device"]
    other = [d for d in devs if len(d) > 1 and d[1] != "device"]
    return ok, other


def screenshot(tag):
    os.makedirs(WORK, exist_ok=True)
    path = os.path.join(WORK, "shot-%s-%s.png" % (tag, time.strftime("%H%M%S")))
    with open(path, "wb") as f:
        f.write(adb("exec-out", "screencap", "-p", binary=True))
    return path


def phone_state():
    """Lock state, focused window and whether the keyboard is up, from dumpsys (no touches)."""
    w = sh("dumpsys window", timeout=30)
    im = sh("dumpsys input_method", timeout=30)
    focus = ""
    m = re.search(r"mCurrentFocus=Window\{[^ ]+ [^ ]+ ([^}]+)\}", w)
    if m:
        focus = m.group(1)
    locked = bool(re.search(r"(mDreamingLockscreen=true|isKeyguardShowing=true|"
                            r"mShowingLockscreen=true|KeyguardShowing=true)", w))
    awake = sh("dumpsys power | grep -m1 mWakefulness=", check=False).strip()
    ime_shown = bool(re.search(r"mInputShown=true|isInputViewShown=true|mIsInputViewShown=true",
                               im))
    cur_ime = ""
    m = re.search(r"mCurMethodId=(\S+)", im)
    if m:
        cur_ime = m.group(1)
    return {"focus": focus, "locked": locked, "wake": awake, "ime_shown": ime_shown,
            "ime": cur_ime}


def notes_pkg():
    pk = sh("pm list packages", timeout=30)
    have = [p for p in NOTES_PKGS if ("package:" + p) in pk.split()]
    return have[0] if have else None


def require_front(pkg, need_ime=True):
    """The guard before every touch: notes app focused, Gboard up, phone unlocked."""
    st = phone_state()
    problems = []
    if "Awake" not in st["wake"]:
        problems.append("screen is not on (%s)" % st["wake"])
    if st["locked"]:
        problems.append("lock screen is showing")
    if not st["focus"].startswith(pkg + "/"):
        problems.append("focused window is %r, not the notes app" % st["focus"])
    if need_ime and not st["ime"].startswith(GBOARD_PKG):
        problems.append("current keyboard is %r, not Gboard" % st["ime"])
    if need_ime and not st["ime_shown"]:
        problems.append("keyboard is not showing")
    if problems:
        shot = screenshot("stop")
        raise Stop("; ".join(problems) + " (screenshot: %s)" % shot)
    return st


def ui_dump():
    sh("uiautomator dump %s" % DEVICE_XML, timeout=60)
    xml = adb("exec-out", "cat", DEVICE_XML, timeout=30)
    return xml


_BOUNDS = re.compile(r"\[(-?\d+),(-?\d+)\]\[(-?\d+),(-?\d+)\]")


def iter_nodes(xml):
    try:
        root = ET.fromstring(xml[xml.index("<"):])
    except (ValueError, ET.ParseError) as e:
        raise Stop("could not read the screen layout dump: %s" % e)
    for n in root.iter("node"):
        b = _BOUNDS.match(n.get("bounds", ""))
        rect = tuple(int(v) for v in b.groups()) if b else None
        yield n, rect


def keys_from_dump(xml):
    """Gboard keys as accessibility nodes: content-desc or text of one letter, in Gboard's
    package. Returns 26 key dicts or None."""
    found = {}
    for n, rect in iter_nodes(xml):
        if n.get("package") != GBOARD_PKG or not rect:
            continue
        for label in (n.get("content-desc") or "", n.get("text") or ""):
            lab = label.strip().lower()
            if len(lab) == 1 and "a" <= lab <= "z" and lab not in found:
                x0, y0, x1, y1 = rect
                found[lab] = {"letter": lab, "x": x0, "y": y0, "w": x1 - x0, "h": y1 - y0}
    return list(found.values()) if len(found) == 26 else None


def strip_from_dump(xml, keys):
    """Words on Gboard's suggestion strip (above the letter rows), left to right."""
    if not keys:
        return []
    top = min(k["y"] for k in keys)
    words = []
    for n, rect in iter_nodes(xml):
        if n.get("package") != GBOARD_PKG or not rect:
            continue
        if rect[3] > top + 2:
            continue
        lab = (n.get("text") or n.get("content-desc") or "").strip()
        if re.fullmatch(r"[A-Za-z][A-Za-z'\-]*", lab) and lab not in [w for _, w in words]:
            words.append((rect[0], lab))
    words.sort()
    return [w for _, w in words]


def field_text(xml, pkg):
    """Text of the focused editable field in the notes app ('' if none)."""
    best = None
    for n, rect in iter_nodes(xml):
        if n.get("package") != pkg:
            continue
        if n.get("focused") == "true" and ("Edit" in (n.get("class") or "")
                                           or n.get("editable") == "true" or n.get("text")):
            best = n
    if best is None:
        return None
    return best.get("text") or ""


# --- screenshot calibration (fallback when Gboard's keys are not in the layout dump) ----------

def raw_screencap():
    """`screencap` without -p: a small header then RGBA pixels. Avoids decoding PNG in Python."""
    data = adb("exec-out", "screencap", binary=True, timeout=60)
    w, h, fmt = struct.unpack("<III", data[:12])
    hdr = len(data) - w * h * 4
    if hdr not in (12, 16) or fmt not in (1, 2):
        raise Stop("unexpected screencap format (w=%d h=%d fmt=%d hdr=%d)" % (w, h, fmt, hdr))
    return w, h, data[hdr:]


def ime_frame():
    w = sh("dumpsys window windows", timeout=30)
    i = w.find("InputMethod")
    while i >= 0:
        seg = w[i:i + 4000]
        m = re.search(r"(?:mFrame=|frame=)\[(\d+),(\d+)\]\[(\d+),(\d+)\]", seg) or \
            re.search(r"mFrame=Rect\((\d+), (\d+) - (\d+), (\d+)\)", seg)
        if m:
            r = tuple(int(v) for v in m.groups())
            if r[3] > r[1] and r[2] > r[0]:
                return r
        i = w.find("InputMethod", i + 1)
    return None


def _runs(flags, min_len):
    runs, start = [], None
    for i, f in enumerate(flags + [False]):
        if f and start is None:
            start = i
        elif not f and start is not None:
            if i - start >= min_len:
                runs.append((start, i))
            start = None
    return runs


def keys_from_pixels(w, h, px, frame, diff=24):
    """Find key caps as blocks that differ from the keyboard background. Bands of rows mostly
    covered by keys are key rows; the band at the bottom is the space-bar row, the three above it
    are the letters (10, 9, then shift + 7 letters + backspace). Returns 26 keys or raises Stop."""
    x0, y0, x1, y1 = frame
    stride = w * 4

    def pix(x, y):
        o = y * stride + x * 4
        return px[o], px[o + 1], px[o + 2]

    # WHY the edge columns: keys can cover most of the keyboard, so the most common colour overall
    # may be the key colour; the outermost pixel columns sit in the margin outside the keys.
    counts = {}
    for y in range(y0, y1):
        for x in (x0, x0 + 1, x1 - 2, x1 - 1):
            c = pix(x, y)
            counts[c] = counts.get(c, 0) + 1
    bg = max(counts, key=counts.get)

    def is_key(x, y):
        r, g, b = pix(x, y)
        return abs(r - bg[0]) + abs(g - bg[1]) + abs(b - bg[2]) > diff

    width = x1 - x0
    rowcov = []
    for y in range(y0, y1):
        hits = sum(1 for x in range(x0, x1, 3) if is_key(x, y))
        rowcov.append(hits / float(len(range(x0, x1, 3))) > 0.45)
    bands = [(a + y0, b + y0) for a, b in _runs(rowcov, 12)]
    if len(bands) < 4:
        raise Stop("screenshot calibration found %d key rows, need >= 4" % len(bands))
    letter_bands = bands[-4:-1]
    want = [10, 9, 9]
    keys = []
    for (ya, yb), row, n in zip(letter_bands, ROWS, want):
        yc = (ya + yb) // 2
        colflags = [is_key(x, yc) or is_key(x, (ya + yc) // 2) for x in range(x0, x1)]
        segs = [(a + x0, b + x0) for a, b in _runs(colflags, max(6, width // 60))]
        if len(segs) != n:
            raise Stop("screenshot calibration: row %r has %d keys, expected %d" % (row, len(segs), n))
        if n == 9 and row == "zxcvbnm":
            segs = segs[1:-1]
        for c, (a, b) in zip(row, segs):
            keys.append({"letter": c, "x": a, "y": ya, "w": b - a, "h": yb - ya})
    return keys


# ---------------------------------------------------------------------------------------------
# Typing helpers
# ---------------------------------------------------------------------------------------------

def tap(x, y):
    sh("input tap %d %d" % (x, y))


def clear_field(pkg, hint):
    """Delete all text: cursor to end, then backspace a few more times than there are letters."""
    for _ in range(4):
        txt = field_text(ui_dump(), pkg)
        if txt is None:
            raise Stop("no focused text field in the notes app")
        if txt == "" or txt == hint:
            return
        n = min(len(txt) + 3, 400)
        sh("input keyevent 123 " + " ".join(["67"] * n))
    raise Stop("could not clear the note field")


def read_typed(pkg, hint, before, keys):
    xml = ui_dump()
    txt = field_text(xml, pkg)
    if txt is None:
        raise Stop("no focused text field after the swipe")
    if txt == hint:
        txt = ""
    typed = txt[len(before):] if before and txt.startswith(before) else txt
    return txt, typed.strip(), strip_from_dump(xml, keys)


class MonkeyInjector:
    """Optional faster path: Android's own `monkey --port` test server injects each touch in
    ~1 ms, so the recorded timing can be followed closely. Started and stopped by this tool."""

    def __init__(self, port=11080):
        self.port = port
        self.proc = subprocess.Popen([ADB, "shell", "monkey", "--port", str(port)],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        adb("forward", "tcp:%d" % port, "tcp:%d" % port)
        self.sock = None
        for _ in range(50):
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=2)
                self._cmd("wake")
                break
            except OSError:
                self.sock = None
                time.sleep(0.2)
        if self.sock is None:
            self.close()
            raise Stop("could not start the monkey touch server")

    def _cmd(self, c):
        self.sock.sendall((c + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = self.sock.recv(64)
            if not chunk:
                raise OSError("monkey closed")
            buf += chunk
        if not buf.startswith(b"OK"):
            raise Stop("monkey refused %r: %r" % (c, buf))

    def swipe(self, pts):
        t0 = pts[0][0]
        start = time.perf_counter()
        pairs = []
        n = len(pts)
        for i, (t, x, y) in enumerate(pts):
            target = (t - t0) / 1000.0
            d = target - (time.perf_counter() - start)
            if d > 0:
                time.sleep(d)
            pairs.append((target * 1000, (time.perf_counter() - start) * 1000))
            act = "down" if i == 0 else ("up" if i == n - 1 else "move")
            self._cmd("touch %s %d %d" % (act, x, y))
        if n == 1:
            self._cmd("touch up %d %d" % (pts[0][1], pts[0][2]))
        return pairs

    def close(self):
        try:
            if self.sock:
                self.sock.sendall(b"quit\n")
                self.sock.close()
        except OSError:
            pass
        adb("forward", "--remove", "tcp:%d" % self.port, check=False)
        pid = sh("pidof com.android.commands.monkey", check=False).strip()
        for p in pid.split():
            if p.isdigit():
                sh("kill %s" % p, check=False)
        self.proc.terminate()


def inject_input(pts):
    out = adb("shell", "sh", input=motion_script(pts), timeout=120)
    return parse_timing(out)


# ---------------------------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------------------------

def need_phone():
    ok, other = device_connected()
    if other:
        raise Stop("phone visible but not usable: %s (unlock it and allow USB debugging)" % other)
    if not ok:
        raise Stop("NEEDS_PHONE: no phone on adb")
    if len(ok) > 1:
        raise Stop("more than one adb device; set ANDROID_SERIAL")


def keys_path():
    return os.path.join(WORK, "phone-keys.json")


def load_phone_keys():
    try:
        with open(keys_path()) as f:
            d = json.load(f)
    except FileNotFoundError:
        raise Stop("no phone-keys.json yet: run `geometry` first")
    return d


def cmd_check(a):
    need_phone()
    pkg = notes_pkg()
    st = phone_state()
    shot = screenshot("check")
    print(json.dumps({"notes_app": pkg, **st, "screenshot": shot}, indent=1))
    print("model:", sh("getprop ro.product.model").strip(),
          "| gboard:", (re.search(r"versionName=(\S+)", sh("dumpsys package %s" % GBOARD_PKG))
                        or [None, "?"])[1])


def cmd_open_note(a):
    need_phone()
    pkg = notes_pkg()
    if not pkg:
        raise Stop("neither Samsung Notes nor Google Keep is installed")
    st = phone_state()
    if st["locked"]:
        raise Stop("phone is locked")
    act = sh("cmd package resolve-activity --brief -c android.intent.category.LAUNCHER %s" % pkg
             ).strip().splitlines()[-1]
    sh("am start -n %s" % act)
    time.sleep(2)
    print("launched", act, "screenshot:", screenshot("opened"))
    print("Next: create a NEW blank note by hand or with a reviewed tap, focus its body, then `check`.")


def cmd_geometry(a):
    need_phone()
    pkg = notes_pkg()
    require_front(pkg)
    shot = screenshot("geometry")
    keys = keys_from_dump(ui_dump())
    source = "uiautomator"
    if not keys:
        frame = ime_frame()
        w, h, px = raw_screencap()
        if not frame:
            frame = (0, h // 2, w, h)
        keys = keys_from_pixels(w, h, px, frame)
        source = "screenshot %s frame=%s" % (shot, frame)
    Grid(keys)  # sanity: 26 letters, rows in order
    with open(keys_path(), "w") as f:
        json.dump({"source": source, "keys": keys, "screenshot": shot}, f, indent=1)
    print("keys from", source)
    for row in ROWS:
        print("  " + "  ".join("%s(%d,%d %dx%d)" % (k["letter"], k["x"], k["y"], k["w"], k["h"])
                               for c in row for k in keys if k["letter"] == c))
    print("saved", keys_path())


def cmd_verify(a):
    need_phone()
    pkg = notes_pkg()
    d = load_phone_keys()
    g = Grid(d["keys"])
    require_front(pkg)
    hint_probe = field_text(ui_dump(), pkg)
    if hint_probe not in ("", None) and not a.hint_ok:
        raise Stop("note field is not empty (%r); pass --hint-ok if that is the empty-note hint"
                   % hint_probe)
    hint = hint_probe or ""
    word = a.letters
    for c in word:
        x, y = g.center(c)
        tap(int(x), int(y))
        time.sleep(0.4)
    txt = field_text(ui_dump(), pkg) or ""
    ok = norm(txt) == word
    print("tapped %r, field reads %r -> %s" % (word, txt, "OK" if ok else "MISMATCH"))
    clear_field(pkg, hint)
    with open(os.path.join(WORK, "hint.json"), "w") as f:
        json.dump({"hint": hint}, f)
    if not ok:
        raise Stop("tap verification failed; geometry is wrong")


def cmd_clear(a):
    need_phone()
    pkg = notes_pkg()
    require_front(pkg, need_ime=False)
    hint = ""
    try:
        hint = json.load(open(os.path.join(WORK, "hint.json")))["hint"]
    except (OSError, ValueError, KeyError):
        pass
    clear_field(pkg, hint)
    print("cleared")


def cmd_plan(a):
    swipes = load_swipes(a.swipes)
    if a.keys:
        keys = json.load(open(a.keys))
        keys = keys.get("keys", keys) if isinstance(keys, dict) else keys
    else:
        keys = synthetic_phone_keys()
    g = Grid(keys)
    s = swipes[a.index - 1]
    pts = simplify(map_swipe(s, g), a.simplify)
    print("# swipe %d: %d recorded points -> %d events, %d ms" % (
        a.index, len(s["points"]), len(pts), pts[-1][0] - pts[0][0]))
    sys.stdout.write(motion_script(pts))


def synthetic_phone_keys(left=0, top=1500, width=1080, row_h=150):
    """A plain Gboard-like phone grid (10/9/7 with the usual offsets) for offline use."""
    kw = width / 10.0
    keys = []
    for r, (row, off) in enumerate(zip(ROWS, (0.0, 0.5, 1.5))):
        for i, c in enumerate(row):
            keys.append({"letter": c, "x": left + (off + i) * kw, "y": top + r * row_h,
                         "w": kw, "h": row_h})
    return keys


def cmd_run(a):
    need_phone()
    pkg = notes_pkg()
    d = load_phone_keys()
    g = Grid(d["keys"])
    hint = ""
    try:
        hint = json.load(open(os.path.join(WORK, "hint.json")))["hint"]
    except (OSError, ValueError, KeyError):
        raise Stop("run `verify` first (it checks the taps and learns the empty-field hint)")
    swipes = load_swipes(a.swipes)
    expect = load_expect(a.expect)
    ours = run_ours(a) if not a.no_ours else {}
    n = min(len(swipes), len(expect)) if not a.all else len(swipes)
    if a.limit:
        n = min(n, a.limit)
    require_front(pkg)
    print("screenshot before first touch:", screenshot("run-start"))
    clear_field(pkg, hint)
    monkey = MonkeyInjector() if a.backend == "monkey" else None
    results = []
    before = ""
    try:
        for i in range(n):
            exp = expect[i] if i < len(expect) else ""
            if exp == "-" and not a.all:
                results.append({"index": i + 1, "expected": "-"})
                continue
            require_front(pkg)
            tol = a.simplify if a.simplify is not None else (
                0.0 if monkey else 0.12 * (g.row_x[0][1] - g.row_x[0][0]))
            pts = simplify(map_swipe(swipes[i], g), tol)
            pairs = monkey.swipe(pts) if monkey else inject_input(pts)
            time.sleep(a.settle)
            full, typed, strip = read_typed(pkg, hint, before, d["keys"])
            r = {"index": i + 1, "expected": exp,
                 "gboard": {"first": typed, "strip": strip},
                 "ours": ours.get(i + 1) or [],
                 "events": len(pts), "timing": timing_stats(pairs)}
            results.append(r)
            print("%-3d %-12s gboard=%-12s strip=%s  timing=%s" % (
                i + 1, exp, typed, strip, r["timing"]), flush=True)
            if a.sentence:
                before = full
            else:
                clear_field(pkg, hint)
    finally:
        if monkey:
            monkey.close()
    if a.sentence:
        clear_field(pkg, hint)
    summ = summarize(results)
    tims = [r["timing"] for r in results if r.get("timing")]
    if tims:
        summ["timing"] = {
            "swipes": len(tims),
            "median_stretch": sorted(t["stretch"] for t in tims if t["stretch"])[len(tims) // 2],
            "mean_lag_ms": round(sum(t["mean_lag_ms"] for t in tims) / len(tims), 1),
            "worst_lag_ms": max(t["max_lag_ms"] for t in tims),
        }
    out = {"date": datetime.datetime.now().isoformat(timespec="seconds"),
           "mode": "sentence" if a.sentence else "isolated", "backend": a.backend,
           "phone_keys": d.get("source"), "summary": summ, "results": results}
    path = os.path.join(WORK, "results-%s.json" % time.strftime("%Y%m%d-%H%M%S"))
    with open(path, "w") as f:
        json.dump(out, f, indent=1)
    print()
    print(table(results))
    print_summary(summ)
    print("results:", path)


def print_summary(s):
    for who in ("gboard", "ours"):
        x = s[who]
        print("%-7s first guess %d/%d (%s%%)   top 3 %d/%d (%s%%)" % (
            who, x["first"], x["scored"], x["first_pct"], x["top3"], x["scored"], x["top3_pct"]))
    if "timing" in s:
        print("timing:", s["timing"])


def cmd_report(a):
    d = json.load(open(a.results))
    print(table(d["results"]))
    print_summary(d["summary"])


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check")
    sub.add_parser("open-note")
    sub.add_parser("geometry")
    v = sub.add_parser("verify")
    v.add_argument("--letters", default="qgm")
    v.add_argument("--hint-ok", action="store_true")
    sub.add_parser("clear")
    pl = sub.add_parser("plan")
    pl.add_argument("--swipes", default=DEFAULT_SWIPES)
    pl.add_argument("--keys")
    pl.add_argument("--index", type=int, default=1)
    pl.add_argument("--simplify", type=float, default=0.0)
    r = sub.add_parser("run")
    r.add_argument("--swipes", default=DEFAULT_SWIPES)
    r.add_argument("--expect", default=DEFAULT_EXPECT)
    r.add_argument("--replay", default=DEFAULT_REPLAY)
    r.add_argument("--data", default=DEFAULT_DATA)
    r.add_argument("--backend", choices=["input", "monkey"], default="input")
    r.add_argument("--simplify", type=float, default=None,
                   help="px tolerance for dropping in-line points (default: 12%% of a key for "
                        "input, 0 for monkey)")
    r.add_argument("--sentence", action="store_true", help="don't clear between swipes")
    r.add_argument("--limit", type=int)
    r.add_argument("--settle", type=float, default=0.8)
    r.add_argument("--all", action="store_true", help="also swipes with no answer")
    r.add_argument("--no-ours", action="store_true")
    rp = sub.add_parser("report")
    rp.add_argument("results")
    a = p.parse_args(argv)
    os.makedirs(WORK, exist_ok=True)
    os.chmod(WORK, 0o700)
    try:
        {"check": cmd_check, "open-note": cmd_open_note, "geometry": cmd_geometry,
         "verify": cmd_verify, "clear": cmd_clear, "plan": cmd_plan, "run": cmd_run,
         "report": cmd_report}[a.cmd](a)
    except Stop as e:
        print("STOP:", e, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
