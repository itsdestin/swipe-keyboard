"""Download Valve's public client manifests and packages, and verify them.

WHY so strict: the manifest and zips come from a third-party server. Nothing in them is allowed to
choose a URL, a local path or an unbounded amount of disk. Names are checked against a fixed shape,
sizes are capped while streaming, and both hashes Valve publishes (SHA-1 in the name, SHA-256 in the
manifest) must match before a package is used.
"""
import hashlib
import re
import time
import urllib.error
import urllib.request
from dataclasses import dataclass

USER_AGENT = 'swipe-keyboard-steam-canary (+https://github.com/itsdestin/swipe-keyboard)'
_NAME_OK = re.compile(r'^[A-Za-z0-9_]+$')
_SHA1 = re.compile(r'^[0-9a-f]{40}$')
_SHA256 = re.compile(r'^[0-9a-f]{64}$')


class FetchError(Exception):
    """Network, format or integrity problem with something Valve published."""


@dataclass(frozen=True)
class PackageInfo:
    package: str
    file: str
    size: int
    sha1: str
    sha256: str


class Manifest:
    def __init__(self, version, packages):
        self.version = version
        self._packages = packages

    def package(self, name):
        node = self._packages.get(name)
        if not isinstance(node, dict):
            raise FetchError(f'manifest has no package {name!r}')
        file = node.get('file', '')
        # WHY a fixed shape: this string is appended to a URL and used as a local file name.
        m = re.fullmatch(re.escape(name) + r'\.zip\.([0-9a-f]{40})', file)
        if not m:
            raise FetchError(f'unexpected file name for {name}: {file!r}')
        try:
            size = int(node.get('size', ''))
        except ValueError:
            raise FetchError(f'bad size for {name}')
        sha256 = node.get('sha2', '')
        if sha256 and not _SHA256.match(sha256):
            raise FetchError(f'bad sha2 for {name}')
        return PackageInfo(name, file, size, m.group(1), sha256)


def _tokens(text):
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"|([{}])', text):
        yield m.group(2) or ('s', m.group(1))


def parse_manifest(text):
    """Parse Valve's KeyValues text format (nested braces, quoted strings)."""
    toks = list(_tokens(text))

    def block(i):
        out = {}
        while i < len(toks):
            t = toks[i]
            if t == '}':
                return out, i + 1
            if not isinstance(t, tuple):
                raise FetchError('manifest syntax error')
            key = t[1]
            i += 1
            if i >= len(toks):
                raise FetchError('manifest ended unexpectedly')
            nxt = toks[i]
            if nxt == '{':
                out[key], i = block(i + 1)
            elif isinstance(nxt, tuple):
                out[key] = nxt[1]
                i += 1
            else:
                raise FetchError('manifest syntax error')
        raise FetchError('manifest ended before closing brace')

    # WHY several top-level blocks: real manifests end with a second "kvsignatures" block. We read the
    # first block (the package list) and only check that the rest is well-formed; the signature is not
    # verified here (HTTPS plus the per-package hashes are what we rely on).
    if len(toks) < 4 or not isinstance(toks[0], tuple) or toks[1] != '{':
        raise FetchError('not a Steam client manifest')
    body, i = block(2)
    while i < len(toks):
        if not isinstance(toks[i], tuple) or i + 1 >= len(toks) or toks[i + 1] != '{':
            raise FetchError('manifest has trailing content')
        _, i = block(i + 2)
    version = body.get('version', '')
    if not re.fullmatch(r'\d{6,15}', version):
        raise FetchError(f'manifest version missing or odd: {version!r}')
    return Manifest(version, body)


class Fetcher:
    def __init__(self, base_url, opener=urllib.request.urlopen, sleep=time.sleep, retries=3, timeout=60):
        if not base_url.startswith('https://') or not base_url.endswith('/'):
            raise ValueError('base_url must be https and end with a slash')
        self.base_url, self.opener, self.sleep, self.retries, self.timeout = base_url, opener, sleep, retries, timeout

    def _open(self, name):
        req = urllib.request.Request(self.base_url + name, headers={'User-Agent': USER_AGENT})
        last = None
        for attempt in range(self.retries):
            try:
                return self.opener(req, timeout=self.timeout)
            except FetchError:
                raise  # a definite answer (e.g. 404): retrying will not change it
            except urllib.error.HTTPError as e:
                if e.code < 500:
                    raise FetchError(f'HTTP {e.code} for {name}')
                last = e
            except OSError as e:  # includes URLError and timeouts
                last = e
            if attempt + 1 < self.retries:
                self.sleep(2 ** attempt)  # 1 s, 2 s, ...
        raise FetchError(f'could not fetch {name}: {last}')

    def _stream(self, name, max_bytes):
        resp = self._open(name)
        try:
            n = 0
            while True:
                chunk = resp.read(1 << 20)
                if not chunk:
                    return
                n += len(chunk)
                if n > max_bytes:
                    raise FetchError(f'{name} is larger than the {max_bytes}-byte limit')
                yield chunk
        finally:
            close = getattr(resp, 'close', None)
            if close:
                close()

    def fetch_manifest(self, name, max_bytes):
        if not _NAME_OK.match(name):
            raise FetchError(f'bad manifest name {name!r}')
        data = b''.join(self._stream(name, max_bytes))
        return parse_manifest(data.decode('utf-8', 'replace'))

    def download_package(self, info, dest, max_bytes):
        """Download info.file to `dest`, verify size and both hashes, return dest. On any problem the
        partial file is deleted and FetchError is raised."""
        from pathlib import Path
        dest = Path(dest)
        if info.size > max_bytes:
            raise FetchError(f'{info.package}: manifest size {info.size} exceeds the {max_bytes}-byte limit')
        sha1, sha256, n = hashlib.sha1(), hashlib.sha256(), 0
        try:
            with open(dest, 'wb') as out:
                for chunk in self._stream(info.file, min(max_bytes, info.size + 1)):
                    n += len(chunk)
                    sha1.update(chunk)
                    sha256.update(chunk)
                    out.write(chunk)
            if n != info.size:
                raise FetchError(f'{info.package}: got {n} bytes, manifest says {info.size}')
            if sha1.hexdigest() != info.sha1:
                raise FetchError(f'{info.package}: SHA-1 does not match the name Valve published')
            if info.sha256 and sha256.hexdigest() != info.sha256:
                raise FetchError(f'{info.package}: SHA-256 does not match the manifest')
        except BaseException:
            dest.unlink(missing_ok=True)
            raise
        return dest
