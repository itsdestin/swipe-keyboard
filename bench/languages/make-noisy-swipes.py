#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Synthetic NOISY swipe traces for the offline language bench (provenance "synthetic-noisy-v1").

These are NOT human swipes and NOT native reviewed. A small noise model, fitted on real
ENGLISH swipes (the MIT-licensed swipe.futo.org dataset), is applied to the other languages' layouts so that
Spanish / French / German get a *provisional* number before real volunteers exist
(roadmap item 0.5). The model is deliberately simple; it can only reproduce the effects
listed below, so it still flatters a decoder compared with real people (see README).

Modes (stdlib only; seeded; same inputs -> byte-identical output):
  fit    --dev dev.jsonl --out noisy-params-v1.json     fit the noise model on real English swipes
  words  --lang es --text-dir DIR --n 400 --seed 1      sample frequency-weighted target words
  gen    --lang es|fr|de|en ... --out dataset.json       write a bench dataset (synthetic-noisy-v1)
  real   --jsonl bench-2000.jsonl --out dataset.json     (calibration only) real English swipes as
                                                         "supplied-human" cases for the same bench
Never fit on bench-2000.jsonl or test.jsonl; those are held-out scoring sets.

Units: one key = 1 x 1 "key unit". Real data are fractions of the letter area; QWERTY key
centres sit at cx = 0.05+0.1*i, i.e. x_units = 10*x_frac and y_units = 3*y_frac (3 rows).
So the fitted offsets are independent of screen size.
"""
import argparse, json, math, random, re, sys, unicodedata
from collections import Counter

ROWS = {'qwerty': ['qwertyuiop', 'asdfghjkl', 'zxcvbnm'],
        'azerty': ['azertyuiop', 'qsdfghjklm', 'wxcvbn'],
        'qwertz': ['qwertzuiop', 'asdfghjkl', 'yxcvbnm']}
# Pixel size of one key unit, copied from the real English canvas (422 x 170.3 px, 10 x 3 keys)
# so the bench sees the same aspect ratio the real swipes had.
UX, VY = 42.2, 170.3125 / 3
LAYOUT_OF = {'en': 'qwerty', 'es': 'qwerty', 'fr': 'azerty', 'de': 'qwertz'}
LOCALE_OF = {'en': 'en-US', 'es': 'es-ES', 'fr': 'fr-FR', 'de': 'de-DE'}
DICT_OF = {'es': 'es.combined', 'fr': 'fr.combined', 'de': 'de.combined'}
PACKS = '/usr/share/swipe-keyboard/language-packs'
# Same trace rules as make-seeds.py: the finger passes over the BASE key of an accented
# letter (popup is a long-press, not a swipe target); apostrophes are never swiped.
BASE = {'œ': 'o', 'ß': 's', 'æ': 'a'}


def key_centres(lang):
    """letter -> (cx, cy) in key units. Rows are staggered like a phone keyboard."""
    layout = LAYOUT_OF[lang]
    rows = ROWS[layout]
    offs = [0.5, 1.0, 2.0]                 # QWERTY/QWERTZ: row 2 half a key in, row 3 1.5 keys in
    if layout == 'azerty':
        offs = [0.5, 0.5, 2.5]             # AZERTY row 2 is full width (10 keys), row 3 six keys centred
    c = {ch: (offs[r] + i, 0.5 + r) for r, row in enumerate(rows) for i, ch in enumerate(row)}
    if lang == 'es':
        c['ñ'] = (offs[1] + 9, 1.5)        # ñ is a real 10th key after l; wider than 10 units by 0.5
    return c


def bench_keys(lang):
    return [dict(letter=ch, x=round((cx - .5) * UX, 3), y=round((cy - .5) * VY, 3), w=UX, h=round(VY, 3))
            for ch, (cx, cy) in key_centres(lang).items()]


def base_letters(target):
    """Keys the finger crosses for a target word (apostrophes dropped, accents to base)."""
    out = []
    for ch in target.lower():
        if ch in "'’-": continue
        ch = BASE.get(ch, ch)
        if ch != 'ñ':
            ch = unicodedata.normalize('NFD', ch)[0]
        out.append(ch)
    return out


# ---------------------------------------------------------------- the trace model
def simulate(keys, centres, P, rng, scale=1.0):
    """One synthetic swipe through the key sequence `keys`; returns [(x_units, y_units, t_ms)].

    WHY each piece exists (all fitted on real English, see `fit`):
      * start/end offsets: people land and lift anywhere on the key, lift is sloppier;
      * interior waypoints: path misses key centres by a random amount (sigma_interior)
        and cuts corners toward the chord between its neighbours (corner_cut * turn angle);
      * Catmull-Rom curve through the waypoints: a finger draws a smooth line that
        overshoots at sharp turns, not a polyline;
      * duration/speed: total time linear in ideal path length with log-normal spread; speed dips at
        sharp turns and at lift-off (see 'Speed profile');
      * sample times: drawn from the real inter-sample gaps (Android touch events are uneven);
      * jitter: small independent position noise on every sample.
    `scale` multiplies ALL spatial noise (start, end, interior, jitter). It is the one knob calibrated
    against real recogniser scores: the noise measured in key units is larger than what the real
    recogniser seems to experience (the recogniser was trained on this kind of data), so the raw fit
    (scale 1.0) is harsher than real; `calibration` in the params file records the chosen value."""
    c = [centres[k] for k in keys]
    n = len(c)
    wp = []
    for i, (cx, cy) in enumerate(c):
        if i == 0:
            b = P['tail_mult'] if rng.random() < P['start_tail_p'] else 1.0
            x = cx + rng.gauss(P['start_mu'][0], P['start_sd'][0] * b * scale)
            y = cy + rng.gauss(P['start_mu'][1], P['start_sd'][1] * b * scale)
        elif i == n - 1:
            b = P['tail_mult'] if rng.random() < P['end_tail_p'] else 1.0
            x = cx + rng.gauss(P['end_mu'][0], P['end_sd'][0] * b * scale)
            y = cy + rng.gauss(P['end_mu'][1], P['end_sd'][1] * b * scale)
        else:
            px, py = c[i - 1]; nx, ny = c[i + 1]
            a1 = math.atan2(cy - py, cx - px); a2 = math.atan2(ny - cy, nx - cx)
            turn = abs((a2 - a1 + math.pi) % (2 * math.pi) - math.pi) / math.pi   # 0 straight .. 1 reversal
            cut = P['corner_cut'] * turn
            # heavy tail: most keys are hit fairly well, a minority (hurried, thumb off-axis) are
            # missed by a lot; a single Gaussian cannot match both the median and the 90th percentile.
            big = P['tail_mult'] if rng.random() < P['tail_p'] else 1.0
            x = cx + cut * ((px + nx) / 2 - cx) + rng.gauss(0, P['interior_sd'][0] * scale * big)
            y = cy + cut * ((py + ny) / 2 - cy) + rng.gauss(0, P['interior_sd'][1] * scale * big)
        wp.append((x, y))
    # dense Catmull-Rom polyline through the waypoints
    pts = [wp[0]]
    ext = [wp[0]] + wp + [wp[-1]]
    for i in range(1, len(ext) - 2):
        p0, p1, p2, p3 = ext[i - 1], ext[i], ext[i + 1], ext[i + 2]
        for s in range(1, 13):
            t = s / 12; t2 = t * t; t3 = t2 * t
            pts.append(tuple(0.5 * (2 * p1[d] + (-p0[d] + p2[d]) * t + (2 * p0[d] - 5 * p1[d] + 4 * p2[d] - p3[d]) * t2
                                    + (-p0[d] + 3 * p1[d] - 3 * p2[d] + p3[d]) * t3) for d in (0, 1)))
    cum = [0.0]
    for a, b in zip(pts, pts[1:]): cum.append(cum[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
    length = cum[-1]
    ideal = sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(c, c[1:]))
    dur = (P['dur_a'] + P['dur_b'] * ideal) * math.exp(rng.gauss(0, P['dur_logsd']))   # seconds
    dur = max(dur, 0.12)
    # Speed profile. WHY: real fingers slow down at sharp turns (measured median 0.64 x trace speed
    # vs 1.22 x on straight stretches) and crawl at lift-off (0.41 x) and start (0.72 x). That
    # dwell is how the recogniser tells a deliberate letter from a pass-through, so a constant-speed
    # trace is unrealistically hard. w(s) = relative speed at arc position s; time = integral ds/w.
    turns = []
    for i in range(1, n - 1):
        px, py = c[i - 1]; cx, cy = c[i]; nx, ny = c[i + 1]
        a1 = math.atan2(cy - py, cx - px); a2 = math.atan2(ny - cy, nx - cx)
        turns.append((cum[i * 12], abs((a2 - a1 + math.pi) % (2 * math.pi) - math.pi) / math.pi))
    tim = [0.0]
    for k in range(1, len(cum)):
        sm = (cum[k] + cum[k - 1]) / 2
        w = 1.0
        for sk, tn in turns: w -= P['turn_dip'] * tn * math.exp(-((sm - sk) / P['dip_width']) ** 2)
        w *= 1 - P['start_slow'] * math.exp(-sm / P['dip_width'])
        w *= 1 - P['end_slow'] * math.exp(-(length - sm) / P['dip_width'])
        tim.append(tim[-1] + (cum[k] - cum[k - 1]) / max(w, 0.15))
    total = tim[-1] or 1.0
    out = []; t = 0.0; j = 0
    while True:
        tau = min(total, total * t / (dur * 1000))
        while j < len(tim) - 2 and tim[j + 1] < tau: j += 1
        seg = tim[j + 1] - tim[j]
        f = 0 if seg <= 0 else (tau - tim[j]) / seg
        x = pts[j][0] + f * (pts[j + 1][0] - pts[j][0]) + rng.gauss(0, P['jitter_sd'] * scale)
        y = pts[j][1] + f * (pts[j + 1][1] - pts[j][1]) + rng.gauss(0, P['jitter_sd'] * scale)
        out.append((x, y, round(t, 1)))
        if tau >= total: break
        t += rng.choice(P['dt_ms'])
    if len(out) < 2: out.append((out[0][0], out[0][1], out[0][2] + 16.0))
    return out


# ---------------------------------------------------------------- fitting on real English
def qs(a, qlist):
    a = sorted(a); return [a[min(len(a) - 1, int(len(a) * q))] for q in qlist]


def load_real(path, n):
    """Real swipes whose word is plain a-z (>=3 distinct consecutive letters). -> (word, keys, pts_units)"""
    cen = key_centres('en'); out = []
    for line in open(path):
        r = json.loads(line); w = r['word'].lower()
        if not (w.isascii() and w.isalpha()) or len(r['data']) < 8: continue
        ks = [w[0]] + [b for a, b in zip(w, w[1:]) if a != b]
        if len(ks) < 3: continue
        d = r['data']
        out.append((w, ks, [(p['x'] * 10, p['y'] * 3, p['t'] - d[0]['t']) for p in d]))
        if len(out) >= n: break
    return out


def path_stats(keys, pts, cen):
    """Per-swipe numbers we match: nearest approach to each interior key, path length / ideal length."""
    miss = [min(math.hypot(p[0] - cen[k][0], p[1] - cen[k][1]) for p in pts) for k in keys[1:-1]]
    L = sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(pts, pts[1:]))
    ideal = sum(math.hypot(cen[b][0] - cen[a][0], cen[b][1] - cen[a][1]) for a, b in zip(keys, keys[1:]))
    return miss, L / ideal if ideal else 1.0


def second_diff_sd(swipes):
    """Robust (MAD) sd of p_i - (p_{i-1}+p_{i+1})/2 over all samples; used to size the jitter."""
    r = []
    for _, _, pts in swipes:
        for a, b, c in zip(pts, pts[1:], pts[2:]):
            r += [abs(b[0] - (a[0] + c[0]) / 2), abs(b[1] - (a[1] + c[1]) / 2)]
    return 1.4826 * sorted(r)[len(r) // 2]


def cmd_fit(a):
    cen = key_centres('en')
    D = load_real(a.dev, a.n)
    m = lambda v: sum(v) / len(v)
    sd = lambda v: math.sqrt(m([x * x for x in v]) - m(v) ** 2)
    sx = [p[0][0] - cen[k[0]][0] for _, k, p in D]; sy = [p[0][1] - cen[k[0]][1] for _, k, p in D]
    ex = [p[-1][0] - cen[k[-1]][0] for _, k, p in D]; ey = [p[-1][1] - cen[k[-1]][1] for _, k, p in D]
    dts = [b[2] - a_[2] for _, _, p in D for a_, b in zip(p, p[1:]) if 0 < b[2] - a_[2] <= 60]
    idl = [sum(math.hypot(cen[b][0] - cen[x][0], cen[b][1] - cen[x][1]) for x, b in zip(k, k[1:])) for _, k, _ in D]
    T = [p[-1][2] / 1000 for _, _, p in D]
    # least-squares T = dur_a + dur_b * ideal
    mi, mt = m(idl), m(T)
    b = sum((i - mi) * (t - mt) for i, t in zip(idl, T)) / sum((i - mi) ** 2 for i in idl)
    a0 = mt - b * mi
    res = [math.log(max(t, .05) / max(a0 + b * i, .1)) for i, t in zip(idl, T)]
    # Touch offsets are heavy-tailed: the robust (MAD) spread is the 'normal' landing, and a
    # minority of swipes land far off. tail_p is solved so core mixture sd == the plain sd.
    def mad(v):
        med = sorted(v)[len(v) // 2]; return 1.4826 * sorted(abs(x - med) for x in v)[len(v) // 2]
    TM = 3.0
    tailp = lambda a, b: max(0.0, min(0.5, ((sd(a) / mad(a) + sd(b) / mad(b)) / 2) ** 2 - 1) / (TM * TM - 1))
    P = dict(start_mu=[m(sx), m(sy)], start_sd=[mad(sx), mad(sy)], end_mu=[m(ex), m(ey)], end_sd=[mad(ex), mad(ey)],
             start_tail_p=tailp(sx, sy), end_tail_p=tailp(ex, ey),
             dur_a=a0, dur_b=b, dur_logsd=sd(res), dt_ms=sorted(dts)[::max(1, len(dts) // 400)],
             interior_sd=[0.2, 0.2], corner_cut=0.3, jitter_sd=0.0, tail_p=0.0, tail_mult=TM,
             # speed profile, read off the real swipes (medians of local speed / trace speed)
             turn_dip=0.75, dip_width=0.5, start_slow=0.3, end_slow=0.6)
    real_miss = []; real_ratio = []
    for _, k, p in D:
        ms, r = path_stats(k, p, cen); real_miss += ms; real_ratio.append(r)
    Q = [.1, .25, .5, .75, .9]
    target = qs(real_miss, Q) + qs(real_ratio, [.5])
    rng0 = random.Random(7)
    sub = D[:1500]

    def score(P_):
        rng = random.Random(11); sm = []; sr = []
        for _, k, _ in sub:
            ms, r = path_stats(k, simulate(k, cen, P_, rng), cen); sm += ms; sr.append(r)
        got = qs(sm, Q) + qs(sr, [.5])
        return sum(abs(g - t) / max(t, .05) for g, t in zip(got, target)), got
    # jitter: real excess second-difference variance over a jitter-free simulation
    sim0 = []; rng = random.Random(5)
    for w, k, _ in sub: sim0.append((w, k, simulate(k, cen, P, rng)))
    v_real, v_sim = second_diff_sd(D[:1500]) ** 2, second_diff_sd(sim0) ** 2
    P['jitter_sd'] = math.sqrt(max(v_real - v_sim, 0) / 1.5)
    best = None
    for isd in (0.08, 0.12, 0.16, 0.2, 0.25, 0.3):
        for cut in (0.0, 0.2, 0.4, 0.6):
            for tp in (0.0, 0.1, 0.2, 0.3):
                P2 = dict(P, interior_sd=[isd, isd * 0.6], corner_cut=cut, tail_p=tp)   # vertical slop is smaller
                s, got = score(P2)
                if best is None or s < best[0]: best = (s, P2, got)
    P = best[1]
    P['_fit'] = dict(source=a.dev.split('/')[-1], swipes=len(D), loss=round(best[0], 3),
                     target_quantiles=dict(q=Q + ['ratio_med'], real=[round(x, 3) for x in target],
                                           synthetic=[round(x, 3) for x in best[2]]))
    json.dump(P, open(a.out, 'w'), indent=1); print(json.dumps(P['_fit'], indent=1))


# ---------------------------------------------------------------- word sampling
TOKEN = re.compile(r"[^\W\d_]+(?:['’][^\W\d_]+)*", re.U)


def dictionary(lang):
    words = set()
    with open(f'{PACKS}/{DICT_OF[lang]}', encoding='utf-8', errors='replace') as f:
        for line in f:
            if line.startswith(' word='):
                e = line.find(',f=')
                if e > 6: words.add(line[6:e])
    return words


def sample_words(lang, text_dir, n, seed):
    """Frequency-weighted sample of dictionary words from everyday.txt + web.txt.

    Case rule ("only where the dictionary would"): keep the token as written if that exact
    spelling is a dictionary word (German nouns, names); else use the lower-case form if the
    dictionary has it (sentence-initial 'Los' -> 'los'); else skip as out-of-dictionary.
    Apostrophes may be ' or ’; whichever spelling the dictionary has is the target."""
    D = dictionary(lang); occ = Counter(); skipped = 0; seen = 0
    for name in ('everyday.txt', 'web.txt'):
        for line in open(f'{text_dir}/{name}', encoding='utf-8'):
            for tok in TOKEN.findall(unicodedata.normalize('NFC', line)):
                if len(tok) < 2 and tok.lower() not in D: continue
                seen += 1
                cands = [tok, tok.lower()]
                cands += [x.replace("'", '’') for x in cands] + [x.replace('’', "'") for x in cands]
                hit = next((x for x in cands if x in D), None)
                if hit is None or tok.isupper() and len(tok) > 1 and tok.lower() not in D: skipped += 1; continue
                if hit and len(base_letters(hit)) >= 2: occ[hit] += 1
    ws = sorted(occ); rng = random.Random(seed)
    pick = rng.choices(ws, weights=[occ[w] for w in ws], k=n)
    return pick, dict(tokens_seen=seen, skipped_out_of_dictionary=skipped, distinct_words=len(ws))


