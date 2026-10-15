"""tune-nlm.py — measure and tune the neural-model re-ranking (step 1b) offline.

  requests DICT NO_NGRAM_DUMP NGRAM_DUMP OUT [--m 12]
      Which words to ask the neural model about, per swipe: the top M of both first-pass rankings (FUTO's
      context model and our n-gram model), written in their dictionary spelling ("dont" -> "don't",
      "bach" -> "Bach"). Feed OUT to build/tools/swipekb-nlm-score.
  eval DICT NO_NGRAM_DUMP NGRAM_DUMP NLM_SCORES [--tune] [--exclude BENCH.jsonl --slice SLICE.jsonl]
      Re-rank with the stored neural scores exactly like swipekb::nlmRerank and print top-1/top-3; --tune
      searches the weights (do this on the tuning slice only, never on the scoreboard's bench-2000).

The dumps come from `swipekb-bench --dump` run once with --no-ngram (fills FUTO's context-model score) and once
with --use-ngram (fills our n-gram score); the candidate pool is the same search beam in both.

WHY offline: the neural model needs ~1-5 ms per swipe; a weight search re-ranks thousands of times. Storing
its scores once makes each trial take milliseconds.
Needs numpy: assets/lm-build/venv/bin/python -I bench/tune-nlm.py ...
"""
import argparse
import itertools
import json
import math

import numpy as np

# FUTO's own scoring rows (models/scoring.json), as used by our Recognizer and bench/tune-rerank.py.
NOLM = (0.5949, 0.7271, 0.0134)   # gamma, beta(length), lambda(freq): no context
LM = (0.1126, 2.2138, 0.0060)     # with context
FUTO_ALPHA = 0.6387
NGRAM_ALPHA = 1.75


def alpha_form(w):
    return "".join(c for c in w.lower() if c.isalpha())


def surfaces(dict_path):
    """letters-only form -> the dictionary's most frequent spelling of it (what the neural model should read)."""
    best = {}
    for line in open(dict_path, encoding="utf-8"):
        if "word=" not in line:
            continue
        fields = dict(kv.split("=", 1) for kv in line.strip().split(",") if "=" in kv)
        w, f = fields.get("word", ""), int(fields.get("f", 0) or 0)
        a = alpha_form(w)
        if a and (a not in best or f > best[a][1]):
            best[a] = (w, f)
    return {a: w for a, (w, f) in best.items()}


def load(no_dump, ng_dump):
    """Per swipe: words, ctc, len, freq, futo_lm, ngram_lm, target, first-of-sentence, id."""
    ng = {}
    for l in open(ng_dump, encoding="utf-8"):
        r = json.loads(l)
        ng[r["id"]] = {c[0]: c[4] for c in r["c"]}
    rows = []
    for l in open(no_dump, encoding="utf-8"):
        r = json.loads(l)
        lm = ng[r["id"]]
        c = r["c"]
        rows.append(dict(id=r["id"], target=r["target"], first=r["first"], words=[x[0] for x in c],
                         ctc=np.array([x[1] for x in c]), L=np.array([x[2] for x in c]),
                         f=np.array([x[3] for x in c]), futo=np.array([x[5] for x in c]),
                         ng=np.array([lm.get(x[0], -9.0) for x in c])))
    return rows


def base_scores(r, kind):
    """The first-pass score, exactly as the Recognizer ranks without the neural model."""
    L = np.maximum(r["L"], 1)
    if kind == "futo":
        g, b, lam = NOLM if r["first"] else LM
        s = r["ctc"] / L ** g + b * r["L"] + lam * r["f"]
        if not r["first"]:
            s = s + FUTO_ALPHA * r["futo"]
        return s
    g, b, lam = LM
    return r["ctc"] / L ** g + b * r["L"] + lam * r["f"] + NGRAM_ALPHA * r["ng"]


def order_of(s):
    return list(np.argsort(-s, kind="stable"))


def cmd_requests(a):
    surf = surfaces(a.dict)
    with open(a.out, "w", encoding="utf-8") as out:
        for r in load(a.no_dump, a.ng_dump):
            idx = []
            for kind in ("futo", "ngram"):
                for i in order_of(base_scores(r, kind))[: a.m]:
                    if i not in idx:
                        idx.append(i)
            words = [surf.get(r["words"][i], r["words"][i]) for i in idx]
            out.write(json.dumps({"id": r["id"], "words": words, "alpha": [r["words"][i] for i in idx]}) + "\n")


def rerank(r, kind, nlm, cfg):
    """Mirror of swipekb::nlmRerank: re-order the first pass's top N with base + weight * neural score.

    cfg: n, beta (neural weight), beta_short (weight for words of <= 3 letters; None = same), norm (divide the
    neural log-prob by token count), eow (include the word-end chance), margin (no change when the first
    pass's best leads by more than this; None = off).
    Returns the full new order (indices into r["words"]) with FUTO's raw-shape guard applied for top-3.
    """
    s = base_scores(r, kind)
    order = order_of(s)
    n = min(cfg["n"], len(order))
    top = order[:n]
    if cfg.get("margin") is not None and n > 1 and s[top[0]] - s[top[1]] > cfg["margin"]:
        return order
    sc = nlm.get(r["id"], {})
    new = []
    for i in top:
        w = r["words"][i]
        if w not in sc:
            return order   # neural scores missing: keep first pass (mirrors the engine's fallback)
        lp, we, tok = sc[w]
        v = lp + (we if cfg["eow"] else 0.0)
        if cfg["norm"]:
            v /= max(tok, 1)
        beta = cfg["beta"] if (cfg.get("beta_short") is None or r["L"][i] > 3) else cfg["beta_short"]
        new.append((s[i] + beta * v, i))
    new.sort(key=lambda x: -x[0])   # stable: ties keep the first pass's order
    return [i for _, i in new] + order[n:]


