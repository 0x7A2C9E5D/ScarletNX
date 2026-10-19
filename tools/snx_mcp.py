#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""An MCP server (stdio) over ScarletNX's report and debugging tools, for any agent that speaks MCP: the game reports
on GitHub and their logs (scripts/game-reports.py, scripts/logscan.py), and the local game folders the logs'
addresses are read against (tools/symbols.py, tools/disasm.py). .mcp.json at the repository's root starts it.

    python3 tools/snx_mcp.py

The tools:
- list_reports: the game reports, filtered by title ID, signature or outcome, with each log's summary data.
- get_report: one issue: its text, its logs (attached or pasted) and the summary comment's data.
- fetch_log: one log of an issue, saved under the reports cache, with logscan's summary and the local path.
- scan_log: logscan's summary of a local log.
- read_log: parts of a log, by regular expression (with context) or by line range, never the whole of a large one.
- find_game: the extracted games under work/ (or SNX_WORK) with a title ID, and their versions.
- symbolize and disassemble: tools/symbols.py and tools/disasm.py on a game, named by its folder or its title ID.

Only the logs and game folders of this machine are read: read_log takes files under the reports cache, the
repository and ScarletNX's own data and state folders. GitHub is reached through gh (its own login) and only by the
report tools. A game's code is disassembled for the agent's context, the way a session uses tools/disasm.py, and
goes nowhere else. Plain JSON-RPC over stdin and stdout, a message a line, with no package beyond Python's own.

SNX_REPORTS_REPO names the repository (scarletnx/ScarletNX), SNX_REPORTS_CACHE the cache (~/.cache/scarletnx/reports),
SNX_WORK the extracted games' folder (work/ of the repository).
"""

import importlib.util
import json
import os
import re
import struct
import subprocess
import sys
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROTOCOL = '2025-06-18'
MAX_TEXT = 40000  # what one answer carries at most: the agent's context is the scarce part
MAX_LINES = 400
LINE = 400


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


logscan = load('logscan', ROOT / 'scripts' / 'logscan.py')
reports = load('game_reports', ROOT / 'scripts' / 'game-reports.py')


def repo():
    return os.environ.get('SNX_REPORTS_REPO', 'scarletnx/ScarletNX')


def cache():
    base = os.environ.get('SNX_REPORTS_CACHE')
    if base:
        return Path(base)
    xdg = os.environ.get('XDG_CACHE_HOME')
    return (Path(xdg) if xdg and xdg.startswith('/') else Path.home() / '.cache') / 'scarletnx' / 'reports'


def work():
    return Path(os.environ.get('SNX_WORK', ROOT / 'work'))


def readable_roots():
    home = Path.home()
    roots = [cache(), ROOT, work()]
    for var, fallback in (('XDG_STATE_HOME', '.local/state'), ('XDG_DATA_HOME', '.local/share')):
        base = os.environ.get(var)
        roots.append((Path(base) if base and base.startswith('/') else home / fallback) / 'scarletnx')
    roots.append(home / 'Library' / 'Application Support' / 'scarletnx')
    if os.environ.get('APPDATA'):
        roots.append(Path(os.environ['APPDATA']) / 'scarletnx')
    return [r.resolve() for r in roots]


def readable(path):
    full = Path(path).expanduser().resolve()
    if not any(full.is_relative_to(root) for root in readable_roots()):
        raise ToolError(
            f'{path} is outside the folders read_log reads (the reports cache, the repository, ScarletNX data)'
        )
    if not full.is_file():
        raise ToolError(f'{path} is not a file')
    return full


class ToolError(Exception):
    pass


def clip(text):
    return text if len(text) <= MAX_TEXT else text[:MAX_TEXT] + f'\n... (cut at {MAX_TEXT} characters)'


def brief(r):
    """logscan's summary without the long parts: what an agent decides from"""
    out = {k: v for k, v in r.items() if k not in ('tail', 'warnings', 'game_output')}
    out['warnings'] = [w['message'] for w in r['warnings'][:10]]
    out['game_output'] = r['game_output'][-5:]
    return out


# ---- the report tools


