#!/usr/bin/env python3
"""Steam UI canary: does Valve's newest public Steam client still match what Gaming Mode needs?

Commands:
  run         fetch the four channels, check them against gaming-contract.json, update the ledger
  dead-check  exit 1 if the canary itself has not run for 3 days (date can be injected with --now)
  accept-drift  record that a person (or the behavioural check) reviewed a keyboard-code change
  issue       open or update the single GitHub tracking issue from a summary file

Everything downloaded is untrusted data: it is hash-checked, unpacked with safe_zip, read as text
only, and deleted at the end of the run. Nothing from Valve's files is ever executed or imported.
"""
import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

# WHY: run with `python -I` (isolated) the script's own folder is not on the path automatically.
sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_contract  # noqa: E402
import fetch  # noqa: E402
import ledger as ledger_mod  # noqa: E402
import safe_zip  # noqa: E402
import tracker  # noqa: E402

FAIL_STATUSES = ('fail', 'unsafe', 'error')
RED_TITLE = 'Gaming canary: Steam UI no longer matches the contract'
DEAD_TITLE = 'Gaming canary: no run for 3 days'


@dataclass
class Outcome:
    channel: str
    status: str  # pass | review | fail | unsafe | error | unchanged
    build: str = ''
    notes: list = field(default_factory=list)


@dataclass
class Summary:
    outcomes: list = field(default_factory=list)
    contract_hash: str = ''

    @property
    def failed(self):
        return any(o.status in FAIL_STATUSES for o in self.outcomes)

    def markdown(self):
        lines = ['## Steam UI canary', '', '| Channel | Build | Result |', '|---|---|---|']
        for o in self.outcomes:
            lines.append(f'| {o.channel} | {o.build or "unknown"} | {o.status} |')
        for o in self.outcomes:
            if o.notes:
                lines += ['', f'### {o.channel} (build {o.build or "unknown"}): {o.status}']
                lines += [f'- {n}' for n in o.notes]
        lines += ['', f'Contract hash: `{self.contract_hash}`', '',
                  'Do not assume users are broken: devices fail safe to stock. Fix and ship a normal update.']
        return '\n'.join(lines) + '\n'


def _prepare(fetcher, info, spec, work, cache, limits):
    """Download, verify and unpack one package once per run; later channels reuse it (or its error)."""
    key = info.file
    if key not in cache:
        try:
            zip_path = work / 'dl' / info.file
            zip_path.parent.mkdir(exist_ok=True)
            fetcher.download_package(info, zip_path, limits['max_download_bytes'])
            dest = work / 'x' / info.file  # WHY always a brand-new, empty directory per package
            dest.mkdir(parents=True)
            only = set(spec['extract']) if spec.get('extract') else None
            safe_zip.safe_extract(zip_path, dest, only=only)
            zip_path.unlink()
            cache[key] = dest
        except (fetch.FetchError, safe_zip.UnsafeArchive) as e:
            cache[key] = e
    if isinstance(cache[key], Exception):
        raise cache[key]
    return cache[key]


def run_canary(contract_path, ledger_path, work_dir, now, fetcher, only_channels=None):
    contract = check_contract.load_contract(contract_path)
    chash = check_contract.contract_hash(contract)
    led = ledger_mod.Ledger.load(ledger_path)
    src = contract['source']
    channels = src['channels']
    if only_channels:
        unknown = set(only_channels) - {c['name'] for c in channels}
        if unknown:
            raise ValueError(f'unknown channel(s): {sorted(unknown)}')
        channels = [c for c in channels if c['name'] in only_channels]
    summary = Summary(contract_hash=chash)
    work = Path(tempfile.mkdtemp(dir=work_dir, prefix='canary-'))
    cache = {}
    try:
        for ch in channels:
            name = ch['name']
            try:
                manifest = fetcher.fetch_manifest(ch['manifest'], src['limits']['max_manifest_bytes'])
            except fetch.FetchError as e:
                summary.outcomes.append(Outcome(name, 'error', '', [f'could not read the manifest: {e}']))
                continue
            build = manifest.version
            if not led.needs_check(name, build, chash):
                summary.outcomes.append(Outcome(name, 'unchanged', build))
                continue
            try:
                roots = {}
                for key, spec in src['packages'].items():
                    info = manifest.package(spec['name'])
                    roots[key] = _prepare(fetcher, info, spec, work, cache, src['limits'])
            except safe_zip.UnsafeArchive as e:
                # WHY its own status: a hostile archive is a security event, not a missing anchor.
                led.record(name, build, 'unsafe', chash, '', now)
                summary.outcomes.append(Outcome(name, 'unsafe', build, [f'unsafe archive refused: {e}']))
                continue
            except fetch.FetchError as e:
                led.record(name, build, 'error', chash, '', now)
                summary.outcomes.append(Outcome(name, 'error', build, [str(e)]))
                continue
            report = check_contract.evaluate(contract, roots)
            status = report.status
            notes = [f'{r.status.upper()} {r.id}: {r.detail}. {_why(contract, r.id)}'
                     for r in report.results if r.status != 'pass']
            if status == 'pass' and led.region_drifted(name, report.region_hash):
                status = 'review'
                notes.append('The keyboard-related part of Steam\'s UI code changed since the last verified build. '
                             'All anchors are still present; run the behavioural check (see plan section 11).')
            led.record(name, build, status, chash, report.region_hash, now)
            summary.outcomes.append(Outcome(name, status, build, notes))
    finally:
        shutil.rmtree(work, ignore_errors=True)  # never leave Valve's files behind
        led.touch(now)
        led.save(ledger_path)
    return summary