def metrics(rows, kind, nlm, cfg):
    t1 = t3 = 0
    hits = []
    for r in rows:
        o = rerank(r, kind, nlm, cfg) if cfg else order_of(base_scores(r, kind))
        top3 = list(o[:3])
        best_ctc = int(np.argmax(r["ctc"]))
        if best_ctc not in top3:
            top3[-1] = best_ctc
        w = [r["words"][i] for i in top3]
        ok1 = w[0] == r["target"]
        t1 += ok1
        t3 += r["target"] in w
        hits.append(ok1)
    n = len(rows)
    return 100 * t1 / n, 100 * t3 / n, np.array(hits)


def cmd_eval(a):
    rows = load(a.no_dump, a.ng_dump)
    if a.exclude and a.slice:
        bench = {json.loads(l)["sentence"] for l in open(a.exclude, encoding="utf-8")}
        drop = {json.loads(l)["id"] for l in open(a.slice, encoding="utf-8") if json.loads(l)["sentence"] in bench}
        rows = [r for r in rows if r["id"] not in drop]
    surf = surfaces(a.dict)
    nlm = {}
    reqs = {}
    for l in open(a.requests, encoding="utf-8"):
        q = json.loads(l)
        reqs[q["id"]] = q["alpha"]
    for l in open(a.nlm, encoding="utf-8"):
        q = json.loads(l)
        if q["s"]:
            nlm[q["id"]] = {al: tuple(x) for al, x in zip(reqs[q["id"]], q["s"])}
    print(f"{len(rows)} swipes; neural scores for {len(nlm)}")
    for kind in ("futo", "ngram"):
        b1, b3, bh = metrics(rows, kind, nlm, None)
        print(f"[{kind}] first pass only: top1 {b1:.2f} top3 {b3:.2f}")
    if a.cfg:
        for c in a.cfg:
            kind, cfg = parse_cfg(c)
            b1, b3, bh = metrics(rows, kind, nlm, None)
            t1, t3, h = metrics(rows, kind, nlm, cfg)
            print(f"{c:60s} top1 {t1:.2f} ({t1 - b1:+.2f}) top3 {t3:.2f} ({t3 - b3:+.2f})  fixed {int((h & ~bh).sum())} "
                  f"broke {int((~h & bh).sum())}")
            if a.show:
                for r, x, y in zip(rows, h, bh):
                    if x != y:
                        print(f"   {'FIX ' if x else 'BREAK'} {r['target']}  (short={len(r['target']) <= 3}) id={r['id']}")
    if not a.tune:
        return
    for kind in ("futo", "ngram"):
        b1, b3, _ = metrics(rows, kind, nlm, None)
        res = []
        for n, beta, norm, eow in itertools.product((3, 5, 8), (0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 4.0),
                                                    (False, True), (False, True)):
            cfg = dict(n=n, beta=beta, norm=norm, eow=eow)
            t1, t3, _ = metrics(rows, kind, nlm, cfg)
            res.append((t1, t3, cfg))
        res.sort(key=lambda x: (-x[0], -x[1]))
        print(f"== base {kind} (first pass {b1:.2f}/{b3:.2f}); best 12:")
        for t1, t3, cfg in res[:12]:
            print(f"  top1 {t1:.2f} ({t1 - b1:+.2f}) top3 {t3:.2f} ({t3 - b3:+.2f})  {cfg}")
        # The function-word guard: a separate weight for short words, around the best setting.
        t1, t3, best = res[0]
        out = []
        for bs in (0.0, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0):
            for margin in (None, 2.0, 4.0, 6.0, 8.0):
                cfg = dict(best, beta_short=bs, margin=margin)
                u1, u3, _ = metrics(rows, kind, nlm, cfg)
                out.append((u1, u3, cfg))
        out.sort(key=lambda x: (-x[0], -x[1]))
        print("  guard variants (best 6):")
        for u1, u3, cfg in out[:6]:
            print(f"  top1 {u1:.2f} ({u1 - b1:+.2f}) top3 {u3:.2f} ({u3 - b3:+.2f})  {cfg}")


def parse_cfg(s):
    """'ngram:n=5,beta=1.0,norm=0,eow=1[,beta_short=..][,margin=..]'"""
    kind, rest = s.split(":", 1)
    cfg = dict(n=5, beta=1.0, norm=False, eow=True, beta_short=None, margin=None)
    for kv in rest.split(","):
        k, v = kv.split("=")
        cfg[k] = int(v) if k == "n" else bool(int(v)) if k in ("norm", "eow") else float(v)
    return kind, cfg


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    q = sub.add_parser("requests")
    q.add_argument("dict"); q.add_argument("no_dump"); q.add_argument("ng_dump"); q.add_argument("out")
    q.add_argument("--m", type=int, default=12)
    e = sub.add_parser("eval")
    e.add_argument("dict"); e.add_argument("no_dump"); e.add_argument("ng_dump")
    e.add_argument("requests"); e.add_argument("nlm")
    e.add_argument("--tune", action="store_true")
    e.add_argument("--cfg", action="append")
    e.add_argument("--show", action="store_true")
    e.add_argument("--exclude"); e.add_argument("--slice")
    a = ap.parse_args()
    cmd_requests(a) if a.cmd == "requests" else cmd_eval(a)


if __name__ == "__main__":
    main()
