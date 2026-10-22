"""Extract a zip that came from the network without trusting anything inside it.

WHY: the Steam UI and client packages are downloaded from a third party. A hostile or corrupted
archive must not be able to write outside the destination, create links, or fill the disk.
Everything is validated BEFORE the first byte is written, and sizes are enforced on the bytes
actually inflated (a zip header can lie about its size).
"""
import re
import stat
import zipfile
from pathlib import Path


class UnsafeArchive(Exception):
    """The archive broke a safety rule; nothing from it should be used."""


# Defaults sized for Valve's bundles (steamui ~108 MB unpacked, largest file ~16 MB) with headroom.
MAX_ENTRIES = 5000
MAX_ENTRY_BYTES = 64 * 1024 * 1024
MAX_TOTAL_BYTES = 600 * 1024 * 1024
MAX_RATIO = 1000  # compressed:uncompressed; real bundles are well under 50
_RATIO_FLOOR = 1024 * 1024  # tiny files can legitimately compress extremely well
_DRIVE = re.compile(r'^[A-Za-z]:')


def clean_name(raw):
    """Turn an archive name into a safe relative POSIX path, or raise UnsafeArchive."""
    if '\x00' in raw:
        raise UnsafeArchive('NUL byte in name')
    # WHY: Valve's zips use backslashes as separators ('steamui\\css/'). On Linux a backslash is a
    # legal filename character, so we convert it, and we do so BEFORE the traversal check so that
    # '..\\..\\x' cannot hide.
    name = raw.replace('\\', '/')
    if name.startswith('/') or _DRIVE.match(name):
        raise UnsafeArchive(f'absolute path: {raw!r}')
    parts = [p for p in name.split('/') if p not in ('',)]
    if not parts:
        raise UnsafeArchive(f'empty name: {raw!r}')
    for p in parts:
        if p in ('.', '..'):
            raise UnsafeArchive(f'traversal component in {raw!r}')
    return '/'.join(parts)


def safe_extract(zip_path, dest, only=None, max_entries=MAX_ENTRIES, max_entry_bytes=MAX_ENTRY_BYTES,
                 max_total_bytes=MAX_TOTAL_BYTES, max_ratio=MAX_RATIO):
    """Extract into the existing, empty directory `dest`. `only` (set of cleaned names) limits what is
    written, but every entry is still validated. Returns the list of cleaned names written."""
    dest = Path(dest).resolve()
    with zipfile.ZipFile(zip_path) as z:
        infos = z.infolist()
        if len(infos) > max_entries:
            raise UnsafeArchive(f'too many entries: {len(infos)}')
        plan = []
        seen = {}
        total = 0
        for info in infos:
            is_dir = info.filename.endswith(('/', '\\'))
            name = clean_name(info.filename)
            mode = info.external_attr >> 16
            # WHY allow type 0: archives written by plain tools carry permission bits with no file-type
            # bits; only an explicit non-file, non-directory type (symlink, device, fifo) is refused.
            if stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR):
                raise UnsafeArchive(f'special file (link/device) refused: {info.filename!r}')
            if name in seen:
                raise UnsafeArchive(f'duplicate name after cleaning: {name!r}')
            seen[name] = is_dir
            if not is_dir:
                if info.file_size > max_entry_bytes:
                    raise UnsafeArchive(f'entry too large: {name}')
                total += info.file_size
                if total > max_total_bytes:
                    raise UnsafeArchive('archive too large when unpacked')
                if info.file_size > _RATIO_FLOOR and info.file_size > max_ratio * max(info.compress_size, 1):
                    raise UnsafeArchive(f'suspicious compression ratio: {name}')
            plan.append((info, name, is_dir))
        # A file that is also used as a directory ('a' and 'a/b') is a trick, not a real bundle.
        for name in seen:
            parent = name.rpartition('/')[0]
            while parent:
                if parent in seen and not seen[parent]:
                    raise UnsafeArchive(f'file used as directory: {parent!r}')
                parent = parent.rpartition('/')[0]
        for _, name, _ in plan:
            target = (dest / name).resolve()
            if dest not in target.parents and target != dest:
                raise UnsafeArchive(f'escapes destination: {name!r}')

        written = []
        for info, name, is_dir in plan:
            if only is not None and name not in only:
                continue
            target = dest / name
            if is_dir:
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            n = 0
            with z.open(info) as src, open(target, 'wb') as out:
                while True:
                    chunk = src.read(1 << 20)
                    if not chunk:
                        break
                    n += len(chunk)
                    # WHY compare to the declared size too: a header that under-reports is lying.
                    if n > max_entry_bytes or n > info.file_size:
                        raise UnsafeArchive(f'entry inflated past its declared size: {name}')
                    out.write(chunk)
            target.chmod(0o644)  # never executable, whatever the archive says
            written.append(name)
        return written
