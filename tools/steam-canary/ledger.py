"""The canary's memory: one small JSON file per channel (build, result, hashes) plus the last run time.

WHY a file on a dedicated git branch (written by the workflow): Actions caches can be evicted, and the
ledger is the evidence that the canary is alive. It is for people only; no device ever reads it.
"""
import json
import os
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

SCHEMA = 1
HISTORY_MAX = 60
DEAD_AFTER = timedelta(days=3)
# Results that are final for a given build. 'fail' and 'error' are retried every run until fixed.
_SETTLED = ('pass', 'review')


class LedgerError(Exception):
    pass


def _utc(dt):
    return dt.replace(tzinfo=timezone.utc) if dt.tzinfo is None else dt.astimezone(timezone.utc)


class Ledger:
    def __init__(self, data):
        self.data = data

    @classmethod
    def load(cls, path):
        p = Path(path)
        if not p.exists():
            return cls({'schema': SCHEMA, 'last_run': None, 'channels': {}, 'history': []})
        try:
            data = json.loads(p.read_text())
        except ValueError as e:
            # WHY not start over: silently resetting would hide that history was lost.
            raise LedgerError(f'ledger is not valid JSON: {e}')
        if not isinstance(data, dict) or data.get('schema') != SCHEMA:
            raise LedgerError('ledger has an unknown schema')
        data.setdefault('channels', {})
        data.setdefault('history', [])
        return cls(data)

    def save(self, path):
        p = Path(path)
        # WHY temp file + rename: a crash mid-write must never leave half a ledger on the branch.
        fd, tmp = tempfile.mkstemp(dir=p.parent, prefix='.ledger-')
        try:
            with os.fdopen(fd, 'w') as fh:
                json.dump(self.data, fh, indent=2, sort_keys=True)
                fh.write('\n')
            os.replace(tmp, p)
        except BaseException:
            os.unlink(tmp)
            raise

    @property
    def last_run(self):
        v = self.data.get('last_run')
        return datetime.fromisoformat(v) if v else None

    def touch(self, now):
        """Mark that the canary ran (the heartbeat the dead-canary check reads), even if nothing was new."""
        self.data['last_run'] = _utc(now).isoformat()

    def needs_check(self, channel, build, contract_hash):
        e = self.data['channels'].get(channel)
        return not (e and e['build'] == build and e['contract_hash'] == contract_hash and e['result'] in _SETTLED)

    def region_drifted(self, channel, region_hash):
        e = self.data['channels'].get(channel)
        verified = e.get('verified_region_hash') if e else None
        return bool(verified) and verified != region_hash

    def record(self, channel, build, result, contract_hash, region_hash, now):
        prev = self.data['channels'].get(channel, {})
        entry = {'build': build, 'result': result, 'checked_at': _utc(now).isoformat(),
                 'contract_hash': contract_hash, 'region_hash': region_hash,
                 'verified_region_hash': prev.get('verified_region_hash')}
        # The baseline for drift only moves on a clean pass (or the first sighting), never on a review.
        if result == 'pass' or (result == 'review' and not entry['verified_region_hash']):
            entry['verified_region_hash'] = region_hash or entry['verified_region_hash']
        self.data['channels'][channel] = entry
        hist = self.data['history']
        hist.append({'date': entry['checked_at'], 'channel': channel, 'build': build, 'result': result})
        del hist[:-HISTORY_MAX]

    def accept_drift(self, channel):
        e = self.data['channels'][channel]
        e['verified_region_hash'] = e['region_hash']


def canary_dead(ledger, now, max_age=DEAD_AFTER):
    """True if the canary has not run for longer than max_age. `now` is injected so tests (and a
    person) can simulate any date."""
    last = ledger.last_run
    return last is None or _utc(now) - _utc(last) > max_age