def list_reports(title_id=None, signature=None, outcome=None):
    out = []
    for report in reports.index(reports.GitHub(repo())):
        logs = [
            log
            for log in report['logs']
            if (not title_id or (log.get('title_id') or '').upper() == title_id.upper())
            and (not signature or log.get('signature') == signature)
            and (not outcome or log.get('outcome') == outcome)
        ]
        if logs or not (title_id or signature or outcome):
            out.append(dict(report, logs=logs or report['logs']))
    return out


def report_logs(github, number):
    issue = github.issue(number)
    comments = github.comments(number)
    users = [c for c in comments if (c.get('user') or {}).get('login') not in reports.BOTS]
    summary = next(
        (
            reports.summary_data(c['body'])
            for c in comments
            if (c.get('user') or {}).get('login') in reports.BOTS and reports.MARK in (c.get('body') or '')
        ),
        None,
    )
    return issue, reports.sources(issue, users), summary


def get_report(issue):
    github = reports.GitHub(repo())
    data, found, summary = report_logs(github, int(issue))
    body = data.get('body') or ''
    return {
        'number': data['number'],
        'title': data['title'],
        'state': data['state'],
        'url': data.get('html_url'),
        'labels': [label['name'] for label in data.get('labels', [])],
        'body': body if len(body) <= 6000 else body[:6000] + '\n... (cut)',
        'logs': [{'index': i, 'name': name, 'url': link} for i, (name, link, _, _) in enumerate(found)],
        'summary': summary,
    }


def fetch_log(issue, index=None, name=None):
    github = reports.GitHub(repo())
    number = int(issue)
    _, found, _ = report_logs(github, number)
    if not found:
        raise ToolError(f'#{number} has no log attached or pasted')
    if name is not None:
        picked = [f for f in found if f[0] == name]
        if not picked:
            raise ToolError(f'#{number} has no log named {name!r}: {[f[0] for f in found]}')
        entry = picked[0]
    else:
        i = int(index) if index is not None else len(found) - 1
        if not 0 <= i < len(found):
            raise ToolError(f'#{number} has {len(found)} log(s), indexes 0 to {len(found) - 1}')
        entry = found[i]
    log_name, link, url, pasted = entry
    text = pasted if pasted is not None else reports.unpack(log_name, reports.download(url))
    folder = cache() / str(number)
    folder.mkdir(parents=True, exist_ok=True)
    safe = re.sub(r'[^\w.-]+', '_', log_name).strip('._') or 'log'
    path = folder / (safe if safe.endswith(('.log', '.txt')) else safe + '.log')
    path.write_text(text, encoding='utf-8')
    r = logscan.scan(text)
    logscan.demangle_all([r])
    return {'path': str(path), 'lines': len(text.splitlines()), 'source': link, 'summary': brief(r)}


def scan_log(path):
    full = Path(path).expanduser()
    if not full.is_file():
        raise ToolError(f'{path} is not a file')
    r = logscan.scan(logscan.read_log(str(full)))
    logscan.demangle_all([r])
    return brief(r) | {'tail': r['tail'], 'warnings': r['warnings']}


def read_log(path, pattern=None, context=2, start=None, count=100):
    full = readable(path)
    lines = logscan.read_log(str(full)).splitlines()
    shown = []
    if pattern:
        try:
            regex = re.compile(pattern)
        except re.error as error:
            raise ToolError(f'pattern: {error}') from None
        keep = set()
        context = max(0, min(int(context), 20))
        for i, line in enumerate(lines):
            if regex.search(line):
                keep.update(range(max(0, i - context), min(len(lines), i + context + 1)))
        shown = sorted(keep)
    else:
        first = int(start) - 1 if start is not None else max(0, len(lines) - int(count))
        shown = list(range(max(0, first), min(len(lines), max(0, first) + min(int(count), MAX_LINES))))
    out, last = [], None
    for i in shown[:MAX_LINES]:
        if last is not None and i != last + 1:
            out.append('--')
        line = lines[i] if len(lines[i]) <= LINE else lines[i][:LINE] + '...'
        out.append(f'{i + 1}: {line}')
        last = i
    more = f'\n({len(shown) - MAX_LINES} more lines: narrow the pattern)' if len(shown) > MAX_LINES else ''
    return f'{full} has {len(lines)} lines\n' + '\n'.join(out) + more


