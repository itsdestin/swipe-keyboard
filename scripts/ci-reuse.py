#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Release tags reuse the packages already built and tested for the exact same commit (card #200).

Change 2026-11-05 (Destin: "fix this stupid broken release process"): besides main's push run, a full run started by
hand on a work branch of this repository counts too (`gh workflow run ... -f full=true`). Every merge makes a new commit,
so waiting for main's own run after the merge added a second ~30-minute build of packages already built and checked.
Tagging the merged branch's last commit (reachable from main through the merge; the gate checks that) reuses its run.
Same commit still means same files, recipes and pinned assets; a fork's run never counts.

Owner decision, 2026-10-24: a tag no longer rebuilds every platform cold (~36 min). It publishes the files of the
successful push run on main for the same commit, after the gate hashes every byte against that run's own recorded
evidence. Same commit means same workflow file, same recipes and same pinned assets. What this trades away: the main
run may restore dependency and compiler caches (scripts/ci-cache.py), which a cold tag build never did. If no such
run exists, the tag builds everything cold exactly as before.

  find      print the run id to reuse, or an empty line for a cold build (reason on stderr). Waits while the
            matching main run is still going, so pushing main and the tag together still reuses.
  statuses  print the producer results of that run, in the gate's needs-JSON shape
"""
import json
import os
import subprocess
import sys
import time

WORKFLOW = '.github/workflows/packages.yml'
# Same producer list as release.py; the gate requires every one of them.
PRODUCERS = ('prepare', 'arch', 'fedora', 'fedora-arm64', 'flatpak', 'steamos', 'distro-packages')
ARTIFACTS = {t + suffix for t in ('source', 'arch', 'fedora', 'fedora-arm64', 'flatpak', 'steamos')
             for suffix in ('-files', '-evidence')}


def api(path, tries=4, pause=15):
    # WHY retries: on 2026-10-30 one GitHub "Server Error (HTTP 502)" on this call stopped the v0.5.1-1 release.
    # A read of run data is safe to repeat; a real fault (wrong run, no access) still fails after the last try.
    for attempt in range(tries):
        try:
            return json.loads(subprocess.check_output(['gh', 'api', path], text=True))
        except subprocess.CalledProcessError:
            if attempt == tries - 1:
                raise
            print(f'gh api {path} failed, retrying in {pause}s', file=sys.stderr)
            time.sleep(pause)


def candidates(runs, sha, repo):
    """Runs of this very workflow for this very commit, in this repository (never a fork): main's push runs, and runs
    started by hand on a branch here (WHY: see the top; a pull request's own runs test a temporary merge commit with
    another SHA, so they can never match)."""
    return [r for r in runs
            if r.get('head_sha') == sha
            and ((r.get('event') == 'push' and r.get('head_branch') == 'main') or r.get('event') == 'workflow_dispatch')
            and r.get('path') == WORKFLOW and (r.get('repository') or {}).get('full_name') == repo
            and (r.get('head_repository') or {}).get('full_name') == repo]


def choose(runs, sha, repo, usable=lambda run: ''):
    """('reuse', run) | ('wait', run) | ('cold', reason). Newest matching run first.

    WHY `usable` per run: a branch run started without full=true succeeds while building nothing; it must be passed
    over for an older full one, not end the search."""
    matching = sorted(candidates(runs, sha, repo), key=lambda r: r.get('run_number', 0), reverse=True)
    if not matching:
        return 'cold', 'no full run for this commit'
    reasons = []
    for run in matching:
        if run.get('status') == 'completed' and run.get('conclusion') == 'success':
            reason = usable(run)
            if not reason:
                return 'reuse', run
            reasons.append(f"run {run.get('id')}: {reason}")
    pending = [r for r in matching if r.get('status') != 'completed']
    if pending:
        return 'wait', pending[0]
    return 'cold', '; '.join(reasons) or 'no run for this commit succeeded'


def producer_statuses(jobs):
    """Gate input from that run's jobs: every producer exactly once, as {'job': {'result': conclusion}}."""
    out = {}
    # WHY combined (card #502): the API lists one job per distro row, "distro-packages (target, image, kind, required)".
    # Like the live run, the producer counts as succeeded when every REQUIRED row succeeded; an optional row (Debian
    # testing, Tumbleweed) may fail, and the release gate then leaves that target out.
    rows = [job for job in jobs if str(job.get('name', '')).startswith('distro-packages (')]
    if rows:
        required = [job for job in rows if str(job.get('name', '')).rstrip(')').split(', ')[-1] == 'true']
        ok = bool(required) and all(job.get('conclusion') == 'success' for job in required)
        out['distro-packages'] = {'result': 'success' if ok else 'failure'}
    for job in jobs:
        name = job.get('name')
        if name in PRODUCERS:
            if name in out:
                raise ValueError('Duplicate producer job in reused run: ' + name)
            out[name] = {'result': job.get('conclusion')}
    if set(out) != set(PRODUCERS):
        raise ValueError('Reused run lacks producer jobs: ' + ', '.join(sorted(set(PRODUCERS) - set(out))))
    return out


def usable(run_id, repo):
    """The run's producers all succeeded and none of the files it made has expired."""
    statuses = producer_statuses(api(f'repos/{repo}/actions/runs/{run_id}/jobs?per_page=100')['jobs'])
    if any(s['result'] != 'success' for s in statuses.values()):
        return 'a producer in that run did not succeed (a fast run builds nothing)'
    artifacts = {a['name']: a for a in api(f'repos/{repo}/actions/runs/{run_id}/artifacts?per_page=100')['artifacts']}
    missing = sorted(n for n in ARTIFACTS if n not in artifacts or artifacts[n].get('expired'))
    return 'run files missing or expired: ' + ', '.join(missing) if missing else ''


def find(env, wait_minutes=75, poll=30):
    sha, repo, ref = env['GITHUB_SHA'], env['GITHUB_REPOSITORY'], env.get('GITHUB_REF', '')
    if not ref.startswith('refs/tags/'):
        return '', 'not a tag run'
    deadline = time.time() + wait_minutes*60
    while True:
        runs = api(f'repos/{repo}/actions/workflows/packages.yml/runs?head_sha={sha}&per_page=30')
        decision, value = choose(runs['workflow_runs'], sha, repo, lambda run: usable(run['id'], repo))
        if decision == 'reuse':
            return str(value['id']), f"reusing run {value['id']} ({value.get('event')} on {value.get('head_branch')})"
        if decision == 'cold' or time.time() > deadline:
            return '', value if decision == 'cold' else 'the run for this commit did not finish in time'
        print(f"waiting for run {value['id']} ({value.get('status')})", file=sys.stderr)
        time.sleep(poll)


def main(argv):
    if argv[1:2] == ['find']:
        run_id, reason = find(os.environ)
        print(('reuse: ' if run_id else 'cold build: ') + reason, file=sys.stderr)
        print(run_id)
    elif argv[1:2] == ['statuses'] and len(argv) == 4 and argv[2] == '--run':
        repo = os.environ['GITHUB_REPOSITORY']
        print(json.dumps(producer_statuses(api(f'repos/{repo}/actions/runs/{argv[3]}/jobs?per_page=100')['jobs'])))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv)
