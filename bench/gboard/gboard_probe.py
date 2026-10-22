# Black-box Gboard next-word / completion probe on an Android emulator (card #170 comparison).
# Puts "context + first k letters" into a text field, reads Gboard's 3 suggestions from the screen.
#   gboard_probe.py LANG SENTENCES OUT.tsv [--count N] [--serial emulator-5554]
# Output rows: sentence# word# k context target slot_left slot_middle slot_right
import argparse, re, sys, time
import uiautomator2 as u2

SPACE = {"en": "Space", "es": "Espacio", "fr": "Espace", "de": "Leerzeichen"}
WORD = re.compile(r"^[^\W\d_]+(?:['’-][^\W\d_]+)*$")
STRIP = "¿¡\"'«»“”„‚‘’()[]—–-.,!?;:…"

ap = argparse.ArgumentParser()
ap.add_argument("lang"); ap.add_argument("sentences"); ap.add_argument("out")
ap.add_argument("--count", type=int, default=100); ap.add_argument("--serial", default="emulator-5554")
ap.add_argument("--skip", type=int, default=0)
a = ap.parse_args()
d = u2.connect(a.serial)

def nodes():
    xml = d.dump_hierarchy()
    for m in re.finditer(r'<node [^>]*>', xml):
        n = m.group(0)
        if 'inputmethod.latin' not in n: continue
        x1, y1, x2, y2 = map(int, re.search(r'bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', n).groups())
        cd = re.search(r'content-desc="([^"]*)"', n).group(1)
        rid = re.search(r'resource-id="([^"]*)"', n).group(1)
        yield x1, y1, x2, y2, cd, rid

def strip():
    out = [(x1, cd) for x1, y1, x2, y2, cd, rid in nodes() if 1400 < y1 and y2 < 1640 and cd and not rid]
    return [c for _, c in sorted(out)]

def unescape(s):
    return s.replace("&quot;", '"').replace("&apos;", "'").replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&")

field = d(className="android.widget.EditText")
# WHY: freshly booted simulators show "isn't responding" dialogs and may not have the message box open yet.
for _ in range(20):
    d(text="Wait").click_exists(timeout=1)
    if field.exists(timeout=2): break
    d.shell("input keyevent 4"); time.sleep(1)   # a popup over the box
    if field.exists(timeout=2): break
    d.shell("am start -a android.intent.action.SENDTO -d sms:5551234"); time.sleep(5)
else:
    sys.exit("no text field")
field.set_text("")
def focus():
    # WHY tap the right edge of an emptied box: tapping words opens the messaging app's spelling popup.
    box = field.info["bounds"]
    r, t, b = box["right"], box["top"], box["bottom"]
    d.shell(f"input tap {r - 40} {(t + b) // 2}"); time.sleep(1.5)
focus()
time.sleep(1)
for _ in range(6):   # cycle the language key until the space bar names the wanted language
    if any(cd == SPACE[a.lang] for *_, cd, rid in nodes()): break
    d.shell("input tap 325 2196"); time.sleep(2)   # WHY adb tap: the library's click doesn't reach this key
else:
    sys.exit("could not switch to " + a.lang)

def read(text):
    # WHY retries: the on-phone helper sometimes drops its connection; reconnecting and retrying is enough.
    global d, field
    for attempt in range(4):
        try:
            return read_once(text)
        except Exception as e:
            print("retry after", type(e).__name__, flush=True); time.sleep(3)
            d = u2.connect(a.serial); field = d(className="android.widget.EditText")
            if not field.exists(timeout=2):   # a popup covers the box: dismiss it, refocus the box
                d.shell("input keyevent 4"); time.sleep(1)
                if field.exists(timeout=2): field.set_text(""); focus()
    return read_once(text)

def read_once(text):
    field.set_text(text)
    time.sleep(0.2)
    prev = strip()
    for _ in range(8):
        time.sleep(0.15)
        cur = strip()
        if cur == prev and cur: return cur
        prev = cur
    return prev

lines = [l.strip() for l in open(a.sentences, encoding="utf-8") if l.strip()][a.skip:a.skip + a.count]
t0 = time.time(); n = 0
with open(a.out, "a", encoding="utf-8") as out:
    for si, line in enumerate(lines, a.skip):
        chunks = line.split()
        for wi, chunk in enumerate(chunks):
            target = chunk.strip(STRIP)
            if not WORD.match(target): continue
            context = " ".join(chunks[:wi]) + (" " if wi else "")
            for k in range(4):
                if k and k >= len(target): break
                s = [unescape(x) for x in read(context + target[:k])]
                s += [""] * (3 - len(s))
                out.write("\t".join([str(si), str(wi), str(k), context, target] + s[:3]) + "\n")
                n += 1
        out.flush()
        print(f"{si+1-a.skip}/{len(lines)} sentences, {n} reads, {(time.time()-t0)/max(n,1):.2f}s/read", flush=True)
field.set_text("")
