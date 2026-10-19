#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""tools/snx_mcp.py driven over stdio as an MCP client does: initialize, the tool list and its schemas, scan_log,
read_log by pattern and by range and its refusal of a file outside its folders, find_game on a made-up extracted game
(its NPDM's program ID and its NACP's version), symbolize's check of its addresses, an unknown tool and method, and a
notification left unanswered. Offline: no report tool is called."""

import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

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
        sys.exit(f'snx_mcp_test: {what}')


def npdm(program_id):
    """A main.npdm with only what the server reads: META, the ACI0's offset at 0x70, its program ID at +0x10"""
    data = bytearray(0x100)
    data[0:4] = b'META'
    struct.pack_into('<I', data, 0x70, 0x80)
    data[0x80:0x84] = b'ACI0'
    struct.pack_into('<Q', data, 0x90, program_id)
    return bytes(data)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        cache, work, outside = tmp / 'cache', tmp / 'work', tmp / 'outside'
        (cache / '1').mkdir(parents=True)
        (work / 'Some Game' / 'exefs').mkdir(parents=True)
        outside.mkdir()
        log = cache / '1' / 'game.log'
        log.write_text(LOG)
        (outside / 'secret.log').write_text(LOG)
        (work / 'Some Game' / 'exefs' / 'main.npdm').write_bytes(npdm(0x0100AAAABBBB0000))
        nacp = bytearray(0x4000)
        nacp[0x3060:0x3065] = b'1.2.3'
        (work / 'Some Game' / 'control.nacp').write_bytes(bytes(nacp))

        def call(name, **arguments):
            return {
                'jsonrpc': '2.0',
                'id': name,
                'method': 'tools/call',
                'params': {'name': name, 'arguments': arguments},
            }

        requests = [
            {'jsonrpc': '2.0', 'id': 'init', 'method': 'initialize', 'params': {'protocolVersion': '2025-06-18'}},
            {'jsonrpc': '2.0', 'method': 'notifications/initialized'},
            {'jsonrpc': '2.0', 'id': 'list', 'method': 'tools/list'},
            call('scan_log', path=str(log)),
            dict(call('read_log', path=str(log), pattern='svcBreak', context=1), id='grep'),
            dict(call('read_log', path=str(log), start=2, count=2), id='range'),
            dict(call('read_log', path=str(outside / 'secret.log')), id='outside'),
            call('find_game', title_id='0100aaaabbbb0000'),
            call('symbolize', game='Some Game', addresses=['-rf']),
            call('nope'),
            {'jsonrpc': '2.0', 'id': 'method', 'method': 'resources/list'},
        ]
        env = dict(os.environ, SNX_REPORTS_CACHE=str(cache), SNX_WORK=str(work), SNX_REPORTS_REPO='nobody/nothing')
        run = subprocess.run(
            [sys.executable, str(ROOT / 'tools' / 'snx_mcp.py')],
            input=''.join(json.dumps(r) + '\n' for r in requests),
            capture_output=True,
            text=True,
            env=env,
            timeout=60,
            check=True,
        )
    replies = {}
    for line in run.stdout.splitlines():
        m = json.loads(line)
        replies[m['id']] = m
    check(len(replies) == len(requests) - 1, f'one reply a request, none to the notification: {sorted(replies)}')

    def text(name):
        return replies[name]['result']['content'][0]['text']

    def failed(name):
        return replies[name]['result'].get('isError', False)

    init = replies['init']['result']
    check(init['protocolVersion'] == '2025-06-18' and 'tools' in init['capabilities'], 'initialize')
    tools = {t['name']: t for t in replies['list']['result']['tools']}
    check(
        set(tools)
        == {'list_reports', 'get_report', 'fetch_log', 'scan_log', 'read_log', 'find_game', 'symbolize', 'disassemble'},
        f'tools: {sorted(tools)}',
    )
    check(all(t['inputSchema']['type'] == 'object' and t['description'] for t in tools.values()), 'schemas')
    check(tools['symbolize']['inputSchema']['required'] == ['game', 'addresses'], 'required arguments')

    scan = json.loads(text('scan_log'))
    check(
        scan['signature'] == 'svcBreak in _ZN2nn3hid18GetNpadJoyHoldTypeEv' and scan['title_id'] == '0100AAAABBBB0000',
        f'scan_log: {scan["signature"]}',
    )
    grep = text('grep')
    check(
        '6: *** The game called svcBreak' in grep
        and '5: ' in grep
        and '7: ' in grep
        and '1: ' not in grep.split('\n', 1)[1],
        f'read_log by pattern, with a line of context:\n{grep}',
    )
    rng = text('range').split('\n')[1:]
    check([line.split(':')[0] for line in rng] == ['2', '3'], f'read_log by range: {rng}')
    check(failed('outside') and 'outside the folders' in text('outside'), 'read_log refuses a file outside its folders')
    games = json.loads(text('find_game'))
    check(
        len(games) == 1 and games[0]['title_id'] == '0100AAAABBBB0000' and games[0]['version'] == '1.2.3',
        f'find_game: {games}',
    )
    check(failed('symbolize') and 'hexadecimal' in text('symbolize'), 'symbolize takes only addresses')
    check(failed('nope'), 'an unknown tool is an error result')
    check(replies['method']['error']['code'] == -32601, 'an unknown method is a JSON-RPC error')
    print('snx_mcp_test: ok')


if __name__ == '__main__':
    main()
