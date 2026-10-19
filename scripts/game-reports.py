#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""Game reports: the issues a player attached a ScarletNX log to, each summarized by scripts/logscan.py in a comment
of its own and grouped with the reports that failed the same way.

    scripts/game-reports.py triage (--event FILE | --issue N) [--repo OWNER/NAME] [--dry-run]
    scripts/game-reports.py index [--repo OWNER/NAME] [--title-id ID] [--signature TEXT] [--outcome KIND]

triage (.github/workflows/game-reports.yml, FILE its event's JSON) reads the issue and its comments, and does nothing
when none of them has a log, attached (a .log, .txt, .gz or .zip on GitHub's attachment host) or pasted (a fenced
block with a log's header or failure marker in it). Otherwise it scans the newest MAX_LOGS and writes their summary in
a comment of its own, edited rather than repeated as logs are added. The issue gets the `game report` label and a
`log: <how it ended>` label for each log's outcome, and the comment names the other game reports with the same
signature. An attachment that isn't a ScarletNX log is named as such.

--issue N triages that issue as an event for it would. --dry-run reads GitHub as triage does but writes nothing:
the comment and the label changes are printed instead.

index writes each game report as a JSON line: its number, title, state and URL, and each log's name, URL, signature,
outcome, title ID and build, as the summary comments record them (a JSON comment at their end). The filters keep the
reports with a log that matches.

GitHub is reached through gh (GH_TOKEN). Nothing of an issue reaches a shell: the event is read from its file, and
what is posted is logscan's escaped markdown. Downloads are limited to GitHub's attachment URLs and MAX_BYTES.
"""

import argparse
import gzip
import importlib.util
import io
import json
import os
import re
import subprocess
import sys
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('logscan', HERE / 'logscan.py')
logscan = importlib.util.module_from_spec(spec)
spec.loader.exec_module(logscan)

LABEL = 'game report'
OUTCOME_PREFIX = 'log: '
LABEL_COLOR = 'c5def5'
OUTCOME_COLOR = 'ededed'
MARK = '<!-- game-reports:summary -->'
DATA = re.compile(r'<!-- game-reports (\{.*?\}) -->', re.DOTALL)
BOTS = ('github-actions[bot]', 'github-actions')
MAX_LOGS = 3
MAX_BYTES = 64 << 20
MAX_SAME = 10
COMMENT_LIMIT = 65000  # GitHub refuses a comment over 65536 characters

# GitHub's attachment URLs: user-attachments/files/<id>/<name>, and the older <owner>/<repo>/files/<id>/<name>
ATTACHMENT = re.compile(
    r'https://github\.com/(?:user-attachments/files|[\w.-]+/[\w.-]+/files)/\d+/[^\s)\]>"\'<]+\.(?:log|txt|gz|zip)\b',
    re.IGNORECASE,
)


class GitHub:
    """The REST and GraphQL calls triage and index make, through gh"""

    def __init__(self, repo):
        self.repo = repo

    def api(self, path, method='GET', data=None, paginate=False):
        args = ['gh', 'api', '-H', 'Accept: application/vnd.github+json', '-X', method, path]
        if paginate:
            args += ['--paginate', '--slurp']
        if data is not None:
            args += ['--input', '-']
        result = subprocess.run(
            args, input=json.dumps(data) if data is not None else None, capture_output=True, text=True, check=False
        )
        if result.returncode:
            raise RuntimeError(f'gh api {method} {path}: {result.stderr.strip() or result.stdout.strip()}')
        value = json.loads(result.stdout) if result.stdout.strip() else None
        return [item for page in value for item in page] if paginate else value

    def issue(self, number):
        return self.api(f'repos/{self.repo}/issues/{number}')

    def comments(self, number):
        return self.api(f'repos/{self.repo}/issues/{number}/comments?per_page=100', paginate=True)

    def comment(self, number, body, existing=None):
        if existing:
            self.api(f'repos/{self.repo}/issues/comments/{existing}', 'PATCH', {'body': body})
        else:
            self.api(f'repos/{self.repo}/issues/{number}/comments', 'POST', {'body': body})

    def label(self, name, color, description):
        try:
            self.api(f'repos/{self.repo}/labels', 'POST', {'name': name, 'color': color, 'description': description})
        except RuntimeError as error:
            if 'already_exists' not in str(error):
                raise

    def set_labels(self, number, add, remove):
        if add:
            self.api(f'repos/{self.repo}/issues/{number}/labels', 'POST', {'labels': sorted(add)})
        for name in sorted(remove):
            self.api(f'repos/{self.repo}/issues/{number}/labels/{urllib.parse.quote(name, safe="")}', 'DELETE')

    def reports(self):
        """Every issue with the game report label, open or closed, with the bots' comments"""
        owner, name = self.repo.split('/', 1)
        query = (
            'query($owner: String!, $name: String!, $after: String) { repository(owner: $owner, name: $name) {'
            f' issues(first: 50, after: $after, labels: ["{LABEL}"], states: [OPEN, CLOSED]) {{'
            ' pageInfo { hasNextPage endCursor }'
            ' nodes { number title state url comments(first: 50) { nodes { author { login } body } } } } } }'
        )
        after, out = None, []
        while True:
            args = ['graphql', '-f', f'query={query}', '-f', f'owner={owner}', '-f', f'name={name}']
            if after:
                args += ['-f', f'after={after}']
            page = self.graphql(args)['data']['repository']['issues']
            out += page['nodes']
            if not page['pageInfo']['hasNextPage']:
                return out
            after = page['pageInfo']['endCursor']

    def graphql(self, args):
        result = subprocess.run(['gh', 'api', *args], capture_output=True, text=True, check=False)
        if result.returncode:
            raise RuntimeError(f'gh api graphql: {result.stderr.strip()}')
        return json.loads(result.stdout)


class DryRun(GitHub):
    """GitHub's reads, and its writes printed"""

    def comment(self, number, body, existing=None):
        print(f'--- {"edit comment " + str(existing) if existing else "new comment"} on #{number}\n{body}---')

    def label(self, name, color, description):
        pass

    def set_labels(self, number, add, remove):
        print(f'labels of #{number}: add {sorted(add)}, remove {sorted(remove)}')


def download(url):
    """The attachment's bytes, MAX_BYTES at most (GitHub's host redirects to its storage, which urllib follows)"""
    request = urllib.request.Request(url, headers={'User-Agent': 'scarletnx-game-reports'})
    with urllib.request.urlopen(request, timeout=60) as reply:
        data = reply.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        raise ValueError(f'over {MAX_BYTES >> 20} MB')
    return data


def unpack(name, data):
    """The log's text from the attachment: a .gz uncompressed, from a .zip its largest .log (else .txt), MAX_BYTES of
    either at most"""
    if data[:2] == b'\x1f\x8b':
        with gzip.GzipFile(fileobj=io.BytesIO(data)) as f:
            data = f.read(MAX_BYTES + 1)
    elif data[:4] == b'PK\x03\x04':
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            members = [m for m in z.infolist() if not m.is_dir() and m.filename.lower().endswith(('.log', '.txt'))]
            members.sort(key=lambda m: (not m.filename.lower().endswith('.log'), -m.file_size))
            if not members:
                raise ValueError('no .log or .txt in the zip')
            with z.open(members[0]) as f:
                data = f.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        raise ValueError(f'over {MAX_BYTES >> 20} MB uncompressed')
    return data.decode('utf-8', 'replace')


def is_scarletnx_log(r):
    return bool(r['title'] or r['build'] or r['failure'] or r['launcher'])


# a fenced block in an issue or comment holding a log's lines, pasted rather than attached
FENCE = re.compile(r'^ {0,3}(`{3,}|~{3,})[^\n]*\n(.*?)^ {0,3}\1[`~]*[ \t]*$', re.MULTILINE | re.DOTALL)


def pasted_log(block):
    """A block is a log when a line of it is a header or a failure marker, as logscan reads them"""
    lines = [logscan.STAMP.sub('', line) for line in block.splitlines()]
    return any(
        logscan.HEADER.match(line) or any(pattern.search(line) for _, pattern in logscan.FAILURES) for line in lines
    )


def sources(issue, comments):
    """The logs of the issue and its comments, in order, each once, the newest MAX_LOGS: (name, url, attachment URL
    or None, the pasted text or None)"""
    places = [('the issue', issue.get('html_url'), issue.get('body'))]
    places += [(f'a comment by {c["user"]["login"]}', c.get('html_url'), c.get('body')) for c in comments]
    out, seen = [], set()
    for where, link, body in places:
        for url in ATTACHMENT.findall(body or ''):
            if url not in seen:
                seen.add(url)
                out.append((attachment_name(url), url, url, None))
        for m in FENCE.finditer(body or ''):
            if pasted_log(m.group(2)):
                out.append((f'pasted in {where}', link, None, m.group(2)))
    return out[-MAX_LOGS:]


def attachments(texts):
    """The attachment URLs in the texts, in order, each once, the newest MAX_LOGS"""
    return [url for _, _, url, _ in sources({'body': '\n'.join(t or '' for t in texts)}, []) if url]


def attachment_name(url):
    return urllib.parse.unquote(url.rsplit('/', 1)[-1])


def summary_data(body):
    m = DATA.search(body or '')
    if not m:
        return None
    try:
        return json.loads(m.group(1))
    except json.JSONDecodeError:
        return None


def index(github):
    """Each game report and its logs as its summary comment records them"""
    out = []
    for issue in github.reports():
        logs = []
        for c in issue['comments']['nodes']:
            if (c.get('author') or {}).get('login') in BOTS and MARK in c['body']:
                data = summary_data(c['body'])
                logs = data['logs'] if data else []
        out.append(
            {
                'number': issue['number'],
                'title': issue['title'],
                'state': issue['state'].lower(),
                'url': issue['url'],
                'logs': logs,
            }
        )
    return out


def same_signature(reports, number, signature):
    return [
        r for r in reports if r['number'] != number and any(log.get('signature') == signature for log in r['logs'])
    ][:MAX_SAME]


def comment_body(entries, reports, number):
    out = [
        MARK,
        '## Log summary',
        '',
        (
            'Written by `scripts/logscan.py` from the logs attached here, and updated when another is attached. '
            "Paths are shortened to `~` and the crash report's code and memory dumps are left out."
        ),
    ]
    for e in entries:
        out += ['', f'### {logscan.code(e["name"])}', '']
        if e.get('error'):
            out.append(f'Could not be read: {logscan.text(e["error"])}')
            continue
        if not e['report']:
            out.append(
                "This doesn't look like a ScarletNX log. The game's log is `<title>.log` in the game's folder, "
                "and the launcher's Share log writes one with the last run's output."
            )
            continue
        out.append(logscan.markdown(e['name'], e['report'], heading='####').rstrip())
        if e['report']['signature']:
            same = same_signature(reports, number, e['report']['signature'])
            if same:
                refs = ', '.join(f'#{r["number"]}' + (' (closed)' if r['state'] == 'closed' else '') for r in same)
                out += ['', f'**The same signature:** {refs}']
    data = {
        'schema': 1,
        'logs': [
            {
                'name': e['name'],
                'url': e['url'],
                'signature': e['report']['signature'],
                'outcome': e['report']['outcome'],
                'title': e['report']['title'],
                'title_id': e['report']['title_id'],
                'game_version': e['report']['game_version'],
                'commit': e['report']['commit'],
            }
            for e in entries
            if e.get('report')
        ],
    }
    # "--" ends an HTML comment: inside the JSON's strings it is written escaped
    out += ['', '<!-- game-reports ' + json.dumps(data, ensure_ascii=False).replace('--', '-\\u002d') + ' -->']
    body = '\n'.join(out) + '\n'
    if len(body) > COMMENT_LIMIT:
        # the folded parts of each summary go first: the header lines and the signatures stay
        body = re.sub(r'\n<details>.*?</details>', '', body, flags=re.DOTALL)
    return body


def triage(github, event, fetch=download):
    issue = event.get('issue') or {}
    if 'pull_request' in issue or not issue.get('number'):
        return 'not an issue'
    number = issue['number']
    issue = github.issue(number)
    comments = github.comments(number)
    users = [c for c in comments if (c.get('user') or {}).get('login') not in BOTS]
    found = sources(issue, users)
    if not found:
        return 'no log attached'
    entries = []
    for name, link, url, pasted in found:
        entry = {'name': name, 'url': link, 'report': None}
        try:
            r = logscan.scan(pasted if pasted is not None else unpack(name, fetch(url)))
            entry['report'] = r if is_scarletnx_log(r) else None
        except (OSError, ValueError, zipfile.BadZipFile, EOFError) as error:
            entry['error'] = str(error)
        entries.append(entry)
    logscan.demangle_all([e['report'] for e in entries if e['report']])
    body = comment_body(entries, index(github), number)
    existing = next(
        (c['id'] for c in comments if (c.get('user') or {}).get('login') in BOTS and MARK in (c.get('body') or '')),
        None,
    )
    github.comment(number, body, existing)
    have = {label['name'] for label in issue.get('labels', [])}
    outcomes = {OUTCOME_PREFIX + e['report']['outcome'] for e in entries if e['report']}
    if not outcomes:
        return f'#{number}: no ScarletNX log in {len(entries)} attachment(s)'
    want = {LABEL} | outcomes
    github.label(LABEL, LABEL_COLOR, 'An issue with a ScarletNX log attached (scripts/game-reports.py)')
    for name in outcomes - have:
        github.label(name, OUTCOME_COLOR, 'How the attached log says the run ended')
    stale = {name for name in have if name.startswith(OUTCOME_PREFIX)} - outcomes
    github.set_labels(number, want - have, stale)
    return f'#{number}: {len(entries)} log(s), {", ".join(sorted(outcomes))}'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='command', required=True)
    t = sub.add_parser('triage')
    which = t.add_mutually_exclusive_group(required=True)
    which.add_argument('--event')
    which.add_argument('--issue', type=int)
    t.add_argument('--dry-run', action='store_true')
    t.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY'))
    i = sub.add_parser('index')
    i.add_argument('--repo', default=os.environ.get('GITHUB_REPOSITORY', 'scarletnx/ScarletNX'))
    i.add_argument('--title-id')
    i.add_argument('--signature')
    i.add_argument('--outcome')
    args = ap.parse_args()
    if not args.repo:
        sys.exit('game-reports: no --repo and no GITHUB_REPOSITORY')
    github = GitHub(args.repo)
    if args.command == 'triage':
        if args.dry_run:
            github = DryRun(args.repo)
        if args.issue:
            event = {'issue': {'number': args.issue}}
        else:
            with open(args.event, encoding='utf-8') as f:
                event = json.load(f)
        print(triage(github, event))
        return
    for report in index(github):
        logs = [
            log
            for log in report['logs']
            if (not args.title_id or (log.get('title_id') or '').upper() == args.title_id.upper())
            and (not args.signature or log.get('signature') == args.signature)
            and (not args.outcome or log.get('outcome') == args.outcome)
        ]
        if logs or not (args.title_id or args.signature or args.outcome):
            print(json.dumps(report, ensure_ascii=False))


if __name__ == '__main__':
    main()
