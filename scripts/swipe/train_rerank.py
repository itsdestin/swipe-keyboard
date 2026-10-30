#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""scripts/swipe/train_rerank.py — trains the learned re-ranker of a swipe's found words (joint-model step 3,
engine/include/swipekb/swipe_rerank.hpp) on swipes the shipped swipe model T1 never trained on.

Steps (each writes into --work, a folder outside the repository; data never goes into git):
  split   English training pool = FUTO `trv8k` (held back from T1) + the `dev.jsonl` sessions T1 held out for its
          validation (`devs-val.jsonl`; T1 trained on the other 90 % of dev sessions, docs/models/swipe-typing/README.md).
          Split by SESSION (sha1 of the session id), never by swipe: 75 % train, 25 % validation.
          French / German (FUTO swipe-5 validation slices, 2 and 5 sessions) and English-on-QWERTZ (8 sessions) are
          validation only (too few sessions to split; they were never trained on by T1 either).
  dump    replays every file through build/tools/swipekb-rescore-bench --features (the engine's own feature code), once
          with the words before the swipe and once without (no sentence: as the first word of a message / HeliBoard).
  train   fits the "sentence" and "no sentence" weight sets (listwise softmax: the right word against the other found
          words of the same swipe), a small grid of L2 strengths and hidden sizes (0 = linear, 8, 16); the model is
          chosen on the validation part only (right first guesses), preferring the simplest within one swipe in 1000.
  export  writes the chosen model as the C++ include compiled into the engine (engine/src/swipe_rerank_weights.inc).

Test sets (bench-2000, test-extra-10000, HeliBoard) are never read here: this script refuses any input but the files
above (WHY: they are scored once, at the end, with the real tools).

Usage: train_rerank.py split|dump|train|export [--work DIR] [--bench PATH] [--assets DIR] [-j N] ...
Run with ~/.cache/swipe-keyboard-train/venv (CPU torch).
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import math
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TRAIN_CACHE = os.path.expanduser("~/.cache/swipe-keyboard-train")
FUTO = f"{TRAIN_CACHE}/shape/futo"
SOURCES = {  # name: (file, language, layout, match)
    "trv8k": (f"{FUTO}/trv8k.jsonl", "en", f"{ROOT}/scripts/swipe/layouts/qwerty.tsv", "ascii"),
    "devs-val": (f"{TRAIN_CACHE}/shape/devs-val.jsonl", "en", f"{ROOT}/scripts/swipe/layouts/qwerty.tsv", "ascii"),
    "fr-val": (f"{FUTO}/s5-azerty-val.jsonl", "fr", f"{FUTO}/layouts/azerty.tsv", "lookup"),
    "de-val": (f"{FUTO}/s5-german-val.jsonl", "de", f"{FUTO}/layouts/german.tsv", "lookup"),
    "enqz-val": (f"{FUTO}/s5-qwertz-val.jsonl", "en", f"{FUTO}/layouts/qwertz.tsv", "ascii"),
}
VAL_SHARE = 25  # percent of English sessions held out for validation
# The engine's RerankFeature order (engine/include/swipekb/swipe_rerank.hpp).
FEATURES = ["score", "path", "prior", "geo", "neural", "first", "rank_log", "freq", "letters", "length_fit", "start",
            "end", "apostrophe", "first_gap", "path_rank", "neural_rank", "swipe_length"]
NEURAL_FEATURES = {"neural", "neural_rank"}


def is_val(session):
    return int(hashlib.sha1(session.encode()).hexdigest(), 16) % 100 < VAL_SHARE


def cmd_split(a):
    os.makedirs(a.work, exist_ok=True)
    out = {"en-train": open(f"{a.work}/en-train.jsonl", "w"), "en-val": open(f"{a.work}/en-val.jsonl", "w")}
    counts = {k: [0, set()] for k in out}
    for name in ("trv8k", "devs-val"):
        with open(SOURCES[name][0]) as f:
            for line in f:
                s = json.loads(line)["session"]
                k = "en-val" if is_val(s) else "en-train"
                out[k].write(line)
                counts[k][0] += 1
                counts[k][1].add(s)
    for k, f in out.items():
        f.close()
        print(f"{k}: {counts[k][0]} swipes, {len(counts[k][1])} sessions")
    assert not counts["en-train"][1] & counts["en-val"][1]


def inputs(work):
    """name: (swipes file, language, layout, match) for every file that is dumped."""
    qwerty = f"{ROOT}/scripts/swipe/layouts/qwerty.tsv"
    d = {"en-train": (f"{work}/en-train.jsonl", "en", qwerty, "ascii"), "en-val": (f"{work}/en-val.jsonl", "en", qwerty, "ascii")}
    for k in ("fr-val", "de-val", "enqz-val"):
        d[k] = SOURCES[k]
    return d


def cmd_dump(a):
    lang_files = os.path.join(a.assets, "language-files")
    jobs = []
    for name, (path, lang, layout, match) in inputs(a.work).items():
        for ctx in (True, False):
            out = f"{a.work}/feat-{name}-{'ctx' if ctx else 'noctx'}.jsonl"
            if os.path.exists(out) and not a.force:
                continue
            d = f"{lang_files}/{'en_swipe' if lang == 'en' else lang}.combined"
            cmd = ["nice", "-n", "10", a.bench, "--dict", d, "--model", f"{lang_files}/swipe-model.bin",
                   "--ngram", f"{lang_files}/{lang}.ngram", "--vocab", f"{lang_files}/{lang}.vocab.tsv",
                   "--nwp", f"{lang_files}/{lang}.nwp.bin", "--language", lang, "--layout", layout, "--match", match,
                   "--features", out, path]
            if ctx:
                cmd.insert(-1, "--context")
            jobs.append((out, cmd))

    def run(job):
        out, cmd = job
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(f"{out}: {r.stderr[-500:]}")
        return out, r.stdout.strip().splitlines()[0]

    with cf.ThreadPoolExecutor(a.j) as ex:
        for out, first in ex.map(run, jobs):
            print(os.path.basename(out), first)


def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            r = json.loads(line)
            rows.append((r["before"], r["right"], [c[0] for c in r["c"]], [c[1:] for c in r["c"]]))
    return rows


def tensors(rows, use, torch):
    """Padded (swipes, 30, features) tensor, mask, target index (-1 = right word not in the list)."""
    n = len(rows)
    P = 30
    X = torch.zeros(n, P, len(use))
    M = torch.zeros(n, P, dtype=torch.bool)
    T = torch.full((n,), -1, dtype=torch.long)
    for i, (_, _, labels, feats) in enumerate(rows):
        for k, (lab, f) in enumerate(zip(labels[:P], feats[:P])):
            X[i, k] = torch.tensor([f[j] for j in use])
            M[i, k] = True
            if lab and T[i] < 0:
                T[i] = k
    return X, M, T


class Net:
    """s = lin . z + out . relu(W z + b), z = (x - mean) / scale: the engine's SwipeReranker::score."""

    def __init__(self, torch, nf, hidden, mean, scale):
        self.torch, self.hidden = torch, hidden
        self.mean, self.scale = mean, scale
        g = torch.Generator().manual_seed(1)
        self.lin = torch.zeros(nf, requires_grad=True)
        self.params = [self.lin]
        if hidden:
            self.w1 = (torch.randn(hidden, nf, generator=g) * 0.3).requires_grad_()
            self.b1 = torch.zeros(hidden, requires_grad=True)
            self.out = (torch.randn(hidden, generator=g) * 0.1).requires_grad_()
            self.params += [self.w1, self.b1, self.out]

    def __call__(self, X):
        z = (X - self.mean) / self.scale
        s = z @ self.lin
        if self.hidden:
            s = s + self.torch.relu(z @ self.w1.T + self.b1) @ self.out
        return s


def fit(torch, X, M, T, hidden, l2, mean, scale, epochs=300):
    net = Net(torch, X.shape[2], hidden, mean, scale)
    keep = T >= 0
    Xk, Mk, Tk = X[keep], M[keep], T[keep]
    # WHY full-batch L-BFGS: a few thousand lists of 30 words fit in memory, and the result is deterministic.
    opt = torch.optim.LBFGS(net.params, lr=0.5, max_iter=epochs, history_size=20, line_search_fn="strong_wolfe")

    def closure():
        opt.zero_grad()
        s = net(Xk).masked_fill(~Mk, -1e9)
        loss = torch.nn.functional.cross_entropy(s, Tk)
        # WHY no penalty on the hidden biases: they only shift where a unit switches on.
        loss = loss + l2 * sum((p * p).sum() for p in net.params if not (hidden and p is net.b1))
        loss.backward()
        return loss

    opt.step(closure)
    return net


def grade(torch, net, X, M, T):
    """(swipes, first right, top-3 of the list right) with this model; T = -1 swipes count as wrong."""
    with torch.no_grad():
        s = net(X).masked_fill(~M, -1e9) if net else torch.where(M, -torch.arange(M.shape[1]).float().expand_as(M), torch.tensor(-1e9))
        s = s.detach()
        order = torch.argsort(s, dim=1, descending=True, stable=True)
    has = T >= 0
    right = (order[:, 0] == T) & has
    top3 = ((order[:, :3] == T.unsqueeze(1)).any(1) & has).sum().item()
    return len(T), right.sum().item(), top3, right


def fixed_baseline(rows, english):
    """The ContextRescore fixed weights (steps 1-2) replayed on the features, swipes with words before only (others keep
    the search's order). A word with no neural score has its n-gram prior in the feature's place, as in the engine."""
    wn, wg = 1.0, (0.0 if english else 0.25)
    first = top3 = 0
    for before, _, labels, feats in rows:
        if not labels:
            continue
        if before:
            sc = []
            for k, f in enumerate(feats):
                d = dict(zip(FEATURES, f))
                sc.append((-(d["score"] + (wg - 1) * d["prior"] + wn * d["neural"]), k))
            order = [k for _, k in sorted(sc)]
        else:
            order = list(range(len(labels)))
        first += labels[order[0]]
        top3 += any(labels[k] for k in order[:3])
    return len(rows), first, top3


def cmd_train(a):
    import torch
    torch.set_num_threads(a.threads)
    torch.manual_seed(1)
    results = {}
    chosen = {}
    for setname, tag, use_names in (("sentence", "ctx", FEATURES), ("nosentence", "noctx", [f for f in FEATURES if f not in NEURAL_FEATURES])):
        use = [FEATURES.index(f) for f in use_names]
        tr = load(f"{a.work}/feat-en-train-{tag}.jsonl")
        if tag == "ctx":
            # WHY only swipes with words before: the "sentence" weights only ever see those (the rest take "no sentence").
            tr = [r for r in tr if r[0]]
        vals = {v: load(f"{a.work}/feat-{v}-{tag}.jsonl") for v in ("en-val", "fr-val", "de-val", "enqz-val")}
        if tag == "ctx":
            vals = {k: [r for r in v if r[0]] for k, v in vals.items()}
        X, M, T = tensors(tr, use, torch)
        # WHY: a feature that never varies in training cannot be weighed; it is left out (weight 0 in the file) rather
        # than given a scale that would blow up the first time it does vary in the keyboard.
        flat = X[M]
        varying = [j for j in range(len(use)) if float(flat[:, j].std()) > 1e-6]
        if len(varying) < len(use):
            print("   left out (constant in training): " + ", ".join(FEATURES[use[j]] for j in range(len(use)) if j not in varying))
            use = [use[j] for j in varying]
            X, M, T = tensors(tr, use, torch)
        flat = X[M]
        mean = flat.mean(0)
        scale = flat.std(0).clamp_min(1e-3)
        VT = {k: tensors(v, use, torch) for k, v in vals.items()}
        print(f"== {setname}: train {len(tr)} swipes ({int((T >= 0).sum())} with the right word in the list); "
              + ", ".join(f"{k} {len(v)}" for k, v in vals.items()))
        line = lambda g: f"{g[1]:5d} ({100 * g[1] / max(1, g[0]):.2f}%) top3 {100 * g[2] / max(1, g[0]):.2f}%"
        base = {k: grade(torch, None, *VT[k]) for k in VT}
        fixed = {k: fixed_baseline(vals[k], k.startswith("en")) for k in VT}
        print("   search order (0.5.0):   " + " | ".join(f"{k} {line(base[k])}" for k in VT))
        if tag == "ctx":
            print("   fixed weights (step 2): " + " | ".join(f"{k} {line(fixed[k])}" for k in VT))
        grid = []
        for hidden in a.hidden:
            for l2 in a.l2:
                net = fit(torch, X, M, T, hidden, l2, mean, scale)
                g = {k: grade(torch, net, *VT[k]) for k in VT}
                grid.append((hidden, l2, net, g))
                print(f"   hidden {hidden:2d} l2 {l2:<7g}  " + " | ".join(f"{k} {line(g[k])}" for k in VT), flush=True)
        # WHY chosen on English validation first guesses only, the simplest within 0.1 %: French/German validation is
        # too small to choose by (it is reported, and a model that loses there is not shipped for them).
        best = max(g["en-val"][1] for _, _, _, g in grid)
        tol = max(1, int(0.001 * grid[0][3]["en-val"][0]))
        pick = next(x for x in sorted(grid, key=lambda x: (x[0], -x[1])) if x[3]["en-val"][1] >= best - tol)
        print(f"   chosen: hidden {pick[0]} l2 {pick[1]}")
        # WHY paired counts: how many swipes the chosen model gets right that the search order / the best linear model
        # gets wrong and the other way round (the honest size of a difference on a few thousand swipes).
        lin = max((x for x in grid if x[0] == 0), key=lambda x: x[3]["en-val"][1])
        for k in VT:
            r = pick[3][k][3]
            vs = lambda o: f"+{int((r & ~o).sum())} -{int((~r & o).sum())}"
            print(f"   {k}: chosen vs search order {vs(base[k][3])}, vs linear (l2 {lin[1]}) {vs(lin[3][k][3])}")
        chosen[setname] = (pick[2], use)
        results[setname] = {"hidden": pick[0], "l2": pick[1],
                            "val": {k: {"swipes": base[k][0], "search_first": base[k][1], "search_top3": base[k][2],
                                        "fixed_first": fixed[k][1], "fixed_top3": fixed[k][2],
                                        "model_first": pick[3][k][1], "model_top3": pick[3][k][2]} for k in VT}}
    path = f"{a.work}/rerank.txt"
    with open(path, "w") as f:
        f.write(model_text(chosen))
    with open(f"{a.work}/train-result.json", "w") as f:
        json.dump(results, f, indent=1)
    print(f"wrote {path}")


def model_text(chosen):
    """The SwipeReranker text format; features a set does not use get weight 0 (mean 0, scale 1)."""
    nf = len(FEATURES)
    out = [f"swipekb-rerank 1", f"features {nf}"]
    for setname, (net, use) in chosen.items():
        mean, scale, lin = [0.0] * nf, [1.0] * nf, [0.0] * nf
        for j, i in enumerate(use):
            mean[i], scale[i], lin[i] = float(net.mean[j]), float(net.scale[j]), float(net.lin[j].detach())
        out.append(f"set {setname} hidden {net.hidden}")
        fmt = lambda v: " ".join(f"{x:.6g}" for x in v)
        out += [f"mean {fmt(mean)}", f"scale {fmt(scale)}", f"lin {fmt(lin)}"]
        if net.hidden:
            w1 = [[0.0] * nf for _ in range(net.hidden)]
            for h in range(net.hidden):
                for j, i in enumerate(use):
                    w1[h][i] = float(net.w1[h, j].detach())
            out += [f"w1 {fmt([x for r in w1 for x in r])}", f"b1 {fmt(net.b1.detach().tolist())}",
                    f"out {fmt(net.out.detach().tolist())}"]
    out.append("end")
    return "\n".join(out) + "\n"


def cmd_export(a):
    text = open(a.model).read()
    # WHY FUTO's line and the credit: the numbers are learned from FUTO's swipe data (MIT, Copyright (c) 2025 FUTO), whose
    # notice must travel with anything trained on it (docs/models/SOURCES.md, "Learned swipe re-ranker").
    # REUSE-IgnoreStart (these are the header lines written into the .inc, not this script's own licence)
    lines = ["// SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>",
             "// SPDX-FileCopyrightText: 2025 FUTO (training data: swipe.futo.org swipes, MIT)", "//",
             "// SPDX-License-Identifier: MIT", "",
             "// Built-in SwipeReranker weights (swipe_rerank.cpp includes this inside an unnamed namespace).",
             "// Models trained and released by Destin Moss via youcoded.ai, 2026.",
             "// Trained on FUTO's swipe.futo.org swipes (MIT, Copyright (c) 2025 FUTO); keep FUTO's MIT notice with these weights",
             "// (FUTO-SWIPE-DATA-LICENSE.txt, installed in the licence folder). Provenance: docs/models/SOURCES.md.",
             "// Written by scripts/swipe/train_rerank.py export; see that script and swipe_rerank.hpp for how they were made.",
             # REUSE-IgnoreEnd
             f"// {a.note}" if a.note else "//",
             "const std::vector<std::pair<std::string, std::string>> kBuiltInRerankers = {",
             f'    {{"{a.lang}", R"SWKBRR({text})SWKBRR"}},', "};", ""]
    with open(a.inc, "w") as f:
        f.write("\n".join(lines))
    print(f"wrote {a.inc} ({len(text)} bytes of weights)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("step", choices=["split", "dump", "train", "export"])
    ap.add_argument("--work", default=f"{TRAIN_CACHE}/rerank")
    ap.add_argument("--bench", default=f"{ROOT}/build/tools/swipekb-rescore-bench")
    ap.add_argument("--assets", default=os.environ.get("SWIPEKB_ASSETS", f"{TRAIN_CACHE}/one-brain-assets"))
    ap.add_argument("-j", type=int, default=4)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--hidden", type=int, nargs="+", default=[0, 8, 16])
    ap.add_argument("--l2", type=float, nargs="+", default=[1e-4, 3e-4, 1e-3, 3e-3, 1e-2])
    ap.add_argument("--model", default=None)
    ap.add_argument("--inc", default=f"{ROOT}/engine/src/swipe_rerank_weights.inc")
    ap.add_argument("--lang", default="*")
    ap.add_argument("--note", default="")
    a = ap.parse_args()
    if a.model is None:
        a.model = f"{a.work}/rerank.txt"
    {"split": cmd_split, "dump": cmd_dump, "train": cmd_train, "export": cmd_export}[a.step](a)


if __name__ == "__main__":
    sys.exit(main())