# ---------------------------------------------------------------- datasets
def case_header(lang, i, target, prov, reviewed=False):
    return dict(id=f'{LOCALE_OF[lang]}-n{i:04d}', locale=LOCALE_OF[lang], layout=LAYOUT_OF[lang], target=target,
                provenance=prov, nativeReviewed=reviewed)


def write(path, cases):
    json.dump(dict(schema=1, matchingPolicy=dict(display='NFC-case-sensitive',
                   lookup='shared-lookup-lowercase-NFC-no-diacritic-folding', denominator='all-input-cases'),
                   cases=cases), open(path, 'w'), ensure_ascii=False, separators=(',', ':'))


def cmd_gen(a):
    P = json.load(open(a.params)); cen = key_centres(a.lang); keys = bench_keys(a.lang)
    if a.scale is None: a.scale = P.get('calibration', {}).get('scale', 1.0)
    if a.words_json:
        words = json.load(open(a.words_json))
    else:
        words, info = sample_words(a.lang, a.text_dir, a.n, a.seed); print(a.lang, info, file=sys.stderr)
    rng = random.Random(a.seed * 1000 + 17); cases = []
    for i, w in enumerate(words):
        trace = [k for k in base_letters(w)]
        ks = [trace[0]] + [y for x, y in zip(trace, trace[1:]) if x != y]     # double letters: one pass over the key
        if len(ks) < 2: continue
        tr = simulate(ks, cen, P, rng, a.scale)
        # WHY rounded: keeps files small; 0.01 px is far below any touch resolution.
        pts = [dict(x=round(x * UX, 2), y=round(y * VY, 2), t=t) for x, y, t in tr]
        cases.append(dict(case_header(a.lang, i, w, 'synthetic-noisy-v1'), keys=keys, points=pts))
    write(a.out, cases); print(f'{a.out}: {len(cases)} cases', file=sys.stderr)


