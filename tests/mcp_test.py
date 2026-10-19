#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scarletnx --mcp (src/nx/runtime/driver/mcp.c) driven over stdio as an MCP client does: initialize, the tool list,
scan_log, read_log by text, by range and its last lines, and its refusal of a file outside the data folder, find_game
on a made-up extracted game (its NPDM's program ID, its NACP's version), symbolize and disassemble on a made-up NRO
(nx_runtime_smoke.py's), the errors a tool answers with, an unknown method, a notification left unanswered and a line
that isn't JSON. Offline.

    tests/mcp_test.py SCARLETNX"""

import json
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nx_runtime_smoke import make_nro

LOG = """[   0.00] ScarletNX - Some Game
[   0.00] build: ScarletNX main 1234abc (clang 23.1.3, Linux x86_64)
[   0.30] game: 0100aaaabbbb0000, version 1.2.3
[   1.00] sm: GetServiceHandle(hid)
[   2.25]
*** The game called svcBreak (reason 0, info 1300100df8, 4 bytes) ***
[   2.25]    #0 163d18d8 <_ZN2nn4diag6detail10VAbortImplEv+d8>
[   2.25]    #1 1683a358 <_ZN2nn3hid18GetNpadJoyHoldTypeEv+38>
[   2.32] FATAL: The game stopped (svcBreak, reason 0). See the .log for details.
"""


# the parts of an edenprobe checkout the scaffold writes into, as edenprobe lays them out
CHECKOUT = {
    'source/serviceref.h': (
        'typedef enum {\n    SERVICE_RANDOM, ///< csrng: two asks fill their buffers, differently\n'
        '    SERVICE_CASES\n} ServiceCaseId;\n\nbool ServiceJudgeRandom(uint32_t rc, char *out, size_t out_size);\n\n'
        '#ifdef __cplusplus\n}\n#endif\n'
    ),
    'source/serviceref.c': (
        'static const struct {\n    const char *suite, *name;\n} CASES[] = {\n'
        '    [SERVICE_RANDOM] = { "csrng", "GenerateRandomBytes fills the buffer" },\n};\n'
    ),
    'source/tests/t_service.c': 'static void RunCsrng(void) {}\n\nvoid RunService(void) {\n    RunCsrng();\n}\n',
    'source/tests/t_serviceref.c': 'static void CheckRandom(void) {}\n\nvoid RunReferenceService(void) {\n    CheckRandom();\n}\n',
    'source/ui.c': '    { "csrng", UI_FAMILY_SERVICE },\n    { "nifm", UI_FAMILY_NET },\n',
}
NAME = 'GetNpadJoyHoldType answers the hold type set before it'


def report(result, detail):
    lines = [
        {'type': 'note', 'text': 'edenprobe 0.5.0 (abc1234) starting'},
        {'type': 'case', 'suite': 'hid-style', 'name': NAME, 'tier': 'exact', 'result': result, 'detail': detail},
        {'type': 'summary', 'passed': 1, 'failed': 0, 'skipped': 0, 'crashed': 0},
    ]
    return ''.join(json.dumps(line) + '\n' for line in lines)


def check(cond, what):
    if not cond:
        sys.exit(f'mcp_test: {what}')


def npdm(program_id):
    """A main.npdm with what the loader reads: META, 64-bit, the ACI0's offset and size, its program ID"""
    data = bytearray(0x100)
    data[0:4] = b'META'
    data[0x0C] = 1
    struct.pack_into('<II', data, 0x70, 0x80, 0x40)
    data[0x80:0x84] = b'ACI0'
    struct.pack_into('<Q', data, 0x90, program_id)
    return bytes(data)


def call(tool, id_=None, **arguments):
    return {
        'jsonrpc': '2.0',
        'id': id_ or tool,
        'method': 'tools/call',
        'params': {'name': tool, 'arguments': arguments},
    }


def main():
    runtime = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        data, outside = Path(tmp) / 'data', Path(tmp) / 'outside'
        game = data / 'work' / 'Some Game'
        (game / 'exefs').mkdir(parents=True)
        (game / 'exefs' / 'main').write_bytes(b'\0' * 16)
        (game / 'exefs' / 'main.npdm').write_bytes(npdm(0x0100AAAABBBB0000))
        nacp = bytearray(0x4000)
        nacp[0x3060:0x3065] = b'1.2.3'
        (game / 'control.nacp').write_bytes(bytes(nacp))
        (data / 'games').mkdir()
        log = data / 'games' / 'game.log'
        log.write_text(LOG)
        outside.mkdir()
        (outside / 'secret.log').write_text(LOG)
        (outside / 'secret.nro').write_bytes(make_nro())
        nro = data / 'smoke.nro'
        nro.write_bytes(make_nro())
        # a clone on Windows has CRLF lines (git's autocrlf): the scaffold keeps each file's ending. Written as bytes,
        # so every system writes the same
        checkout, crlf, broken = Path(tmp) / 'edenprobe', Path(tmp) / 'crlf', Path(tmp) / 'broken'
        for root, ending in ((checkout, '\n'), (crlf, '\r\n'), (broken, '\n')):
            for rel, text in CHECKOUT.items():
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_bytes(text.replace('\n', ending).encode())
        (broken / 'source/serviceref.h').write_bytes(b'typedef enum {\n} ServiceCaseId;\n')
        ours, theirs, passing = Path(tmp) / 'ours.jsonl', Path(tmp) / 'theirs.jsonl', Path(tmp) / 'passing.jsonl'
        ours.write_text(report('fail', 'GetNpadJoyHoldType: rc 0xca8c0500'))
        theirs.write_text(report('pass', 'hold type 1, rc 0x0'))
        passing.write_text(report('pass', 'hold type 1, rc 0x0'))
        hold = {
            'family': 'service',
            'suite': 'hid-style',
            'id': 'HID_HOLD_TYPE',
            'name': NAME,
            'tier': 'exact',
            'issue': 345,
        }

        requests = [
            {'jsonrpc': '2.0', 'id': 'init', 'method': 'initialize', 'params': {'protocolVersion': '2025-06-18'}},
            {'jsonrpc': '2.0', 'method': 'notifications/initialized'},
            {'jsonrpc': '2.0', 'id': 'list', 'method': 'tools/list'},
            call('scan_log', path=str(log)),
            call('read_log', 'grep', path=str(log), pattern='SVCBREAK|nothing', ignore_case=True, context=1),
            call('read_log', 'range', path=str(log), start=2, count=2),
            call('read_log', 'tail', path=str(log), count=3),
            call('read_log', 'outside', path=str(outside / 'secret.log')),
            call('scan_log', 'scan_outside', path=str(outside / 'secret.log')),
            call('symbolize', 'game_outside', game=str(outside / 'secret.nro'), addresses=['8000000']),
            call('find_game', title_id='0100aaaabbbb0000'),
            call('find_game', 'find_none', title_id='0100000000000001'),
            call('symbolize', game=str(nro), addresses=['8000080', 'zz']),
            call('disassemble', game=str(nro), start='8000000', end='8000094'),
            call('disassemble', 'no_range', game=str(nro)),
            call('symbolize', 'no_game', game='0100000000000001', addresses=['1']),
            call('edenprobe_scaffold', 'scaffold', checkout=str(checkout), **hold),
            call('edenprobe_scaffold', 'again', checkout=str(checkout), **hold),
            call('edenprobe_scaffold', 'crlf', checkout=str(crlf), **hold),
            call('edenprobe_scaffold', 'broken', checkout=str(broken), **hold),
            call('edenprobe_scaffold', 'bad_id', checkout=str(checkout), **dict(hold, id='hold')),
            call('edenprobe_scaffold', 'cpu', checkout=str(checkout), **dict(hold, family='cpu')),
            call('edenprobe_check', 'no_host', checkout=str(data)),
            call('edenprobe_run', 'no_nro', nro=str(data / 'none.nro'), mode='quick notime service', filter='hid'),
            call(
                'edenprobe_finding',
                'finding',
                issue=345,
                suite='hid-style',
                name=NAME,
                scarletnx_report=str(ours),
                reference_report=str(theirs),
                source='reden answers what SetNpadJoyHoldType set.',
            ),
            call(
                'edenprobe_finding', 'passing', issue=345, suite='hid-style', name=NAME, scarletnx_report=str(passing)
            ),
            call('nope'),
            {'jsonrpc': '2.0', 'id': 'method', 'method': 'resources/list'},
        ]
        stdin = ''.join(json.dumps(r) + '\n' for r in requests) + 'not json\n'
        run = subprocess.run(
            [runtime, '--mcp', '--repo', str(data)],
            input=stdin,
            capture_output=True,
            text=True,
            timeout=120,
            check=True,
        )
        written = {rel: (checkout / rel).read_bytes().decode() for rel in CHECKOUT}
        crlf_written = {rel: (crlf / rel).read_bytes().decode() for rel in CHECKOUT}
        untouched = all(
            (broken / rel).read_bytes() == text.encode() for rel, text in CHECKOUT.items() if 'serviceref.h' not in rel
        )
    replies = [json.loads(line) for line in run.stdout.splitlines()]
    by_id = {r['id']: r for r in replies}
    check(
        len(replies) == len(requests),
        f'a reply a request but the notification, and one for the bad line: {len(replies)}',
    )
    check(any(r['id'] is None and r['error']['code'] == -32700 for r in replies), 'a line that is not JSON')

    def text(name):
        return by_id[name]['result']['content'][0]['text']

    def failed(name):
        return by_id[name]['result'].get('isError', False)

    init = by_id['init']['result']
    check(init['protocolVersion'] == '2025-06-18' and 'tools' in init['capabilities'], 'initialize')
    check('same game version' in init['instructions'], 'the instructions')
    tools = {t['name']: t for t in by_id['list']['result']['tools']}
    check(
        set(tools)
        == {
            'scan_log',
            'read_log',
            'find_game',
            'symbolize',
            'disassemble',
            'edenprobe_scaffold',
            'edenprobe_check',
            'edenprobe_run',
            'edenprobe_finding',
        },
        f'tools: {sorted(tools)}',
    )
    check(all(t['inputSchema']['type'] == 'object' and len(t['description']) > 40 for t in tools.values()), 'schemas')
    check(tools['symbolize']['inputSchema']['required'] == ['game', 'addresses'], 'required arguments')

    scan = json.loads(text('scan_log'))
    check(scan['signature'] == 'svcBreak in _ZN2nn3hid18GetNpadJoyHoldTypeEv', f'scan_log: {scan["signature"]}')
    check(scan['failure']['function']['demangled'] == 'nn::hid::GetNpadJoyHoldType()', 'demangled in the binary')

    grep = text('grep').splitlines()
    check(grep[0].endswith('has 9 lines'), grep[0])
    check(any(line.startswith('6: *** The game called svcBreak') for line in grep), f'read_log by text:\n{grep}')
    # two matches (the marker and FATAL's line), a line of context each side, joined
    check([line.split(':')[0] for line in grep[1:]] == ['5', '6', '7', '8', '9'], f'context each side: {grep}')
    check([line.split(':')[0] for line in text('range').splitlines()[1:]] == ['2', '3'], 'read_log by range')
    check([line.split(':')[0] for line in text('tail').splitlines()[1:]] == ['7', '8', '9'], 'read_log, the last lines')
    check(failed('outside') and 'outside the folders' in text('outside'), 'read_log refuses a file outside')
    check(failed('scan_outside') and 'outside the folders' in text('scan_outside'), 'so does scan_log')
    check(failed('game_outside') and 'outside the folders' in text('game_outside'), 'and symbolize, for a game')

    games = json.loads(text('find_game'))
    check(
        len(games) == 1 and games[0]['title_id'] == '0100AAAABBBB0000' and games[0]['version'] == '1.2.3',
        f'find_game: {games}',
    )
    check(json.loads(text('find_none')) == [], 'find_game with another title ID')

    sym = text('symbolize').splitlines()
    check(sym[0].startswith('8000080 main') and 'not a hexadecimal address' in sym[1], f'symbolize: {sym}')
    dis = text('disassemble')
    if 'no A64 disassembler' not in dis:
        check('b\t0x80' in dis.replace(' ', '\t') or '8000080' in dis.splitlines()[0], f'the first branch:\n{dis}')
        check('svc' in dis and 'adr' in dis, f'the code disassembled:\n{dis}')
    check(failed('no_range') and 'name a function, or a range' in text('no_range'), 'disassemble without a target')
    check(failed('no_game') and 'title ID' in text('no_game'), 'a title ID with no game')
    check(failed('nope'), 'an unknown tool')
    check(by_id['method']['error']['code'] == -32601, 'an unknown method')
    # the scaffold: each part where edenprobe keeps it, the call at its runner's end, the new suite's ui.c row
    check(not failed('scaffold') and 'Left to write' in text('scaffold'), text('scaffold'))
    h, c = written['source/serviceref.h'], written['source/serviceref.c']
    check(h.index('    SERVICE_HID_HOLD_TYPE, ///< hid-style: ') < h.index('    SERVICE_CASES'), 'the enum entry')
    check(h.index('bool ServiceJudgeHidHoldType(') < h.index('#ifdef __cplusplus'), 'the judge declared')
    check(
        '    [SERVICE_HID_HOLD_TYPE] = { "hid-style",' in c and c.index('[SERVICE_HID_HOLD_TYPE]') < c.index('\n};'),
        f'the CASES row:\n{c}',
    )
    check('bool ServiceJudgeHidHoldType(uint32_t rc' in c and 'TODO(ScarletNX#345)' in c, 'the judge')
    t = written['source/tests/t_service.c']
    check(t.index('static void RunHidHoldType(void)') < t.index('void RunService(void)'), 'the device side')
    check(t.rstrip().endswith('RunCsrng();\n    RunHidHoldType();\n}'), f'called at RunService end:\n{t}')
    check('Report(SERVICE_HID_HOLD_TYPE, CHECK_EXACT, passed, detail);' in t, 'the tier')
    r = written['source/tests/t_serviceref.c']
    check(r.rstrip().endswith('CheckRandom();\n    CheckHidHoldType();\n}'), f'the reference check:\n{r}')
    check(
        written['source/ui.c'].startswith(
            '    { "csrng", UI_FAMILY_SERVICE },\n    { "hid-style", UI_FAMILY_SERVICE },'
        ),
        'the new suite row after the service family',
    )
    check(failed('again') and 'already' in text('again'), 'a case written twice')
    check(not failed('crlf'), f'a clone with CRLF lines: {text("crlf")}')
    check(
        all(
            t.replace('\r\n', '\n') == written[rel] and '\n' not in t.replace('\r\n', '')
            for rel, t in crlf_written.items()
        ),
        'the CRLF clone gets the same parts, every line still ending in CRLF',
    )
    check(
        failed('broken') and 'nothing was written' in text('broken') and untouched, 'an unknown layout, no file changed'
    )
    check(failed('bad_id') and failed('cpu'), 'a bad id, a family the scaffold does not write')
    check(failed('no_host') and 'host.mk' in text('no_host'), 'edenprobe_check outside a checkout')
    check(failed('no_nro') and 'cannot be read' in text('no_nro'), 'edenprobe_run without its NRO')

    # the finding: ScarletNX's line and the reference's, the subject's length said, warnings when they don't differ
    finding = text('finding')
    check(
        '| ScarletNX (`edenprobe 0.5.0 (abc1234) starting`) | fail | `GetNpadJoyHoldType: rc 0xca8c0500` |' in finding,
        finding,
    )
    check('| reference (' in finding and 'scarletnx/ScarletNX#345' in finding and 'WARNING' not in finding, finding)
    check('this one is' in finding, 'a subject over 72 characters is said so')
    check('WARNING: the case passes on ScarletNX' in text('passing'), 'a case that shows nothing')
    print('mcp_test: ok')


if __name__ == '__main__':
    main()
