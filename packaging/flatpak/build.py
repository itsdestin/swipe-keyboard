#!/usr/bin/env python3
"""Run only inside the owned builder, with already installed signed pinned runtimes."""
import argparse
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent


def verify_runtime(pins, run=subprocess.check_output):
    # WHY: builder 1.4.12 overwrites manifest sdk-commit/runtime-commit with the
    # installed refs. Explicit preflight prevents silently building against updates.
    for name in ('org.kde.Sdk', 'org.kde.Platform'):
        actual = run(['flatpak', 'info', '--user', '--arch=x86_64', '--show-commit',
                      name + '//' + pins['branch']], text=True).strip()
        if actual != pins[name]:
            raise RuntimeError(f'{name} commit mismatch: expected {pins[name]}, got {actual}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build_directory', type=Path)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--ci-manifest', type=Path, help='Explicit hosted CI entry; default lab guard is unchanged')
    args = parser.parse_args()
    if args.ci_manifest:
        import os
        if os.environ.get('GITHUB_ACTIONS') != 'true':
            raise RuntimeError('CI entry requires GitHub Actions; use the owned builder locally.')
    else:
        marker = Path.home() / '.cache/swipe-platform-168/ownership'
        if not marker.is_file() or marker.read_text().strip() != 'owned guest for work item 168':
            raise RuntimeError('Refusing build outside the owned Task 168 builder.')
        if 'QEMU' not in Path('/sys/class/dmi/id/sys_vendor').read_text():
            raise RuntimeError('Refusing build without QEMU DMI identity.')
    verify_runtime(json.loads((HERE / 'runtime-lock.json').read_text()))
    subprocess.run(['flatpak-builder', '--user', '--jobs=2', '--force-clean',
                    '--disable-rofiles-fuse', '--repo=' + str(args.repo.resolve()),
                    str(args.build_directory.resolve()),
                    str(args.ci_manifest.resolve() if args.ci_manifest else HERE / 'org.itsdestin.SwipeKeyboard.json')], check=True)


if __name__ == '__main__':
    main()
