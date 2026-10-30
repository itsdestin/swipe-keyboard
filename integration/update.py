#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Automatic updates for the native Arch, Fedora, Kubuntu/Debian and openSUSE packages
(plan: docs/active/plans/2026-10-23-auto-updates.md; Kubuntu/Debian/openSUSE: docs/active/specs/2026-10-30-more-kde-distros.md).

Run once a day as root by swipe-keyboard-update.timer. It only ever updates this one package:
- Fedora: dnf, from the signed repository the package itself installed (/etc/yum.repos.d/swipe-keyboard.repo).
- Arch: the signed arch/latest.json on the update site names the newer package and its SHA256; the file is checked
  against both, then installed with pacman -U. Nothing else on the system is synced or upgraded.
- Kubuntu/Debian: apt, refreshing only the signed source the package installed (/etc/apt/sources.list.d/swipe-keyboard.sources).
- openSUSE: zypper, from the signed repository the package installed (/etc/zypp/repos.d/swipe-keyboard.repo).

Also the `swipe-keyboard updates [status|on|off|now]` command.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = Path(__file__).resolve().parent


def setting(name, default):
    # WHY only in test mode: this runs as root, and a stray environment variable (sudo -E) must never be able to point
    # it at another site or key. The tests and the rehearsal set SWIPEKB_UPDATE_TESTING=1 explicitly.
    if os.environ.get('SWIPEKB_UPDATE_TESTING') == '1':
        return os.environ.get(name, default)
    return default


# WHY two sites, the new one first (Destin, 2026-10-24, card #501: the project is being renamed to OpenSwipe): GitHub
# does not forward a project's web address after a rename, so the old address dies at the rename. This version already
# knows the new one, and falls back to the old one until the rename happens. Same key, same checks, on both.
# (In test mode SWIPEKB_UPDATE_SITE may list several, separated by spaces, to prove the fallback.)
SITES = tuple(str(site).rstrip('/') for site in (
    str(setting('SWIPEKB_UPDATE_SITE', '')).split()
    or ('https://itsdestin.github.io/openswipe', 'https://itsdestin.github.io/swipe-keyboard')))
KEYRING = Path(setting('SWIPEKB_UPDATE_KEYRING', HERE / 'update-key.gpg'))
OFF_FLAG = Path(setting('SWIPEKB_UPDATE_OFF_FLAG', '/etc/swipe-keyboard/auto-update-off'))
PACKAGE = 'swipe-keyboard'
APT_SOURCE = Path(setting('SWIPEKB_UPDATE_APT_SOURCE', '/etc/apt/sources.list.d/swipe-keyboard.sources'))
ZYPPER_REPO = 'swipe-keyboard'
# WHY a fixed prefix: even a correctly signed index must not be able to send the updater to another site.
# WHY both names: after the rename the signed index names openswipe downloads; before it, swipe-keyboard ones.
RELEASE_DOWNLOADS = ((setting('SWIPEKB_UPDATE_DOWNLOADS', ''),) if setting('SWIPEKB_UPDATE_DOWNLOADS', '')
                     else ('https://github.com/itsdestin/openswipe/releases/download/',
                           'https://github.com/itsdestin/swipe-keyboard/releases/download/'))
MAX_PACKAGE_BYTES = 200 * 1024 * 1024
# WHY a whole-download deadline: a socket timeout alone lets a slow server drip bytes for hours and hold the update.
FETCH_DEADLINE_SECONDS = 15 * 60


class UpdateError(RuntimeError):
    pass


class Unreachable(Exception):
    """The site could not be reached (offline, down): not an error, just try tomorrow."""


def run(command, **kwargs):
    return subprocess.run(command, text=True, capture_output=True, **kwargs)


