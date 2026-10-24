# SPDX-FileCopyrightText: 2026 Destin Moss <https://youcoded.ai>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Open or update the single GitHub tracking issue for the canary (through the `gh` command line)."""
import json

LABEL = 'gaming-canary'
LIMIT = 60000  # GitHub rejects bodies over 65536 characters
FOOTER = ('\n\nDo not assume users are broken: devices fail safe to stock. '
          'Fix and ship a normal update.')


def sync_issue(gh, title, body):
    """gh(args, input=None) -> stdout. Creates the issue if no open one has this exact title,
    otherwise adds a comment. The body travels on stdin so nothing from Valve's files is ever
    placed on a command line."""
    if len(body) > LIMIT:
        body = body[:LIMIT] + '\n\n(truncated)'
    found = json.loads(gh(['issue', 'list', '--label', LABEL, '--state', 'open',
                           '--json', 'number,title', '--limit', '50']) or '[]')
    for issue in found:
        if issue['title'] == title:
            gh(['issue', 'comment', str(issue['number']), '--body-file', '-'], input=body)
            return issue['number']
    gh(['label', 'create', LABEL, '--force', '--description', 'Steam UI canary for Gaming Mode',
        '--color', 'B60205'])
    gh(['issue', 'create', '--title', title, '--label', LABEL, '--body-file', '-'], input=body)
    return None
