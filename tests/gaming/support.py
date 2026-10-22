"""Shared helpers for the Steam canary tests (stdlib only)."""
import io
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# WHY a path insert: the canary directory has a hyphen, so it cannot be imported as a package.
sys.path.insert(0, str(ROOT / 'tools' / 'steam-canary'))
FIXTURES = Path(__file__).resolve().parent / 'fixtures'


def make_zip(entries):
    """entries: list of (name, bytes) or (ZipInfo, bytes). Names are stored verbatim (no sanitising),
    which is how Valve's zips arrive: with backslashes in directory names."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in entries:
            z.writestr(name, data)
    return buf.getvalue()
