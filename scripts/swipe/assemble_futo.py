"""Turn the FUTO public swipe collections (MIT data) into clean training/validation jsonl groups.

  python -I assemble_futo.py DATA_DIR OUT_DIR holdout-final.json holdout-val.json

What it does, per record:
  * drops two-finger swipes and swipes with a 'distance' above 150 (the collections use 100001+ as an invalid marker;
    the swipe-1 95th percentile is ~105),
  * DROPS every swipe whose (session, timestamp) key OR whose session appears in the evaluation files given
    (bench-2000, test-extra-10000, test.jsonl, and the clean validation sessions) -- counts are printed,
  * splits train.jsonl by session hash into 'main' (95%) and 'trv' (5%, validation that still has sentence text),
  * writes one jsonl per (collection group): main (swipe-1..4 English QWERTY), and for swipe-5 one per layout name
    with language en/de/fr/es. Each line keeps session, timestamp, word, canvas size, data, sentence, word_idx.
Also converts swipe-5/layouts/*.json to tsv layouts (letter cx cy half_w half_h) in OUT_DIR/layouts.
"""
import collections
import hashlib
import json
import os
import sys

data_dir, out_dir, hf, hv = sys.argv[1:5]
os.makedirs(out_dir + "/layouts", exist_ok=True)
hold = json.load(open(hf)); holdv = json.load(open(hv))
bad_keys = set(hold["keys"]) | set(holdv["keys"])
bad_sessions = set(hold["sessions"]) | set(holdv["sessions"])
print("holdout keys", len(bad_keys), "sessions", len(bad_sessions))

for p in os.listdir(data_dir + "/swipe-5/layouts"):
    d = json.load(open(f"{data_dir}/swipe-5/layouts/{p}"))
    with open(f"{out_dir}/layouts/{p[:-5]}.tsv", "w", encoding="utf-8") as f:
        f.write("letter\tcx\tcy\thalf_w\thalf_h\n")
        for k in d["keys"]:
            f.write(f"{k['letter']}\t{k['cx']:.5f}\t{k['cy']:.5f}\t{k['rx']:.5f}\t{k['ry']:.5f}\n")

outs = {}
def out(name):
    if name not in outs:
        outs[name] = open(f"{out_dir}/{name}.jsonl", "w", encoding="utf-8")
    return outs[name]

stat = collections.defaultdict(collections.Counter)
KEEP = ("session", "timestamp", "word", "canvas_width", "canvas_height", "data", "sentence", "word_idx")
sources = [("swipe-1", "train.jsonl"), ("swipe-2", "swipe-2/swipe2.jsonl"), ("swipe-3", "swipe-3/swipe3.jsonl"),
           ("swipe-4", "swipe-4/swipe4.jsonl"), ("swipe-5", "swipe-5/swipe5.jsonl")]
for name, rel in sources:
    for line in open(f"{data_dir}/{rel}", encoding="utf-8"):
        r = json.loads(line)
        s = stat[name]
        s["read"] += 1
        if r.get("dual_finger", 0) or r.get("distance", 0) > 150 or not isinstance(r["data"], list) or len(r["data"]) < 2:
            s["invalid"] += 1; continue
        if f"{r['session']}|{r['timestamp']}" in bad_keys:
            s["dropped_key_overlap"] += 1; continue
        if r["session"] in bad_sessions:
            s["dropped_session_overlap"] += 1; continue
        rec = {k: r[k] for k in KEEP if k in r}
        if name == "swipe-5":
            lang = r["language"]
            if lang not in ("en", "de", "fr", "es"):
                s["other_language"] += 1; continue
            group = f"s5-{r['layout']}"
        elif name == "swipe-1" and int(hashlib.sha1(r["session"].encode()).hexdigest(), 16) % 20 == 0:
            group = "trv"
        else:
            group = "main"
        s["kept"] += 1
        out(group).write(json.dumps(rec) + "\n")
for f in outs.values():
    f.close()
for k, v in stat.items():
    print(k, dict(v))
