#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: MIT

"""Release tags reuse the packages main already built and tested for the exact same commit (card #200).

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
PRODUCERS = ('prepare', 'arch', 'fedora', 'fedora-arm64', 'flatpak', 'steamos')
ARTIFACTS = {t + suffix for t in ('source', 'arch', 'fedora', 'fedora-arm64', 'flatpak', 'steamos')
             for suffix in ('-files', '-evidence')}


def api(path):
    return json.loads(subprocess.check_output(['gh', 'api', path], text=True))


def candidates(runs, sha, repo):
    """Main-branch push runs of this very workflow for this very commit, in this repository (never a fork)."""
    return [r for r in runs
            if r.get('head_sha') == sha and r.get('head_branch') == 'main' and r.get('event') == 'push'
            and r.get('path') == WORKFLOW and (r.get('repository') or {}).get('full_name') == repo
            and (r.get('head_repository') or {}).get('full_name') == repo]


def choose(runs, sha, repo):
    """('reuse', run) | ('wait', run) | ('cold', reason). Newest matching run first."""
    matching = sorted(candidates(runs, sha, repo), key=lambda r: r.get('run_number', 0), reverse=True)
    if not matching:
        return 'cold', 'no main push run for this commit'
    for run in matching:
        if run.get('status') == 'completed' and run.get('conclusion') == 'success':
            return 'reuse', run
    pending = [r for r in matching if r.get('status') != 'completed']
    if pending:
        return 'wait', pending[0]
    return 'cold', 'the main run for this commit did not succeed'


def producer_statuses(jobs):
    """Gate input from that run's jobs: every producer exactly once, as {'job': {'result': conclusion}}."""
    out = {}
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
        return 'a producer in the main run did not succeed'
    artifacts = {a['name']: a for a in api(f'repos/{repo}/actions/runs/{run_id}/artifacts?per_page=100')['artifacts']}
    missing = sorted(n for n in ARTIFACTS if n not in artifacts or artifacts[n].get('expired'))
    return 'main run files missing or expired: ' + ', '.join(missing) if missing else ''


def find(env, wait_minutes=75, poll=30):
    sha, repo, ref = env['GITHUB_SHA'], env['GITHUB_REPOSITORY'], env.get('GITHUB_REF', '')
    if not ref.startswith('refs/tags/'):
        return '', 'not a tag run'
    deadline = time.time() + wait_minutes*60
    while True:
        runs = api(f'repos/{repo}/actions/workflows/packages.yml/runs?head_sha={sha}&event=push&branch=main&per_page=30')
        decision, value = choose(runs['workflow_runs'], sha, repo)
        if decision == 'reuse':
            reason = usable(value['id'], repo)
            return ('', reason) if reason else (str(value['id']), f"reusing main run {value['id']}")
        if decision == 'cold' or time.time() > deadline:
            return '', value if decision == 'cold' else 'the main run for this commit did not finish in time'
        print(f"waiting for main run {value['id']} ({value.get('status')})", file=sys.stderr)
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
