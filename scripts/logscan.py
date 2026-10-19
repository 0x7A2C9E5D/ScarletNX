#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""A game's .log as a summary a machine can sort: which build ran which game on which GPU, how the run ended (a crash,
svcBreak, an unhandled exception, a fatal error, or nothing logged), the game function it ended in, the service
commands the runtime does not answer, and a signature, the same for every log that failed the same way, so reports
can be grouped by it.

    scripts/logscan.py LOG... [--format json|markdown]

LOG is a path, a .gz of one, or - for stdin. json writes one object a line, a line a log (SCHEMA below versions it);
markdown writes a summary for an issue comment. Logs of older nightlies, without the build: and game: lines, still
parse, with those fields null.

What is written is meant for public reports: paths are shortened to ~ (they name the player's account), and the crash
report's code and memory dumps, the game's own bytes, are left out. Symbols are shown demangled when llvm-cxxfilt or
c++filt is on PATH, and the signature keeps them mangled, so it does not depend on either.
"""

import argparse
import gzip
import json
import re
import shutil
import subprocess
import sys

SCHEMA = 1

STAMP = re.compile(r'^\[\s*\d+\.\d+\] ?')
TIME = re.compile(r'^\[\s*(\d+\.\d+)\]')
HEADER = re.compile(r'^ScarletNX - (.*?)(?: \((?:/|~|[A-Za-z]:[\\/]).*\))?$')
BUILD = re.compile(r'^build: ScarletNX (\S+) (\S+) \((.*)\)$')
GAME = re.compile(r'^game: ([0-9a-f]{16}), version (.*)$')
GPU = re.compile(r'^vulkan: (.+?), Vulkan ([\d.]+), driver (\w+)')

# the markers a run's end starts with, each kind's fields as named groups. The first in the log is the failure
FAILURES = [
    (
        'crash',
        re.compile(
            r'\*\*\* Crash \((?:signal (?P<signal>\d+)|exception (?P<exception>0x[0-9a-f]+))\) at host '
            r'pc=\S+?(?:, access to \S+?(?: \(game address (?P<game_address>[0-9a-f]+)\))?)? \*\*\*'
        ),
    ),
    ('svcBreak', re.compile(r'\*\*\* The game called svcBreak \(reason[ =](?P<reason>[0-9a-f]+)')),
    (
        'exception',
        re.compile(r'\*\*\* The game returned from an exception it had no handler for \((?P<result>[0-9a-f]+)'),
    ),
    ('brk', re.compile(r'\*\*\* The game stopped at BRK #(?P<imm>[0-9a-f]+) at (?P<address>[0-9a-f]+)')),
    (
        'unsupported instruction',
        re.compile(
            r'\*\*\* The game ran instruction (?P<word>[0-9a-f]+) at '
            r'(?P<address>[0-9a-f]+), which the recompiler cannot run'
        ),
    ),
    (
        'unsupported instruction',
        re.compile(
            r'^Unsupported (?P<set>Thumb|A32) instruction (?P<word>[0-9a-f]+) at '
            r'(?P<address>[0-9a-f]+)'
        ),
    ),
    (
        'thread stopped',
        re.compile(
            r"\*\*\* The game's thread \d+ stopped at (?P<address>[0-9a-f]+): (?P<what>.*) "
            r'\((?P<code>[0-9a-f]+)\) \*\*\*'
        ),
    ),
    ('host abort', re.compile(r'\*\*\* Shutdown: (?P<what>.*?) \*\*\*')),
]
FATAL = re.compile(r'^FATAL: (.*)$')

# where the crash report places the host pc (crash.c's report_place)
IN_GAME = re.compile(r'^inside the recompiled code of game function ([0-9a-f]+)')
IN_BLOCK = re.compile(r'^inside the recompiled (Thumb|A32) block ([0-9a-f]+)')
IN_RUNTIME = re.compile(r'^in the runtime itself \(exe offset \+([0-9a-f]+)\)')
IN_LIBRARY = re.compile(r'^in (\S+) \+([0-9a-f]+) \((.*?)\)')

# kernel_dump_regs' "pc <sym>  lr <sym>" and its backtrace's "#N addr <sym>"
PC_LINE = re.compile(r'^\s+pc (<[^>]*>)?\s+lr ')
FRAME = re.compile(r'^\s+#(\d+) ([0-9a-f]+)(?: <([^>]*)>)?\s*$')
SYMBOL = re.compile(r'^(.*)\+([0-9a-f]+)$')
# the lines of a report that hold the game's bytes or its registers, never copied out
DUMP = re.compile(r'^\s+(code |mem |vtable:|-> |x\d+\s*=|r\d+\s*=|pc=|cpsr=|info\+|data:|\(the rest)')

IPC = re.compile(r'^\s+\[(unknown|undeclared|bad request)\] (\S+) cmd (\d+)')
IPC_CONTROL = re.compile(r'^ipc: (\S+): control request (\d+), UnknownCommandId')
WARNING = re.compile(r'unsupported|is unknown|unknown, InvalidEnumValue|unimplemented|not implemented', re.IGNORECASE)
GAME_OUTPUT = re.compile(r'^\[game\] (.*)$')

# the SDK's abort and assert machinery, between svcBreak and the function that gave up: nn::diag, nn::result's
# unhandled results, the svc wrapper, libc's abort
ABORT_FRAME = re.compile(r'nn4diag|nn6result6detail|UnexpectedDefault|nn3svc\d*Break|^_?_?abort|__assert')

HOME = [
    re.compile(r'/home/[^/\s]+'),
    re.compile(r'/Users/[^/\s]+'),
    re.compile(r'[A-Za-z]:[\\/]Users[\\/][^\\/\s]+'),
    re.compile(r'/data/user/\d+/[^/\s]+'),
]


def redact(text):
    for home in HOME:
        text = home.sub('~', text)
    return text


def read_log(path):
    if path == '-':
        return sys.stdin.buffer.read().decode('utf-8', 'replace')
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] == b'\x1f\x8b':
        data = gzip.decompress(data)
    return data.decode('utf-8', 'replace')


def split_symbol(sym):
    """'<name+off>' without the brackets: the function (or module) and the offset into it"""
    if not sym:
        return None
    m = SYMBOL.match(sym)
    return {'name': m.group(1), 'offset': m.group(2)} if m else {'name': sym, 'offset': None}


def read_report(lines, start):
    """The failure's report from the marker at start: its detail lines (what, where, which thread), the symbol of
    the pc and the first backtrace, up to the next blank line, marker or thread list"""
    details, frames, pc, place = [], [], None, None
    for raw in lines[start + 1 : start + 400]:
        line = raw.rstrip()
        if not line.strip() or line.startswith(('***', 'FATAL:', 'threads (')):
            break
        f = FRAME.match(line)
        if f:
            frames.append({'address': f.group(2), 'symbol': split_symbol(f.group(3))})
            continue
        if frames:  # the backtrace ended; what follows is dumps, or another thread's
            if not DUMP.match(line) and not line.startswith('  last ') and not line.startswith('    '):
                break
            continue
        p = PC_LINE.match(line)
        if p:
            pc = split_symbol(p.group(1)[1:-1] if p.group(1) else None)
            continue
        if DUMP.match(line) or not line.startswith('  '):
            continue
        text = line.strip()
        for kind, pattern in (('game', IN_GAME), ('block', IN_BLOCK), ('runtime', IN_RUNTIME), ('library', IN_LIBRARY)):
            m = pattern.match(text)
            if m and not place:
                place = {'kind': kind, 'detail': list(m.groups())}
        if len(details) < 12:
            details.append(redact(text))
    return details, pc, frames, place


def culprit(pc, frames, stopped):
    """The game function the failure is pinned on: the pc's, else the first symbolized frame. When the game stopped
    itself, the pc is in the SDK's svc wrapper (often named after whatever symbol precedes it), so only the frames
    count, past the abort machinery"""
    candidates = ([pc] if pc and not stopped else []) + [f['symbol'] for f in frames if f['symbol']]
    for sym in candidates:
        if stopped and ABORT_FRAME.search(sym['name']):
            continue
        return sym
    return None


def normalize(message):
    return re.sub(r'\b(0x)?[0-9a-f]*\d[0-9a-f]*\b', 'N', message)


def scan(text):
    lines = [STAMP.sub('', raw) for raw in text.splitlines()]
    out = {
        'schema': SCHEMA,
        'title': None,
        'title_id': None,
        'game_version': None,
        'build': None,
        'commit': None,
        'branch': None,
        'gpu': None,
        'vulkan': None,
        'driver': None,
        'seconds': None,
        'outcome': 'no failure logged',
        'failure': None,
        'signature': None,
        'ipc': [],
        'warnings': [],
        'game_output': [],
        'tail': [],
    }
    for raw in reversed(text.splitlines()):
        m = TIME.match(raw)
        if m:
            out['seconds'] = float(m.group(1))
            break
    ipc, warnings, game_output, failure = {}, {}, [], None
    for i, line in enumerate(lines):
        if out['title'] is None:
            m = HEADER.match(line)
            if m:
                out['title'] = m.group(1)
                continue
        m = BUILD.match(line)
        if m:
            out['build'] = line[len('build: ') :]
            out['branch'], out['commit'] = m.group(1), m.group(2)
            continue
        m = GAME.match(line)
        if m:
            out['title_id'], out['game_version'] = m.group(1).upper(), m.group(2)
            continue
        m = GPU.match(line)
        if m and out['gpu'] is None:
            out['gpu'], out['vulkan'], out['driver'] = m.groups()
            continue
        m = IPC.match(line) or IPC_CONTROL.match(line)
        if m:
            kind, iface, cmd = m.groups() if len(m.groups()) == 3 else ('unknown control', *m.groups())
            key = (kind, iface, int(cmd))
            ipc[key] = ipc.get(key, 0) + 1
            continue
        m = GAME_OUTPUT.match(line)
        if m:
            game_output.append(redact(m.group(1)))
            continue
        if failure is None:
            for kind, pattern in FAILURES + [('fatal', FATAL)]:
                m = pattern.search(line)
                if m:
                    failure = {
                        'kind': kind,
                        'line': i,
                        'fields': m.groupdict() if m.groupdict() else {'fatal': m.group(1)},
                    }
                    break
            if failure:
                continue
        if WARNING.search(line) and not line.startswith(' '):
            key = redact(line.strip())
            warnings[key] = warnings.get(key, 0) + 1
    out['ipc'] = [
        {'kind': k, 'interface': iface, 'command': cmd, 'count': n}
        for (k, iface, cmd), n in sorted(ipc.items(), key=lambda kv: (kv[0][0], kv[0][1], kv[0][2]))
    ]
    out['warnings'] = [{'message': w, 'count': n} for w, n in list(warnings.items())[:30]]
    out['game_output'] = game_output[-10:]
    out['tail'] = [redact(line) for line in lines if line.strip() and not DUMP.match(line)][-15:]
    if failure:
        add_failure(out, lines, failure)
    return out


def add_failure(out, lines, failure):
    kind = failure['kind']
    details, pc, frames, place = read_report(lines, failure['line'])
    fatal = next((FATAL.match(line).group(1) for line in lines[failure['line'] :] if FATAL.match(line)), None)
    report = {
        'kind': kind,
        'details': details,
        'fatal': redact(fatal) if fatal else None,
        'pc': pc,
        'frames': frames[:16],
        'place': place,
        'function': None,
    }
    report.update({k: redact(v) for k, v in failure['fields'].items() if v is not None})
    if kind == 'crash':
        report['what'] = details[0] if details else None
    if kind == 'unsupported instruction':
        report.setdefault('set', 'A64')
    # svcBreak and an unhandled exception are the game stopping itself, from inside the SDK's abort
    report['function'] = culprit(pc, frames, kind in ('svcBreak', 'exception'))
    out['outcome'], out['failure'] = kind, report
    out['signature'] = signature(out, report)


def signature(out, r):
    kind, fn, place = r['kind'], r['function'], r['place']
    if kind == 'unsupported instruction':
        return f'unsupported {r["set"]} instruction {r["word"]}'
    if kind == 'fatal':
        return 'fatal ' + normalize(r['fatal'] or '')
    if kind == 'host abort':
        return 'host ' + normalize(r['what'])
    head = {
        'crash': f'crash ({r.get("what") or "unknown"})',
        'svcBreak': 'svcBreak',
        'brk': 'brk',
        'exception': f'exception {r.get("result")}',
        'thread stopped': r.get('what'),
    }[kind]
    # in the runtime or a library, the host place is the bug's, whatever game code led there
    if place and place['kind'] == 'runtime':
        return f'{head} in the runtime {out["commit"] or "unknown build"} +{place["detail"][0]}'
    if place and place['kind'] == 'library':
        return f'{head} in {place["detail"][0]} {place["detail"][2]}'
    if fn:
        # a module and an offset (<main+1234>) only means something within one game and version
        named = fn['name'] not in ('rtld', 'main', 'sdk', 'module') and not fn['name'].startswith('subsdk')
        where = fn['name'] if named else f'{out["title_id"] or out["title"]} {fn["name"]}+{fn["offset"]}'
        return f'{head} in {where}'
    return f'{head} in unknown code'


def demangler():
    for tool in ('llvm-cxxfilt', 'c++filt'):
        path = shutil.which(tool)
        if path:
            return path
    return None


def demangle_all(reports):
    """Each symbol's demangled name beside it, in one call of the demangler for every name"""
    tool = demangler()
    syms = []
    for r in reports:
        f = r['failure']
        if not f:
            continue
        syms += [s for s in [f['pc'], f['function']] + [fr['symbol'] for fr in f['frames']] if s]
    names = sorted({s['name'] for s in syms if s['name'].startswith('_Z')})
    plain = {}
    if tool and names:
        try:
            result = subprocess.run(
                [tool], input='\n'.join(names) + '\n', capture_output=True, text=True, timeout=30, check=False
            )
            plain = dict(zip(names, result.stdout.splitlines()))
        except (OSError, subprocess.SubprocessError):
            plain = {}
    for s in syms:
        s['demangled'] = plain.get(s['name'], s['name'])


def show(sym):
    if not sym:
        return '?'
    name = sym.get('demangled', sym['name'])
    return f'{name}+{sym["offset"]}' if sym['offset'] else name


def markdown(path, r):
    out = []
    title = r['title'] or path
    out.append(f'### {title}' + (f' ({r["title_id"]}, version {r["game_version"]})' if r['title_id'] else ''))
    out.append('')
    out.append(f'- **Build:** {r["build"] or "not in the log (a build older than its build: line)"}')
    if r['gpu']:
        out.append(f'- **GPU:** {r["gpu"]}, Vulkan {r["vulkan"]}, driver {r["driver"]}')
    out.append(f'- **Ended:** {r["outcome"]}' + (f' after {r["seconds"]:.1f} s' if r['seconds'] is not None else ''))
    if r['signature']:
        out.append(f'- **Signature:** `{r["signature"]}`')
    f = r['failure']
    if f:
        if f['function']:
            out.append(f'- **In:** `{show(f["function"])}`')
        if f['fatal'] and f['kind'] != 'fatal':
            out.append(f'- **Message:** {f["fatal"]}')
        if f['details']:
            out += ['', '```', *f['details'], '```']
        if f['frames']:
            out += ['', '<details><summary>Backtrace</summary>', '', '```']
            out += [
                f'#{i} {fr["address"]} {show(fr["symbol"]) if fr["symbol"] else ""}'.rstrip()
                for i, fr in enumerate(f['frames'])
            ]
            out += ['```', '</details>']
    if r['ipc']:
        out += ['', '**Service commands the runtime does not implement:**', '']
        out += [f'- `{c["interface"]}` cmd {c["command"]} ({c["kind"]}, {c["count"]}x)' for c in r['ipc']]
    if r['warnings']:
        out += ['', '<details><summary>Warnings</summary>', '', '```']
        out += [w['message'] + (f' ({w["count"]}x)' if w['count'] > 1 else '') for w in r['warnings']]
        out += ['```', '</details>']
    if r['game_output']:
        out += [
            '',
            "<details><summary>The game's last output</summary>",
            '',
            '```',
            *r['game_output'],
            '```',
            '</details>',
        ]
    out += ['', "<details><summary>The log's last lines</summary>", '', '```', *r['tail'], '```', '</details>']
    return '\n'.join(out) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('logs', nargs='+', metavar='LOG')
    ap.add_argument('--format', choices=('json', 'markdown'), default='json')
    args = ap.parse_args()
    reports = []
    for path in args.logs:
        try:
            reports.append((path, scan(read_log(path))))
        except OSError as e:
            sys.exit(f'logscan: {e}')
    demangle_all([r for _, r in reports])
    for path, r in reports:
        if args.format == 'json':
            print(json.dumps(r, ensure_ascii=False))
        else:
            print(markdown(path, r))


if __name__ == '__main__':
    main()
