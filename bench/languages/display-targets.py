#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""display-targets.py DATASET.json DICTIONARY.combined [--out FILE]

Rewrites each case's `target` to the spelling the dictionary shows for that word (German nouns with their capital,
names, "l’école"), keeping ids, keys and traces unchanged.
WHY: synthetic-noisy-v1 targets were sampled lower-case from the old wordfreq-derived lists, which had no noun
capitals. The open word lists spell words as written, so a correct "Beitrag" was counted as a miss against
"beitrag" in the case-sensitive top1/top3. Re-spelling the targets (rather than comparing case-insensitively)
keeps the case-sensitive metric meaningful: a lower-case "beitrag" output is still a miss for a noun.
Where several spellings share a lower-case form, the most frequent one is used. Words not in the dictionary keep
their target (they stay misses and show in targetInDictionary)."""
import json, re, sys, unicodedata

def load(path):
    best = {}
    for line in open(path, encoding='utf-8'):
        m = re.match(r" word=([^,]+),f=(\d+)", line)
        if m:
            w, f = unicodedata.normalize('NFC', m.group(1)), int(m.group(2))
            k = w.lower()
            if k not in best or f > best[k][1]:
                best[k] = (w, f)
    return {k: v[0] for k, v in best.items()}

def main():
    path, dictionary = sys.argv[1], sys.argv[2]
    out = sys.argv[4] if len(sys.argv) > 4 and sys.argv[3] == '--out' else path
    display = load(dictionary)
    data = json.load(open(path, encoding='utf-8'))
    changed = 0
    for case in data['cases']:
        shown = display.get(unicodedata.normalize('NFC', case['target']).lower())
        if shown and shown != case['target']:
            case['target'] = shown; changed += 1
    json.dump(data, open(out, 'w', encoding='utf-8'), ensure_ascii=False, separators=(',', ':'))
    print(f'{out}: {changed} targets re-spelled', file=sys.stderr)

if __name__ == '__main__':
    main()