# ---- the local game tools


def npdm_title_id(folder):
    """The program ID in an extracted game's main.npdm (its ACI0's), where the loader finds the ExeFS"""
    for rel in ('exefs/main.npdm', 'ExeFS/main.npdm', 'main.npdm'):
        path = folder / rel
        if path.is_file():
            data = path.read_bytes()
            if data[:4] != b'META' or len(data) < 0x80:
                return None
            off = struct.unpack_from('<I', data, 0x70)[0]
            if data[off : off + 4] != b'ACI0':
                return None
            return f'{struct.unpack_from("<Q", data, off + 0x10)[0]:016X}'
    return None


def nacp_version(folder):
    path = folder / 'control.nacp'
    if not path.is_file():
        return None
    return path.read_bytes()[0x3060:0x3070].split(b'\0')[0].decode('utf-8', 'replace') or None


def find_game(title_id=None):
    out = []
    base = work()
    if not base.is_dir():
        return out
    for folder in sorted(p for p in base.iterdir() if p.is_dir()):
        tid = npdm_title_id(folder)
        if tid and (not title_id or tid == title_id.upper()):
            out.append({'path': str(folder), 'title_id': tid, 'version': nacp_version(folder)})
    return out


def game_folder(game):
    if re.fullmatch(r'[0-9A-Fa-f]{16}', game):
        found = find_game(game)
        if not found:
            raise ToolError(f'no extracted game with title ID {game} under {work()}')
        return found[0]['path']
    path = Path(game).expanduser()
    if not path.is_dir() and (work() / game).is_dir():
        path = work() / game
    if not path.is_dir():
        raise ToolError(f'{game} is neither a folder, a folder of {work()}, nor a title ID')
    return str(path)


def run_tool(script, args):
    result = subprocess.run(
        [sys.executable, str(ROOT / 'tools' / script), *args],
        capture_output=True,
        text=True,
        timeout=600,
        check=False,
        cwd=ROOT,
    )
    lines = (result.stdout + (result.stderr if result.returncode else '')).splitlines()
    out = '\n'.join(lines[:MAX_LINES])
    if len(lines) > MAX_LINES:
        out += f'\n... ({len(lines) - MAX_LINES} more lines)'
    if result.returncode:
        raise ToolError(f'{script} exited {result.returncode}:\n{out}')
    return out


def symbolize(game, addresses):
    if not addresses or not all(re.fullmatch(r'(0x)?[0-9A-Fa-f]+', a) for a in addresses):
        raise ToolError('addresses: hexadecimal game addresses, as crash reports print them')
    return run_tool('symbols.py', [game_folder(game), *addresses])


def disassemble(game, targets):
    if not targets or any(t.startswith('-') for t in targets):
        raise ToolError('targets: function names (substrings) or hexadecimal addresses')
    return run_tool('disasm.py', [game_folder(game), *targets])


