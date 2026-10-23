"""Convert the public HeliBoard Gesture Data Project into swipe TEST sets (python -I heliboard_to_jsonl.py ...).

LICENCE: the data is CC BY-SA 4.0 and is used here ONLY to test/evaluate. Never train on it, and never commit the
gestures or any per-swipe file derived from them (they are share-alike). Keep every output under
~/.cache/swipe-keyboard-train/heliboard/. Only this script and summary numbers belong in git.

Input : a clone of https://codeberg.org/eclexic/heliboard-gesture-data  (compressed/batch_N/<lang>/<lang>-K.gestures.json.zst)
Output (per language <L>, under --out):
  <L>/test.jsonl, <L>/dev.jsonl     engine format: {id, word, canvas_width, canvas_height, layout, layout_type, locale, data:[{t,x,y}]}
                                    x,y are 0..1 of the LETTER AREA (bounding box of the letter keys); canvas_* are that area in pixels
  layouts/<layout id>.tsv           per-layout key file (letter cx cy half_w half_h, same format as scripts/swipe/layouts/*.tsv)
  bench/<L>-test-NN.json            tools/swipekb-language-bench cases (provenance supplied-human, explicit keys), chunks <=16 MB;
                                    only qwerty / azerty / qwertz boards (the bench refuses other boards)
  stats.json                        counts kept/dropped per reason, layout types, keyboard sizes

Split: sha256(uuid) decides, so it is stable whatever order files are read: lowest hashes -> test (up to --test-n), the
next --dev-cap -> dev. The split is NOT by contributor (the data has no contributor id), so repeated words from one
person can sit on both sides; fine for scoring a model that never saw this data.
"""
import argparse
import glob
import hashlib
import heapq
import json
import os
import sys
import unicodedata
from collections import Counter
from multiprocessing import Pool

import zstandard

# language -> HeliBoard language-code directories to read, and the locale the language bench resolves.
LANGS = {
    "es": (["es"], "es-ES"),
    "fr": (["fr"], "fr-FR"),
    "de": (["de", "de-AT"], "de-DE"),
    "en": (["en-US", "en-GB", "en-AU", "en-CA"], "en-US"),
}
# Letters with no key of their own that people still type through a neighbouring base key (after NFD stripping fails).
EXTRA_FOLD = {"ß": "s", "æ": "a", "œ": "o", "ø": "o", "đ": "d", "ł": "l", "ð": "d", "þ": "t", "ı": "i"}
SIGNATURES = {"qwertyuiopasdfghjklzxcvbnm": "qwerty", "qwertzuiopasdfghjklyxcvbnm": "qwertz",
              "azertyuiopqsdfghjklmwxcvbn": "azerty"}
REASONS = ["null_target", "multi_pointer", "short_word", "non_letter_char", "no_letter_keys", "unreachable_letter",
           "single_key_word", "short_trace", "bad_time"]


def letter_keys(info):
    """Alphabetic single-character keys of one keyboard: {letter: (left, top, width, height)}."""
    keys = {}
    for k in info["keys"]:
        v = k.get("value", "")
        if len(v) == 1 and v.isalpha() and v.lower() not in keys:
            keys[v.lower()] = (k["left"], k["top"], k["width"], k["height"])
    return keys