def _why(contract, check_id):
    return next((c['why'] for c in contract['checks'] if c['id'] == check_id), '')


def _parse_now(text):
    if not text:
        return datetime.now(timezone.utc)
    try:
        return datetime.fromisoformat(text.replace('Z', '+00:00'))
    except ValueError:
        raise SystemExit(2)


def _gh(args, input=None):
    return subprocess.run(['gh', *args], input=input, text=True, check=True, capture_output=True).stdout


def main(argv=None, fetcher_factory=fetch.Fetcher):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--contract', required=True)
    r.add_argument('--ledger', required=True)
    r.add_argument('--work-dir', default=None)
    r.add_argument('--channels', default='')
    r.add_argument('--now', default='')
    r.add_argument('--summary-md')
    r.add_argument('--json-out')
    d = sub.add_parser('dead-check')
    d.add_argument('--ledger', required=True)
    d.add_argument('--now', default='')
    a = sub.add_parser('accept-drift')
    a.add_argument('--ledger', required=True)
    a.add_argument('--channel', required=True)
    i = sub.add_parser('issue')
    i.add_argument('--kind', choices=('red', 'dead'), required=True)
    i.add_argument('--body-file', required=True)
    args = ap.parse_args(argv)

    if args.cmd == 'run':
        contract = check_contract.load_contract(args.contract)
        fetcher = fetcher_factory(contract['source']['base_url'])
        only = [c for c in args.channels.split(',') if c] or None
        summary = run_canary(args.contract, args.ledger, args.work_dir or tempfile.gettempdir(),
                             _parse_now(args.now), fetcher, only)
        md = summary.markdown()
        print(md)
        if args.summary_md:
            Path(args.summary_md).write_text(md)
        if args.json_out:
            Path(args.json_out).write_text(json.dumps(
                {'failed': summary.failed, 'outcomes': [o.__dict__ for o in summary.outcomes]}, indent=2))
        return 1 if summary.failed else 0
    if args.cmd == 'dead-check':
        now = _parse_now(args.now)
        led = ledger_mod.Ledger.load(args.ledger)
        if ledger_mod.canary_dead(led, now):
            print(f'The Steam canary has not run since {led.last_run or "ever"} (more than 3 days before {now.isoformat()}).')
            return 1
        print(f'Steam canary alive: last run {led.last_run.isoformat()}')
        return 0
    if args.cmd == 'accept-drift':
        led = ledger_mod.Ledger.load(args.ledger)
        led.accept_drift(args.channel)
        led.save(args.ledger)
        return 0
    if args.cmd == 'issue':
        body = Path(args.body_file).read_text()
        title = RED_TITLE if args.kind == 'red' else DEAD_TITLE
        tracker.sync_issue(_gh, title, body)
        return 0


def cli():
    # WHY exit 3 for "the canary itself is broken": 1 means "Steam no longer matches" (or "canary dead"),
    # and those open issues with a specific meaning; a broken tool must not impersonate them.
    try:
        return main()
    except (ledger_mod.LedgerError, check_contract.ContractError, ValueError, OSError) as e:
        print(f'canary error: {e}', file=sys.stderr)
        return 3


if __name__ == '__main__':
    sys.exit(cli())
