# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Shared helpers for the Steam canary tests (stdlib only)."""
import hashlib
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


# Synthetic stand-ins for Valve's native binaries: only the strings the contract looks for.
NATIVE_FILES = {
    'ubuntu12_32/steamui.so': b'\x7fELF....\x00.cef-enable-remote-debugging\x00more',
    'ubuntu12_32/steamclient.so': b'\x00--remote-debugging-port=%s\x00--remote-allow-origins=*\x00',
}


BASE = 'https://client-update.steamstatic.com/'


import fetch  # noqa: E402  (needs the sys.path insert above)


class FakeServer:
    """Stands in for urllib: maps file name -> bytes (or an exception to raise)."""
    def __init__(self, files):
        self.files = files
        self.calls = []

    def __call__(self, req, timeout=None):
        url = req.full_url if hasattr(req, 'full_url') else req
        self.calls.append(url)
        item = self.files.get(url[len(BASE):])
        if isinstance(item, Exception):
            raise item
        if item is None:
            raise fetch.FetchError('HTTP 404')
        return io.BytesIO(item)


def vdf(version, packages):
    """packages: {name: (file, size, sha256)}"""
    out = ['"ubuntu12"', '{', f'\t"version"\t\t"{version}"']
    for name, (f, size, sha2) in packages.items():
        out += [f'\t"{name}"', '\t{', f'\t\t"file"\t\t"{f}"', f'\t\t"size"\t\t"{size}"', f'\t\t"sha2"\t\t"{sha2}"', '\t}']
    out.append('}')
    return '\n'.join(out)


def ui_zip(mutate=None, extra=()):
    """The good UI fixture as a zip shaped like Valve's: backslash-separated directory entries."""
    files = {}
    root = FIXTURES / 'ui'
    for p in sorted(root.rglob('*')):
        if p.is_file():
            files[p.relative_to(root).as_posix()] = p.read_bytes()
    if mutate:
        mutate(files)
    entries = [('steamui\\css/', b''), ('steamui/', b'')]
    entries += list(files.items()) + list(extra)
    return make_zip(entries)


def native_zip(extra=()):
    return make_zip(list(NATIVE_FILES.items()) + list(extra))


def add_channel(server_files, manifest_name, version, ui_bytes, native_bytes):
    """Register a channel's manifest and both packages on the fake server."""
    pkgs = {}
    for pkg, data in (('steamui_websrc_all', ui_bytes), ('bins_ubuntu12', native_bytes)):
        sha1 = hashlib.sha1(data).hexdigest()
        name = f'{pkg}.zip.{sha1}'
        server_files[name] = data
        pkgs[pkg] = (name, len(data), hashlib.sha256(data).hexdigest())
    server_files[manifest_name] = vdf(version, pkgs).encode()
