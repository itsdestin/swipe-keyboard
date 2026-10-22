"""Remembers failed contract checks per Steam build so the helper never loops against a changed Steam
(plan Section 14, M9). Stored on disk so a restarted helper (systemd Restart=always) does not start over."""
import json
import os


class RetryLedger:
    def __init__(self, path, max_failures):
        self.path = path
        self.max = max_failures
        self.fingerprint = None
        self.failures = 0
        self.detail = ''
        self.last_failure_at = None   # monotonic; spacing only needs to hold within one run
        self._load()

    def _load(self):
        try:
            d = json.loads(self.path.read_text())
            self.fingerprint = d.get('fingerprint')
            self.failures = int(d.get('failures', 0))
            self.detail = str(d.get('detail', ''))
        except (OSError, ValueError, TypeError):
            pass

    def _save(self):
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            tmp = self.path.with_name('.retry.%d.tmp' % os.getpid())
            fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, 'w') as f:
                json.dump({'fingerprint': self.fingerprint, 'failures': self.failures, 'detail': self.detail}, f)
            os.replace(tmp, self.path)
        except OSError:
            pass

    def use_build(self, fingerprint):
        """A different Steam build gets a fresh set of tries."""
        if fingerprint != self.fingerprint:
            self.fingerprint, self.failures, self.detail = fingerprint, 0, ''
            self.last_failure_at = None
            self._save()

    @property
    def exhausted(self):
        return self.failures >= self.max

    def record_failure(self, detail, now, spacing):
        """Count a failed check; failures closer together than `spacing` count once. -> True if it counted."""
        if self.last_failure_at is not None and now - self.last_failure_at < spacing:
            return False
        self.last_failure_at = now
        self.failures += 1
        self.detail = detail
        self._save()
        return True

    def backoff(self, spacing, cap):
        """How long to wait before the next try: `spacing`, then twice that, then four times... up to `cap`.
        WHY (M9): a Steam that keeps refusing is asked less and less often, and after `max` failures not at all."""
        return min(cap, spacing * (2 ** max(0, self.failures - 1)))

    def record_success(self):
        if self.failures:
            self.failures, self.detail = 0, ''
            self._save()