def fetch(url, limit):
    deadline = time.monotonic() + FETCH_DEADLINE_SECONDS
    chunks, size = [], 0
    try:
        with urllib.request.urlopen(url, timeout=60) as response:
            while True:
                # WHY read1: it returns whatever has arrived, so the deadline is checked even when a server drips bytes.
                chunk = response.read1(64 * 1024)
                if not chunk:
                    break
                size += len(chunk)
                if size > limit:
                    raise UpdateError(f'{url} is larger than expected')
                if time.monotonic() > deadline:
                    raise Unreachable(f'{url} is downloading too slowly')
                chunks.append(chunk)
    except urllib.error.HTTPError as error:
        # WHY 404/403 and 5xx are quiet: before the first site is published, and while Pages redeploys, there is simply
        # nothing to fetch. Other answers mean the site is wrong, which is worth an error.
        if error.code >= 500 or error.code in (403, 404):
            raise Unreachable(f'{url}: the update site answered {error.code}') from error
        raise UpdateError(f'{url}: the update site answered {error.code}') from error
    except urllib.error.URLError as error:
        # WHY loud: a certificate failure is not "offline" (it can be someone in the middle, or a broken clock); hiding
        # it would leave the machine silently un-updated for good.
        if isinstance(error.reason, ssl.SSLError):
            raise UpdateError(f'{url}: secure connection failed ({error.reason})') from error
        raise Unreachable(f'{url}: {error}') from error
    except (TimeoutError, ConnectionError) as error:
        raise Unreachable(f'{url}: {error}') from error
    return b''.join(chunks)


def manager():
    """Which installer owns this keyboard: 'pacman', 'dnf', 'apt', 'zypper', or None (for example a developer build)."""
    if shutil.which('pacman') and run(['pacman', '-Q', PACKAGE]).returncode == 0:
        return 'pacman'
    if shutil.which('dnf') and shutil.which('rpm') and run(['rpm', '-q', PACKAGE]).returncode == 0:
        return 'dnf'
    # WHY the status text: dpkg-query also answers for a package that was removed but whose settings were kept.
    if (shutil.which('apt-get') and shutil.which('dpkg-query')
            and run(['dpkg-query', '-W', '-f=${Status}', PACKAGE]).stdout.strip() == 'install ok installed'):
        return 'apt'
    if shutil.which('zypper') and shutil.which('rpm') and run(['rpm', '-q', PACKAGE]).returncode == 0:
        return 'zypper'
    return None


def installed_pacman_version():
    out = run(['pacman', '-Q', PACKAGE], check=True).stdout.split()
    return out[1]


def newer(candidate, installed):
    # WHY vercmp: pacman's own version order (epochs, pkgrel), so we never "update" to an older or equal package.
    return int(run(['vercmp', candidate, installed], check=True).stdout.strip()) > 0


def verified_index(work):
    """Download arch/latest.json and refuse it unless the update key signed it."""
    index, signature = work / 'latest.json', work / 'latest.json.sig'
    unreachable = None
    for site in SITES:
        # WHY only "unreachable" moves on to the next site: a site that answers with a bad or unsigned index is refused
        # outright below, never quietly swapped for another one.
        try:
            index.write_bytes(fetch(f'{site}/arch/latest.json', 64 * 1024))
            signature.write_bytes(fetch(f'{site}/arch/latest.json.sig', 64 * 1024))
            break
        except Unreachable as error:
            unreachable = error
    else:
        raise unreachable
    if not KEYRING.is_file():
        raise UpdateError(f'update key missing: {KEYRING}')
    check = run(['gpgv', '--keyring', str(KEYRING), str(signature), str(index)])
    if check.returncode != 0:
        raise UpdateError('the update index is not signed by the Swipe Keyboard update key; nothing was installed')
    data = json.loads(index.read_text())
    if data.get('schema') != 1 or data.get('package') != PACKAGE:
        raise UpdateError('the update index is not for this package')
    return data