def cmd_real(a):
    """Calibration helper: the REAL swipes of a jsonl as bench cases (plain a-z words only)."""
    cases = []; keysets = {}
    for line in open(a.jsonl):
        r = json.loads(line); w = r['word'].lower()
        if not (w.isascii() and w.isalpha()) or len(r['data']) < 2: continue
        ux, vy = r['canvas_width'] / 10, r['canvas_height'] / 3
        d = r['data']; t0 = d[0]['t']
        keys = [dict(letter=ch, x=round((cx - .5) * ux, 3), y=round((cy - .5) * vy, 3), w=round(ux, 3), h=round(vy, 3))
                for ch, (cx, cy) in key_centres('en').items()]
        pts = [dict(x=round(p['x'] * r['canvas_width'], 2), y=round(p['y'] * r['canvas_height'], 2), t=p['t'] - t0) for p in d]
        cases.append(dict(case_header('en', len(cases), w, 'supplied-human'), keys=keys, points=pts))
    write(a.out, cases); print(f'{a.out}: {len(cases)} cases', file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest='cmd', required=True)
    f = sp.add_parser('fit'); f.add_argument('--dev', required=True); f.add_argument('--out', required=True)
    f.add_argument('--n', type=int, default=6000); f.set_defaults(fn=cmd_fit)
    g = sp.add_parser('gen'); g.add_argument('--lang', required=True, choices=sorted(LAYOUT_OF))
    g.add_argument('--params', required=True); g.add_argument('--out', required=True)
    g.add_argument('--text-dir'); g.add_argument('--words-json', help='JSON list of target words (en calibration)')
    g.add_argument('--n', type=int, default=400); g.add_argument('--seed', type=int, default=1)
    g.add_argument('--scale', type=float, default=None, help='spatial noise multiplier (default: params calibration.scale)')
    g.set_defaults(fn=cmd_gen)
    r = sp.add_parser('real'); r.add_argument('--jsonl', required=True); r.add_argument('--out', required=True)
    r.set_defaults(fn=cmd_real)
    a = ap.parse_args(); a.fn(a)


if __name__ == '__main__':
    main()
