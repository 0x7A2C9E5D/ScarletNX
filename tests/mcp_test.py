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


def call(name, id_=None, **arguments):
    return {
        'jsonrpc': '2.0',
        'id': id_ or name,
        'method': 'tools/call',
        'params': {'name': name, 'arguments': arguments},
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
        nro = data / 'smoke.nro'
        nro.write_bytes(make_nro())

        requests = [
            {'jsonrpc': '2.0', 'id': 'init', 'method': 'initialize', 'params': {'protocolVersion': '2025-06-18'}},
            {'jsonrpc': '2.0', 'method': 'notifications/initialized'},
            {'jsonrpc': '2.0', 'id': 'list', 'method': 'tools/list'},
            call('scan_log', path=str(log)),
            call('read_log', 'grep', path=str(log), pattern='SVCBREAK|nothing', ignore_case=True, context=1),
            call('read_log', 'range', path=str(log), start=2, count=2),
            call('read_log', 'tail', path=str(log), count=3),
            call('read_log', 'outside', path=str(outside / 'secret.log')),
            call('find_game', title_id='0100aaaabbbb0000'),
            call('find_game', 'find_none', title_id='0100000000000001'),
            call('symbolize', game=str(nro), addresses=['8000080', 'zz']),
            call('disassemble', game=str(nro), start='8000000', end='8000094'),
            call('disassemble', 'no_range', game=str(nro)),
            call('symbolize', 'no_game', game='0100000000000001', addresses=['1']),
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
    check(set(tools) == {'scan_log', 'read_log', 'find_game', 'symbolize', 'disassemble'}, f'tools: {sorted(tools)}')
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
    print('mcp_test: ok')


if __name__ == '__main__':
    main()
