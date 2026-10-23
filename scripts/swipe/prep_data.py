"""Turn swipe .jsonl files into one compact .npz the trainer reads (python -I prep_data.py ...).

Each swipe: {"word", "canvas_width", "canvas_height", "data":[{"t","x","y"},...]} (x,y in 0..1 of the letter area).
Output arrays: feats (N,64,8), pos (N,64,2), aspect (N,), labels (flat int16, key index), lens (N,), words (N,).
Swipes whose word has no mappable letters, or more than 30 key changes, are dropped (and counted).
"""
import argparse
import os
import sys
from multiprocessing import Pool

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import swipelib as S  # noqa: E402

LAYOUT = None


def _init(layout_path):
    global LAYOUT
    LAYOUT = S.Layout(layout_path)


def _one(line):
    import json
    r = json.loads(line)
    d = r["data"]
    if not d:
        return None
    keys = LAYOUT.word_keys(r["word"])
    if not keys or len(keys) > 30:
        return None
    aspect = r["canvas_height"] / r["canvas_width"]
    f, p = S.trace_to_input([q["x"] for q in d], [q["y"] for q in d], [q["t"] for q in d], aspect, LAYOUT)
    return f, p, aspect, keys, r["word"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--layout", default=os.path.join(os.path.dirname(__file__), "layouts", "qwerty.tsv"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--procs", type=int, default=os.cpu_count())
    a = ap.parse_args()
    lines = []
    for p in a.inputs:
        with open(p, encoding="utf-8") as f:
            for i, l in enumerate(f):
                if a.limit and i >= a.limit:
                    break
                lines.append(l)
    with Pool(a.procs, initializer=_init, initargs=(a.layout,)) as pool:
        res = pool.map(_one, lines, chunksize=256)
    ok = [r for r in res if r]
    print(f"{len(ok)} kept, {len(res) - len(ok)} dropped")
    np.savez_compressed(
        a.out,
        feats=np.stack([r[0] for r in ok]), pos=np.stack([r[1] for r in ok]),
        aspect=np.array([r[2] for r in ok], np.float32),
        labels=np.concatenate([np.array(r[3], np.int16) for r in ok]),
        lens=np.array([len(r[3]) for r in ok], np.int16),
        words=np.array([r[4] for r in ok]))


if __name__ == "__main__":
    main()