STRING = {'type': 'string'}
TOOLS = [
    (
        'list_reports',
        list_reports,
        (
            "The game reports on GitHub (issues with a ScarletNX log), each with its logs' signature, outcome, title ID, "
            'game version and build. Filter by title_id, an exact signature (to find the same failure in other reports) '
            'or outcome (crash, svcBreak, exception, brk, unsupported instruction, fatal, no failure logged).'
        ),
        {'title_id': STRING, 'signature': STRING, 'outcome': STRING},
        [],
    ),
    (
        'get_report',
        get_report,
        (
            'One issue: its title, state, labels, text, the logs attached or pasted in it (index and name) and the '
            "summary comment's data."
        ),
        {'issue': {'type': 'integer'}},
        ['issue'],
    ),
    (
        'fetch_log',
        fetch_log,
        (
            'Downloads one log of an issue (the newest unless index or name says) into the reports cache and returns its '
            'local path and logscan summary. Read its parts with read_log.'
        ),
        {'issue': {'type': 'integer'}, 'index': {'type': 'integer'}, 'name': STRING},
        ['issue'],
    ),
    (
        'scan_log',
        scan_log,
        (
            "logscan's summary of a local log or the launcher's Share log file: build, title ID, version, GPU, how the "
            'run ended, the game function it is pinned on, its backtrace, unimplemented service commands, signature.'
        ),
        {'path': STRING},
        ['path'],
    ),
    (
        'read_log',
        read_log,
        (
            'Parts of a log, with line numbers: the lines matching pattern (a Python regular expression) with context '
            'lines around each, or count lines from start (1-based), or the last count lines. At most 400 lines.'
        ),
        {
            'path': STRING,
            'pattern': STRING,
            'context': {'type': 'integer'},
            'start': {'type': 'integer'},
            'count': {'type': 'integer'},
        },
        ['path'],
    ),
    (
        'find_game',
        find_game,
        (
            'The extracted games on this machine (work/ of the repository, or SNX_WORK), with their title IDs and '
            "versions; title_id keeps one. A report's addresses only match a game of the same version."
        ),
        {'title_id': STRING},
        [],
    ),
    (
        'symbolize',
        symbolize,
        (
            "Names game addresses (from a log's backtrace or crash report) with the game's symbols. game is a folder, a "
            'folder name under work/, or a title ID.'
        ),
        {'game': STRING, 'addresses': {'type': 'array', 'items': STRING}},
        ['game', 'addresses'],
    ),
    (
        'disassemble',
        disassemble,
        (
            'Disassembles game functions, named by substrings of their symbols or by hexadecimal addresses. game as for '
            'symbolize. Long functions are cut at 400 lines.'
        ),
        {'game': STRING, 'targets': {'type': 'array', 'items': STRING}},
        ['game', 'targets'],
    ),
]
BY_NAME = {name: fn for name, fn, _, _, _ in TOOLS}


def tool_list():
    return [
        {
            'name': name,
            'description': description,
            'inputSchema': {'type': 'object', 'properties': props, 'required': required, 'additionalProperties': False},
        }
        for name, _, description, props, required in TOOLS
    ]


def call(name, arguments):
    fn = BY_NAME.get(name)
    if not fn:
        return {'content': [{'type': 'text', 'text': f'no tool {name}'}], 'isError': True}
    try:
        value = fn(**(arguments or {}))
        text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False, indent=1)
        return {'content': [{'type': 'text', 'text': clip(text)}]}
    except (ToolError, RuntimeError, ValueError, OSError, TypeError, subprocess.TimeoutExpired) as error:
        return {'content': [{'type': 'text', 'text': f'{type(error).__name__}: {error}'}], 'isError': True}


def handle(message):
    method, mid = message.get('method'), message.get('id')
    if mid is None:  # a notification: initialized, cancelled
        return None
    params = message.get('params') or {}
    if method == 'initialize':
        result = {
            'protocolVersion': params.get('protocolVersion') or PROTOCOL,
            'capabilities': {'tools': {}},
            'serverInfo': {'name': 'scarletnx', 'version': '1'},
            'instructions': 'ScarletNX game reports and logs, and the local game folders their addresses are read '
            'against. Start from list_reports or get_report, fetch_log a log, then read_log its parts.',
        }
    elif method == 'tools/list':
        result = {'tools': tool_list()}
    elif method == 'tools/call':
        result = call(params.get('name'), params.get('arguments'))
    elif method == 'ping':
        result = {}
    else:
        return {'jsonrpc': '2.0', 'id': mid, 'error': {'code': -32601, 'message': f'no method {method}'}}
    return {'jsonrpc': '2.0', 'id': mid, 'result': result}


def main():
    for raw in sys.stdin:
        if not raw.strip():
            continue
        try:
            message = json.loads(raw)
        except json.JSONDecodeError as error:
            reply = {'jsonrpc': '2.0', 'id': None, 'error': {'code': -32700, 'message': str(error)}}
        else:
            try:
                reply = handle(message)
            except Exception as error:  # noqa: BLE001 - the server outlives one bad request
                traceback.print_exc(file=sys.stderr)
                reply = {'jsonrpc': '2.0', 'id': message.get('id'), 'error': {'code': -32603, 'message': str(error)}}
        if reply is not None:
            sys.stdout.write(json.dumps(reply, ensure_ascii=False) + '\n')
            sys.stdout.flush()


if __name__ == '__main__':
    main()