def update_pacman():
    with tempfile.TemporaryDirectory(prefix='swipekb-update-') as tmp:
        work = Path(tmp)
        index = verified_index(work)
        installed = installed_pacman_version()
        version = index.get('version', '')
        if not newer(version, installed):
            print(f'Swipe Keyboard {installed} is up to date.')
            return 0
        entry = index.get('files', {}).get(platform.machine())
        if not entry:
            print(f'No {platform.machine()} package in update {version}; staying on {installed}.')
            return 0
        url, sha256, size = str(entry.get('url', '')), str(entry.get('sha256', '')), entry.get('size', 0)
        if not isinstance(size, int):
            raise UpdateError('the update index is malformed; nothing was installed')
        name = url.rsplit('/', 1)[-1]
        if (not url.startswith(RELEASE_DOWNLOADS) or not name.startswith(f'{PACKAGE}-{version}-')
                or not name.endswith('.pkg.tar.zst') or '/' in name or not 0 < size <= MAX_PACKAGE_BYTES):
            raise UpdateError('the update index names an unexpected file; nothing was installed')
        package = work / name
        data = fetch(url, MAX_PACKAGE_BYTES)
        if len(data) != size or hashlib.sha256(data).hexdigest() != sha256:
            raise UpdateError('the downloaded package does not match the signed index; nothing was installed')
        package.write_bytes(data)
        # WHY -U of this one file and --needed: install exactly what was checked, never a system-wide sync.
        result = run(['pacman', '-U', '--noconfirm', '--needed', str(package)])
        if result.returncode != 0:
            hint = ''
            if 'signature' in result.stderr.lower():
                # WHY say so: the package file is checked against the signed index, not signed itself, so a pacman.conf
                # with LocalFileSigLevel = Required refuses it every day.
                hint = ('\nYour pacman.conf requires signed local packages (LocalFileSigLevel). Update by hand from '
                        'https://github.com/itsdestin/swipe-keyboard/releases or run "sudo swipe-keyboard updates off".')
            raise UpdateError('pacman could not install the update (it will be tried again tomorrow):\n'
                              + result.stderr.strip() + hint)
        print(f'Swipe Keyboard updated from {installed} to {version}. It takes effect at your next login.')
        return 0


def update_dnf():
    # WHY only our package: dnf may pull a needed dependency, but never upgrades the rest of the system here.
    result = run(['dnf', '-y', 'upgrade', '--refresh', PACKAGE])
    if result.returncode != 0:
        raise UpdateError('dnf could not update Swipe Keyboard (it will be tried again tomorrow):\n' + result.stderr.strip())
    print(result.stdout.strip())
    return 0


# WHY these phrases: apt reports "the site cannot be reached" and "the site sent something unsigned or wrongly signed"
# in the same exit code. Only the first may be shrugged off until tomorrow; the second must show as a failure.
APT_UNREACHABLE = ('Could not resolve', 'Temporary failure resolving', 'Could not connect', 'Connection timed out',
                   'Network is unreachable', 'No route to host')
APT_UNTRUSTED = ('NO_PUBKEY', 'is not signed', "couldn't be verified", 'BADSIG', 'EXPKEYSIG', 'Signed-By')


def update_apt():
    if not APT_SOURCE.is_file():
        raise UpdateError(f'the update source is missing ({APT_SOURCE}); reinstall the package to restore it')
    env = {**os.environ, 'DEBIAN_FRONTEND': 'noninteractive'}
    # WHY only our source: refreshing every source would be a system-wide "apt update" the owner did not ask for;
    # List-Cleanup=0 keeps the other sources' lists exactly as they were.
    refresh = run(['apt-get', 'update', '-q', '-o', f'Dir::Etc::sourcelist={APT_SOURCE}', '-o', 'Dir::Etc::sourceparts=-',
                   '-o', 'APT::Get::List-Cleanup=0', '-o', 'APT::Update::Error-Mode=any'], env=env)
    text = refresh.stdout + refresh.stderr
    if refresh.returncode != 0:
        if any(phrase in text for phrase in APT_UNTRUSTED):
            raise UpdateError('the update source is not signed by the Swipe Keyboard update key; nothing was installed:\n'
                              + text.strip())
        if any(phrase in text for phrase in APT_UNREACHABLE):
            raise Unreachable('apt could not reach the update site')
        raise UpdateError('apt could not read the update source (it will be tried again tomorrow):\n' + text.strip())
    # WHY --only-upgrade of our package: it may pull a needed dependency, but never upgrades the rest of the system.
    # WHY confold: a setting file the owner changed (the source file itself) is kept, never overwritten unasked.
    result = run(['apt-get', 'install', '-y', '-q', '--only-upgrade', '-o', 'Dpkg::Options::=--force-confold', PACKAGE],
                 env=env)
    if result.returncode != 0:
        raise UpdateError('apt could not update Swipe Keyboard (it will be tried again tomorrow):\n' + result.stderr.strip())
    print(result.stdout.strip())
    return 0


