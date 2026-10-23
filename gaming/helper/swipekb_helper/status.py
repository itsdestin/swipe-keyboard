"""Status file (plan Section 6): what the helper is doing, for the Desktop settings page. States and counts
only, never text the user typed (plan Section 8)."""
import json
import os
import time

STATES = ('waiting-for-steam', 'needs-steam-restart', 'active', 'paused-steam-changed',
          'steam-not-found', 'error', 'off',
          'switch-off',               # Steam's developer-switch file is gone: the person turned it off (or Steam/another tool did)
          'blocked-outside-access')   # the debug port is reachable from other devices; see exposure.py


class Status:
    def __init__(self, state_dir):
        self.dir = state_dir
        self.path = state_dir / 'status.json'

    def write(self, state, reason='', **fields):
        assert state in STATES, state
        doc = {'state': state, 'reason': reason, 'steam_browser': fields.get('steam_browser'),
               'layer_version': fields.get('layer_version'), 'last_inject': fields.get('last_inject'),
               'attached_pages': fields.get('attached_pages', 0), 'keyboard': fields.get('keyboard'), 'pid': os.getpid(), 'updated': time.time()}
        try:
            self.dir.mkdir(parents=True, exist_ok=True)
            tmp = self.dir / ('.status.%d.tmp' % os.getpid())
            # WHY write-then-rename: a reader never sees a half-written file.
            fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, 'w') as f:
                json.dump(doc, f)
            os.replace(tmp, self.path)
        except OSError:
            pass  # a status we cannot write must never stop the helper from doing its job