def classify(keys):
    """Row-by-row ascii letter order -> qwerty / qwertz / azerty / other."""
    items = sorted(((t, l, c) for c, (l, t, w, h) in keys.items() if "a" <= c <= "z"))
    if not items:
        return "other"
    rows, cur, row_top = [], [], items[0][0]
    hs = [h for (_, _, _, h) in keys.values()]
    band = (sorted(hs)[len(hs) // 2] if hs else 1) / 2
    for t, l, c in items:
        if t - row_top > band:
            rows.append(cur)
            cur, row_top = [], t
        cur.append((l, c))
    rows.append(cur)
    sig = "".join(c for row in rows for _, c in sorted(row))
    return SIGNATURES.get(sig, "other")


def fold(ch, keys):
    c = ch.lower()
    if c in keys:
        return c
    base = unicodedata.normalize("NFD", c)[0]
    if base in keys:
        return base
    b = EXTRA_FOLD.get(c)
    return b if b in keys else None


def layout_geometry(keys):
    """Letter-area box (x0, y0, w, h) in keyboard pixels and the keys normalised to it."""
    x0 = min(l for l, t, w, h in keys.values()); y0 = min(t for l, t, w, h in keys.values())
    x1 = max(l + w for l, t, w, h in keys.values()); y1 = max(t + h for l, t, w, h in keys.values())
    W, H = x1 - x0, y1 - y0
    norm = {c: ((l - x0 + w / 2) / W, (t - y0 + h / 2) / H, w / 2 / W, h / 2 / H) for c, (l, t, w, h) in keys.items()}
    return (x0, y0, W, H), norm


def layout_id(ltype, norm):
    # WHY quantise to 0.01: the same board on different phones differs by a pixel or two, which is not a different layout.
    sig = ";".join(f"{c}{round(v[0], 2)},{round(v[1], 2)},{round(v[2], 2)},{round(v[3], 2)}" for c, v in sorted(norm.items()))
    return f"{ltype}-{hashlib.sha256(sig.encode()).hexdigest()[:8]}"


def convert(g, lang, locale, src):
    """One gesture -> (None, reason) or (record dict, bench case dict or None, layout tsv rows)."""
    word = g.get("targetWord")
    if word is None:
        return None, "null_target"
    pts = g["gesture"]
    if len({p["id"] for p in pts}) > 1:
        return None, "multi_pointer"
    if sum(ch.isalpha() for ch in word) < 2:
        return None, "short_word"
    # WHY apostrophes are kept: French l'/d'/qu' words are ~13% of the data and a swipe only crosses the letters (the
    # bench trace rule too); any other non-letter (hyphen, digit, dot) is dropped.
    if not all(ch.isalpha() or ch in "'\u2019" for ch in word):
        return None, "non_letter_char"
    info = g.get("keyboardInfo") or {"keys": []}
    keys = letter_keys(info)
    if len(keys) < 20:
        return None, "no_letter_keys"
    seq = []
    for ch in word:
        if not ch.isalpha():
            continue
        k = fold(ch, keys)
        if k is None:
            return None, "unreachable_letter"
        if not seq or seq[-1] != k:
            seq.append(k)
    if len(seq) < 2:
        return None, "single_key_word"
    if len(pts) < 3:
        return None, "short_trace"
    ts = [p["millis"] for p in pts]
    if any(b < a for a, b in zip(ts, ts[1:])):
        return None, "bad_time"
    (x0, y0, W, H), norm = layout_geometry(keys)
    ltype = classify(keys)
    lid = layout_id(ltype, norm)
    uid = g["uuid"][:12]
    rec = {"id": f"{lang}-{uid}", "word": word, "canvas_width": W, "canvas_height": H, "layout": lid,
           "layout_type": ltype, "locale": src,
           "data": [{"t": p["millis"] - ts[0], "x": round((p["x"] - x0) / W, 4), "y": round((p["y"] - y0) / H, 4)} for p in pts]}
    bench = None
    if ltype != "other":
        # WHY snap tops: the bench orders keys by exact y, and HeliBoard rows can differ by a pixel after rounding.
        rows = {}
        for c, (l, t, w, h) in keys.items():
            rows.setdefault(round(t / max(h, 1) * 2), []).append(t)
        snap = {r: min(v) for r, v in rows.items()}
        bench = {"id": f"{lang}-{uid}", "locale": locale, "layout": ltype, "target": word, "nativeReviewed": False,
                 "provenance": "supplied-human",
                 "keys": [{"letter": c, "x": round(l - x0, 1), "y": round(snap[round(t / max(h, 1) * 2)] - y0, 1),
                           "w": float(w), "h": float(h)} for c, (l, t, w, h) in keys.items()],
                 "points": [{"x": round(p["x"] - x0, 1), "y": round(p["y"] - y0, 1), "t": float(p["millis"] - ts[0])} for p in pts]}
    return (rec, bench, lid, ltype, norm, info.get("width"), info.get("height")), None


def process_file(args):
    path, lang, locale = args
    data = json.loads(zstandard.ZstdDecompressor().stream_reader(open(path, "rb")).read())
    src = os.path.basename(path).split(".")[0]
    drops, out = Counter(), []
    layouts = {}
    for g in data:
        r, why = convert(g, lang, locale, src)
        if why:
            drops[why] += 1
            continue
        rec, bench, lid, ltype, norm, kw, kh = r
        layouts[lid] = norm
        h = int(hashlib.sha256(g["uuid"].encode()).hexdigest()[:12], 16)
        out.append((h, g["uuid"], json.dumps(rec, separators=(",", ":")),
                    json.dumps(bench, separators=(",", ":")) if bench else None, lid, ltype, (kw, kh)))
    return len(data), drops, out, layouts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=os.path.expanduser("~/.cache/swipe-keyboard-train/heliboard/repo"))
    ap.add_argument("--out", default=os.path.expanduser("~/.cache/swipe-keyboard-train/heliboard/out"))
    ap.add_argument("--langs", nargs="+", default=list(LANGS))
    ap.add_argument("--test-n", type=int, default=5000)
    ap.add_argument("--dev-cap", type=int, default=20000)
    ap.add_argument("--chunk", type=int, default=1500, help="bench cases per json file (the bench refuses >16 MB)")
    ap.add_argument("--procs", type=int, default=min(8, os.cpu_count() or 1))
    a = ap.parse_args()
    if "heliboard" not in os.path.abspath(a.out) or os.path.abspath(a.out).startswith(os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))):
        sys.exit("refusing: --out must be outside the git checkout (share-alike data stays in ~/.cache/.../heliboard)")
    os.makedirs(os.path.join(a.out, "layouts"), exist_ok=True)
    os.makedirs(os.path.join(a.out, "bench"), exist_ok=True)
    stats = {}
    all_layouts = {}
    for lang in a.langs:
        dirs, locale = LANGS[lang]
        files = sorted(f for d in dirs for f in glob.glob(os.path.join(a.repo, "compressed", "batch_*", d, "*.zst")))
        total, drops, heap, seen = 0, Counter(), [], set()
        ltypes, kbsizes, lids, dup = Counter(), Counter(), Counter(), 0
        keep = a.test_n + a.dev_cap
        with Pool(a.procs) as pool:
            for n, d, out, lay in pool.imap_unordered(process_file, [(f, lang, locale) for f in files]):
                total += n; drops.update(d); all_layouts.update(lay)
                for h, uuid, rec, bench, lid, ltype, kb in out:
                    if uuid in seen:
                        dup += 1
                        continue
                    seen.add(uuid)
                    ltypes[ltype] += 1; lids[lid] += 1; kbsizes[f"{kb[0]}x{kb[1]}"] += 1
                    # max-heap on hash (negated) keeps the `keep` lowest hashes whatever the read order
                    item = (-h, uuid, rec, bench)
                    if len(heap) < keep:
                        heapq.heappush(heap, item)
                    elif item > heap[0]:
                        heapq.heapreplace(heap, item)
        rows = sorted(heap, key=lambda t: -t[0])  # ascending hash
        rows = [(-h, u, r, b) for h, u, r, b in [(x[0], x[1], x[2], x[3]) for x in rows]]
        nt = min(a.test_n, len(rows) // 2)
        test, dev = rows[:nt], rows[nt:]
        os.makedirs(os.path.join(a.out, lang), exist_ok=True)
        for name, part in (("test", test), ("dev", dev)):
            with open(os.path.join(a.out, lang, f"{name}.jsonl"), "w", encoding="utf-8") as f:
                for _, _, rec, _ in part:
                    f.write(rec + "\n")
        for old in glob.glob(os.path.join(a.out, "bench", f"{lang}-test-*.json")):
            os.remove(old)
        bcases = [b for _, _, _, b in test if b]
        for i in range(0, len(bcases), a.chunk):
            with open(os.path.join(a.out, "bench", f"{lang}-test-{i // a.chunk:02d}.json"), "w", encoding="utf-8") as f:
                f.write('{"schema":1,"cases":[' + ",".join(bcases[i:i + a.chunk]) + "]}")
        kept = sum(ltypes.values())
        stats[lang] = {"files": len(files), "gestures_read": total, "dropped": {r: drops.get(r, 0) for r in REASONS},
                       "duplicate_uuid": dup, "kept_all": kept, "test": len(test), "dev": len(dev),
                       "bench_cases": len(bcases), "layout_types_kept": dict(ltypes),
                       "top_layout_ids": lids.most_common(6), "distinct_layout_ids": len(lids),
                       "top_keyboard_px": kbsizes.most_common(5)}
        print(lang, json.dumps(stats[lang]), flush=True)
    for lid, norm in all_layouts.items():
        with open(os.path.join(a.out, "layouts", f"{lid}.tsv"), "w", encoding="utf-8") as f:
            f.write("letter\tcx\tcy\thalf_w\thalf_h\n")
            for c, v in sorted(norm.items()):
                f.write(c + "\t" + "\t".join(f"{x:.4f}" for x in v) + "\n")
    with open(os.path.join(a.out, "stats.json"), "w") as f:
        json.dump(stats, f, indent=1)


if __name__ == "__main__":
    main()
