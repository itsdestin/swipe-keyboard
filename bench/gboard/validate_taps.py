# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

# Check: does Gboard suggest the same words when phrases are typed with real key taps
# as when the text box is filled directly (the fast method used by gboard_probe.py)?
#   validate_taps.py EMULATOR-SERIAL REFERENCE.tsv   (prints how many of 30 random rows match)
import random, re, sys, time
import uiautomator2 as u2
serial, rows_file = sys.argv[1], sys.argv[2]
d = u2.connect(serial)
field = d(className="android.widget.EditText")
def nodes():
    for n in re.findall(r'<node [^>]*>', d.dump_hierarchy()):
        if 'inputmethod.latin' not in n: continue
        x1, y1, x2, y2 = map(int, re.search(r'bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', n).groups())
        yield x1, y1, x2, y2, re.search(r'content-desc="([^"]*)"', n).group(1), re.search(r'resource-id="([^"]*)"', n).group(1)
def strip():
    time.sleep(0.6)
    return [c for _, c in sorted((x1, cd) for x1, y1, x2, y2, cd, rid in nodes() if 1400 < y1 and y2 < 1640 and cd and not rid)]
field.set_text(""); time.sleep(1)
keys = {cd.lower(): ((x1 + x2) // 2, (y1 + y2) // 2) for x1, y1, x2, y2, cd, rid in nodes() if re.fullmatch(r"[a-zA-Z]", cd) and y1 > 1640}
space = [((x1 + x2) // 2, (y1 + y2) // 2) for x1, y1, x2, y2, cd, rid in nodes() if cd in ("Space", "Espacio", "Espace", "Leerzeichen")][0]
rows = [l.rstrip("\n").split("\t") for l in open(rows_file, encoding="utf-8")]
rows = [r for r in rows if int(r[1]) > 0 and re.fullmatch(r"[A-Za-z ]+", r[3] + r[4][:int(r[2])]) and len(r[3]) < 40]
random.seed(7); sample = random.sample(rows, 30)
same = 0
for r in sample:
    text = r[3] + r[4][:int(r[2])]
    field.set_text(""); time.sleep(0.5)
    for i, ch in enumerate(text):
        if ch.isupper() and i > 0: d.shell("input tap 84 2024")   # shift; sentence start is auto-capitalised
        x, y = space if ch == " " else keys[ch.lower()]
        d.shell(f"input tap {x} {y}")
    typed = field.get_text()
    got = strip()
    want = r[5:8]
    ok = [w.lower() for w in got] == [w.lower() for w in want]
    same += ok
    print("SAME" if ok else "DIFF", repr(text), "typed->", repr(typed), "| taps:", got, "| fast:", want, flush=True)
print(f"{same}/30 identical")
