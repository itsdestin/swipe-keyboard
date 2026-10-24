# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Read gaming-contract.json and judge a set of unpacked Steam files against it.

WHY one engine for everyone: the daily canary (this file) and the later on-device check read the
same contract, so "what Gaming Mode needs from Steam" is written down in exactly one place.
Unpacked files are untrusted data: they are only ever read as text, never executed or imported.
"""
import hashlib
import json
import re
from dataclasses import dataclass, field
from pathlib import PurePosixPath

SEVERITIES = ('fail', 'review')
MAX_FILE_BYTES = 64 * 1024 * 1024


class ContractError(Exception):
    """gaming-contract.json is malformed (a bug in the contract, not in Steam)."""


@dataclass
class CheckResult:
    id: str
    status: str  # pass | fail | review
    count: int
    detail: str
    severity: str


@dataclass
class Report:
    results: list = field(default_factory=list)
    region_hash: str = ''

    @property
    def status(self):
        # fail outranks review outranks pass
        statuses = {r.status for r in self.results}
        return 'fail' if 'fail' in statuses else 'review' if 'review' in statuses else 'pass'


def _flags(s):
    return re.I if 'i' in (s or '') else 0


def _relative(path, where):
    p = PurePosixPath(path)
    if p.is_absolute() or '..' in p.parts or not p.parts:
        raise ContractError(f'{where}: path must be relative and stay inside the package: {path!r}')


def load_contract(path):
    try:
        with open(path, encoding='utf-8') as fh:
            c = json.loads(fh.read())
    except (OSError, ValueError) as e:
        raise ContractError(f'cannot read contract: {e}')
    if c.get('contract_version') != 1:
        raise ContractError('unsupported contract_version')
    packages = c.get('source', {}).get('packages', {})
    if not packages or not c.get('source', {}).get('channels'):
        raise ContractError('source.packages and source.channels are required')
    for key, spec in packages.items():
        for f in spec.get('extract') or []:
            _relative(f, f'packages.{key}.extract')
    ids = set()
    for ch in c.get('checks', []):
        cid = ch.get('id')
        if not cid or cid in ids:
            raise ContractError(f'missing or duplicate check id: {cid!r}')
        ids.add(cid)
        if ch.get('severity') not in SEVERITIES:
            raise ContractError(f'{cid}: severity must be one of {SEVERITIES}')
        if ch.get('package') not in packages:
            raise ContractError(f'{cid}: unknown package {ch.get("package")!r}')
        if ch.get('match') not in ('literal', 'regex') or not ch.get('pattern'):
            raise ContractError(f'{cid}: needs match literal|regex and a pattern')
        if ch.get('scope', 'file') not in ('file', 'region'):
            raise ContractError(f'{cid}: scope must be file or region')
        if ch.get('min') is None and ch.get('max') is None:
            raise ContractError(f'{cid}: needs min or max')
        _relative(ch.get('file', ''), cid)
        if ch['match'] == 'regex':
            try:
                re.compile(ch['pattern'], _flags(ch.get('flags')))
            except re.error as e:
                raise ContractError(f'{cid}: bad regex: {e}')
    region = c.get('region')
    if region:
        if region.get('package') not in packages or not region.get('anchors'):
            raise ContractError('region needs a known package and anchors')
        _relative(region.get('file', ''), 'region')
        for s in region.get('strip', []):
            try:
                re.compile(s['regex'])
            except (re.error, KeyError) as e:
                raise ContractError(f'region.strip: {e}')
    # The device block must only point at checks that exist, so the two sides cannot drift apart.
    dev = c.get('device', {})
    refs = [t.get('static_check') for t in dev.get('targets', [])]
    refs += [m.get('static_check') for m in dev.get('steam_client_methods', [])]
    refs += [dev.get('keyboard_dom', {}).get('static_check'), dev.get('password_signal', {}).get('static_check'),
            dev.get('ui_origin', {}).get('static_check')]
    for r in refs:
        if r is not None and r not in ids:
            raise ContractError(f'device block points at unknown static check {r!r}')
    return c


def contract_hash(contract):
    """Hash of the whole contract (canonical JSON), recorded in the ledger so a changed contract is visible."""
    blob = json.dumps(contract, sort_keys=True, separators=(',', ':')).encode()
    return hashlib.sha256(blob).hexdigest()


def _read(root, rel, max_bytes):
    p = root / rel
    if not p.is_file():
        return None, 'file missing'
    if p.stat().st_size > max_bytes:
        return None, 'file larger than the safety limit'
    return p.read_bytes().decode('utf-8', 'replace'), None


def compute_region(contract, text):
    """Concatenate windows of text around the region anchors, with the per-build stamp removed."""
    spec = contract['region']
    w = spec.get('window_chars', 1500)
    spans = []
    for anchor in spec['anchors']:
        start = 0
        while True:
            i = text.find(anchor, start)
            if i < 0:
                break
            spans.append((max(0, i - w), min(len(text), i + len(anchor) + w)))
            start = i + len(anchor)
    spans.sort()
    merged = []
    for a, b in spans:
        if merged and a <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(b, merged[-1][1]))
        else:
            merged.append((a, b))
    region = '\n--\n'.join(text[a:b] for a, b in merged)
    for s in spec.get('strip', []):
        region = re.sub(s['regex'], s['with'], region)
    return region


def evaluate(contract, roots, max_file_bytes=MAX_FILE_BYTES):
    """roots: {package key: directory with that package's unpacked files}. A missing package or file
    fails the checks that need it (silence must never read as success)."""
    report = Report()
    cache = {}

    def text_of(pkg, rel):
        key = (pkg, rel)
        if key not in cache:
            root = roots.get(pkg)
            cache[key] = (None, 'package not available') if root is None else _read(root, rel, max_file_bytes)
        return cache[key]

    region_text = None
    spec = contract.get('region')
    if spec:
        t, _ = text_of(spec['package'], spec['file'])
        if t is not None:
            region_text = compute_region(contract, t)
            report.region_hash = hashlib.sha256(region_text.encode()).hexdigest()

    for ch in contract['checks']:
        text, err = text_of(ch['package'], ch['file'])
        if ch.get('scope') == 'region':
            text = region_text
            err = None if text is not None else 'region could not be computed (file missing)'
        if text is None:
            report.results.append(CheckResult(ch['id'], 'fail', 0, err, ch['severity']))
            continue
        if ch['match'] == 'literal':
            n = text.count(ch['pattern'])
        else:
            n = len(re.findall(ch['pattern'], text, _flags(ch.get('flags'))))
        lo, hi = ch.get('min'), ch.get('max')
        ok = (lo is None or n >= lo) and (hi is None or n <= hi)
        want = ' and '.join(x for x in (f'at least {lo}' if lo is not None else '',
                                        f'at most {hi}' if hi is not None else '') if x)
        status = 'pass' if ok else ch['severity']
        report.results.append(CheckResult(ch['id'], status, n, f'found {n}, expected {want}', ch['severity']))
    return report