def update_zypper():
    # WHY auto-import here: the key zypper is told to trust is the one inside this package (gpgkey=file:// in the repo
    # file), not one fetched from the site; zypper still refuses an index that key did not sign.
    refresh = run(['zypper', '--non-interactive', '--gpg-auto-import-keys', 'refresh', ZYPPER_REPO])
    if refresh.returncode != 0:
        text = refresh.stdout + refresh.stderr
        if 'Download (curl) error' in text or 'Could not resolve host' in text or 'Timeout' in text:
            raise Unreachable('zypper could not reach the update site')
        raise UpdateError('zypper could not read the update source (it will be tried again tomorrow):\n' + text.strip())
    # WHY only our package, from our repository: never a system-wide update.
    result = run(['zypper', '--non-interactive', 'update', '--repo', ZYPPER_REPO, PACKAGE])
    if result.returncode != 0:
        raise UpdateError('zypper could not update Swipe Keyboard (it will be tried again tomorrow):\n'
                          + result.stderr.strip())
    print(result.stdout.strip())
    return 0


UPDATERS = {'pacman': lambda: update_pacman(), 'dnf': lambda: update_dnf(), 'apt': lambda: update_apt(),
            'zypper': lambda: update_zypper()}


def check_now():
    if OFF_FLAG.exists():
        print('Automatic updates are off. Turn them on with: sudo swipe-keyboard updates on')
        return 0
    owner = manager()
    if owner is None:
        print('This copy of Swipe Keyboard was not installed by pacman, dnf, apt or zypper; it is not updated automatically.')
        return 0
    return UPDATERS[owner]()


def set_enabled(enabled):
    if os.geteuid() != 0:
        raise UpdateError('changing this needs administrator rights: run it with sudo')
    if enabled:
        OFF_FLAG.unlink(missing_ok=True)
        print('Automatic updates are on. Swipe Keyboard checks for a new version once a day.')
    else:
        OFF_FLAG.parent.mkdir(parents=True, exist_ok=True)
        OFF_FLAG.write_text('Swipe Keyboard automatic updates are off. Delete this file (or run '
                            '"sudo swipe-keyboard updates on") to turn them back on.\n')
        print('Automatic updates are off. Update by hand from https://github.com/itsdestin/swipe-keyboard/releases')
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(prog='swipe-keyboard updates', description='Swipe Keyboard automatic updates')
    parser.add_argument('action', nargs='?', default='status', choices=('status', 'on', 'off', 'now', 'check'))
    args = parser.parse_args(argv)
    try:
        if args.action == 'status':
            # WHY ask the package manager first: a copy pacman or dnf did not install (KDE Linux's system extension,
            # a developer build) is never updated, whatever the setting says.
            if manager() is None:
                print('This copy of Swipe Keyboard was not installed by pacman, dnf, apt or zypper; it is not updated automatically.')
                return 0
            print('Automatic updates are ' + ('off.' if OFF_FLAG.exists() else 'on (checked once a day).'))
            return 0
        if args.action in ('on', 'off'):
            return set_enabled(args.action == 'on')
        # 'check' is what the daily timer runs; 'now' is the same, by hand.
        if os.geteuid() != 0:
            raise UpdateError('installing an update needs administrator rights: run it with sudo')
        return check_now()
    except Unreachable as error:
        # WHY success: being offline is normal; a failed unit every day would hide the failures that matter.
        print(f'Swipe Keyboard update: the update site is not reachable right now ({error}); trying again tomorrow.')
        return 0
    except (UpdateError, OSError, ValueError, TypeError, AttributeError, KeyError,
            subprocess.CalledProcessError) as error:
        print('Swipe Keyboard update: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
