#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/logscan.py on made-up logs in the runtime's formats: the header lines, each kind of failure and the
function it is pinned on (past the SDK's abort for svcBreak), the place a crash is put (the game, the runtime, a
library), the older nightlies' svcBreak line and header, service commands counted, signatures that agree across runs,
paths shortened and the dumps left out, gzip and the markdown summary. Offline."""

import gzip
import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'scripts' / 'logscan.py'
spec = importlib.util.spec_from_file_location('logscan', SCRIPT)
logscan = importlib.util.module_from_spec(spec)
spec.loader.exec_module(logscan)

HEADER = """[   0.00] ScarletNX - Some Game
[   0.00] build: ScarletNX main 1234abc (clang 23.1.3, Linux x86_64)
[   0.05] module main     at 0008005000 (71.8 MB)
[   0.27] vulkan: Some GPU 3000, Vulkan 1.4.351, driver 99d38200, graphics queue family 0, compute 2 (separate family)
[   0.30] game: 0100aaaabbbb0000, version 1.2.3
[   1.00]   [undeclared] nn::audio::detail::IAudioDevice cmd 7 -> ok
[   1.00]   [undeclared] nn::audio::detail::IAudioDevice cmd 7 -> ok
[   1.01]   [unknown] nn::hid::IHidServer cmd 92 -> UnknownCommandId
[   1.02] ipc: nn::foo::IBar: control request 4, UnknownCommandId
[   1.03] vulkan: unsupported texture format 3f, header word 0 0000003f (not drawn)
[   1.04] [game] loading /home/someone/save.bin
"""

SVC_BREAK = (
    HEADER
    + """[   2.25]
*** The game called svcBreak (reason 0, info 1300100df8, 4 bytes) ***
[   2.25]   info+00: 00000000ca8c0500
[   2.25]   x0 = 0000000000000000  x1 = 0000001300100df8  x2 = 0000000000000004  x3 = 0000000000000000
[   2.25]   pc=164ab3c4 lr=163d4828 sp=1300100c30 fp=1300100c30 tls=0
[   2.25]   pc <_ZNSt3__1somethingunrelated+364>  lr <_ZN2nn4diag6detail5AbortEPKNS_6ResultE+18>
[   2.25]    #0 163d18d8 <_ZN2nn4diag6detail10VAbortImplEv+d8>
[   2.25]    #1 163d1a68 <_ZN2nn4diag6detail9AbortImplEPKNS_6ResultE+28>
[   2.25]    #2 1683a358 <_ZN2nn3hid18GetNpadJoyHoldTypeEv+38>
[   2.25]    #3 9a1e2c4
[   2.25]    #4 927fd38 <nnMain+1938>
[   2.25]   code 163d4768: f90007ff 94171a7d a9417bfd 910083ff
[   2.25]   mem x1 1300100df8: 58cca 1300100e10 9a1e2c4 1300100f60
[   2.25]     vtable: [1] <_ZN2nn3hid18GetNpadJoyHoldTypeEv+38>
[   2.26] threads (1):
[   2.26]   thread 1 'main' prio 44 (base 44) core 0: running
[   2.26]    #1 deadbeef <_ZN5other6threadEv+4>
[   2.32] FATAL: The game stopped (svcBreak, reason 0). See the .log for details.
"""
)

# a nightly before the build: and game: lines, with the folder in its header and svcBreak's older line
OLD_SVC_BREAK = """[   0.00] ScarletNX - Some Game (/home/someone/games/Some Game/Some Game)
[   2.25]
*** The game called svcBreak (reason=0, info=1300100df8 size=4) ***
[   2.25]   data: ca8c0500
[   2.25]   pc <_ZNSt3__1somethingunrelated+364>  lr <_ZN2nn4diag6detail5AbortEPKNS_6ResultE+18>  x16-> <x+0>
[   2.25]    #0 163d18d8 <_ZN2nn4diag6detail10VAbortImplEv+d8>
[   2.25]    #1 1683a358 <_ZN2nn3hid18GetNpadJoyHoldTypeEv+48>
[   2.32] FATAL: The game stopped (svcBreak). See the .log for details.
"""

CRASH_IN_GAME = (
    HEADER
    + """[   3.00]
*** Crash (signal 11) at host pc=0x7f0000001234, access to 0x7e0000000010 (game address 10) ***
[   3.00]   invalid memory access
[   3.00]   inside the recompiled code of game function 8123450 (+40 on the host)
[   3.00]   a null pointer plus an offset: a game short of memory does this, memory_gb=6 in its .ini may help
[   3.00]   thread 7 (Render)
[   3.00]   x0 = 0000000000000000  x1 = 0000000000000010  x2 = 0000000000000004  x3 = 0000000000000000
[   3.00]   pc <_ZN4Game6Render4DrawEv+24>  lr <_ZN4Game6Render5FrameEv+80>
[   3.00]    #0 8123500 <_ZN4Game6Render5FrameEv+80>
[   3.00]   code 8123460: f90007ff 94171a7d a9417bfd 910083ff
[   3.00]   memory: 1.2 GB of 4.0 GB used, by /home/someone/x
[   3.01] FATAL: The game crashed (invalid memory access). See the .log for details.
"""
)

CRASH_IN_RUNTIME = (
    HEADER
    + """[   3.00]
*** Crash (signal 11) at host pc=0x55000001234, access to 0x10 ***
[   3.00]   invalid memory access
[   3.00]   in the runtime itself (exe offset +1a2b3c)
[   3.00]   on one of the runtime's threads, not the game's
[   3.01] FATAL: The game crashed (invalid memory access). See the .log for details.
"""
)

CRASH_IN_LIBRARY = (
    HEADER
    + """[   3.00]
*** Crash (signal 11) at host pc=0x7f00000099, access to 0x10 ***
[   3.00]   invalid memory access
[   3.00]   in libnvidia-glcore.so.580 +abc123 (?), on one of the runtime's threads
[   3.00]   host frames: libnvidia-glcore.so.580+abc123 scarletnx+1234
[   3.01] FATAL: The game crashed (invalid memory access). See the .log for details.
"""
)

UNDEFINED = (
    HEADER
    + """[   4.00]
*** The game ran instruction ce62c820 at 8040598, which the recompiler cannot run ***
[   4.00]   pc <main+40598>  lr <main+40568>
[   4.00]    #0 8040568 <main+40568>
[   4.01] FATAL: The game ran an instruction the recompiler cannot run (ce62c820 at 8040598). See the .log for details.
"""
)

# the launcher's Share log file: its build, its message, the run's output (the game's warnings again, on stderr), then
# the newest game log, one without a build: line
LAUNCHER = (
    'ScarletNX main 9999fff (clang 23.1.3, Windows x86_64)\n'
    'Message: The game stopped (svcBreak). See C:\\Users\\someone\\AppData\\x.log\n'
    '\n== the last run: C:\\Users\\someone\\run.log ==\n'
    '  [undeclared] nn::audio::detail::IAudioDevice cmd 7 -> ok\n'
    '*** Crash (signal 11) at host pc=0x1 ***\n'
    '\n== C:\\Users\\someone\\state\\Some Game.log ==\n' + OLD_SVC_BREAK
)

FATAL_ONLY = (
    HEADER
    + """[   0.50] FATAL: Could not load the game executables. See the .log for details.
"""
)


def check(cond, what):
    if not cond:
        sys.exit(f'logscan_test: {what}')


def main():
    r = logscan.scan(SVC_BREAK)
    check(r['schema'] == logscan.SCHEMA, 'schema')
    check(
        r['title'] == 'Some Game' and r['title_id'] == '0100AAAABBBB0000' and r['game_version'] == '1.2.3',
        f'header: {r["title"]!r} {r["title_id"]!r} {r["game_version"]!r}',
    )
    check(r['commit'] == '1234abc' and r['branch'] == 'main', 'build line')
    check(r['gpu'] == 'Some GPU 3000' and r['driver'] == '99d38200', 'gpu line')
    check(r['outcome'] == 'svcBreak' and r['failure']['reason'] == '0', 'svcBreak kind')
    check(
        r['failure']['function']['name'] == '_ZN2nn3hid18GetNpadJoyHoldTypeEv',
        f'svcBreak is pinned past the abort and not on the pc: {r["failure"]["function"]}',
    )
    check(len(r['failure']['frames']) == 5, f"only the failing thread's backtrace: {len(r['failure']['frames'])}")
    check(r['signature'] == 'svcBreak in _ZN2nn3hid18GetNpadJoyHoldTypeEv', r['signature'])
    check(r['failure']['fatal'].startswith('The game stopped'), 'the fatal message')
    ipc = {(c['kind'], c['interface'], c['command']): c['count'] for c in r['ipc']}
    check(
        ipc
        == {
            ('undeclared', 'nn::audio::detail::IAudioDevice', 7): 2,
            ('unknown', 'nn::hid::IHidServer', 92): 1,
            ('unknown control', 'nn::foo::IBar', 4): 1,
        },
        f'ipc: {ipc}',
    )
    check(any('texture format' in w['message'] for w in r['warnings']), 'warnings')
    check(r['game_output'] == ['loading ~/save.bin'], f'game output, its path shortened: {r["game_output"]}')

    old = logscan.scan(OLD_SVC_BREAK)
    check(old['title'] == 'Some Game', f"the older header's folder is dropped: {old['title']!r}")
    check(old['build'] is None and old['title_id'] is None, 'an older log has no build or title id')
    check(old['signature'] == r['signature'], f'the same failure, the same signature: {old["signature"]}')

    c = logscan.scan(CRASH_IN_GAME)
    check(
        c['outcome'] == 'crash' and c['failure']['signal'] == '11' and c['failure']['game_address'] == '10',
        'crash fields',
    )
    check(c['failure']['place'] == {'kind': 'game', 'detail': ['8123450']}, f'crash place: {c["failure"]["place"]}')
    check(c['failure']['function']['name'] == '_ZN4Game6Render4DrawEv', 'a crash is pinned on its pc')
    check(c['signature'] == 'crash (invalid memory access) in _ZN4Game6Render4DrawEv', c['signature'])
    check(any('memory_gb=6' in d for d in c['failure']['details']), 'the crash report hint is kept')

    rt = logscan.scan(CRASH_IN_RUNTIME)
    check(rt['signature'] == 'crash (invalid memory access) in the runtime 1234abc +1a2b3c', rt['signature'])
    lib = logscan.scan(CRASH_IN_LIBRARY)
    check(lib['signature'] == 'crash (invalid memory access) in libnvidia-glcore.so.580 ?', lib['signature'])

    u = logscan.scan(UNDEFINED)
    check(u['outcome'] == 'unsupported instruction' and u['failure']['set'] == 'A64', 'A64 undefined instruction')
    check(u['signature'] == 'unsupported A64 instruction ce62c820', u['signature'])

    f = logscan.scan(FATAL_ONLY)
    check(
        f['outcome'] == 'fatal'
        and f['signature'] == 'fatal Could not load the game executables. See the .log for details.',
        f'fatal: {f["signature"]}',
    )

    la = logscan.scan(LAUNCHER)
    check(
        la['build'] == 'ScarletNX main 9999fff (clang 23.1.3, Windows x86_64)' and la['commit'] == '9999fff',
        f"the launcher's build: {la['build']}",
    )
    check(la['signature'] == r['signature'] and la['ipc'] == [], "only the game log's section is scanned")
    check(
        la['launcher']['message'] == 'The game stopped (svcBreak). See ~\\AppData\\x.log',
        f"the launcher's message, shortened: {la['launcher']['message']}",
    )
    check(
        all(len(t) <= logscan.LINE + 3 for t in logscan.scan(HEADER + '[game] ' + 'x' * 5000)['game_output']),
        'long lines are cut',
    )

    # nothing of the player's folders, the game's code or its memory in what is written
    for text in (SVC_BREAK, OLD_SVC_BREAK, CRASH_IN_GAME, LAUNCHER):
        out = json.dumps(logscan.scan(text))
        for leak in ('someone', 'f90007ff', '58cca', '00000000ca8c0500', 'ca8c0500'):
            check(leak not in out, f'{leak} copied into the summary')

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'game.log.gz'
        path.write_bytes(gzip.compress(SVC_BREAK.encode()))
        run = subprocess.run(
            [sys.executable, str(SCRIPT), str(path), '--format', 'markdown'], capture_output=True, text=True, check=True
        )
        check('**Signature:** `svcBreak in _ZN2nn3hid18GetNpadJoyHoldTypeEv`' in run.stdout, 'markdown from a .gz')
        check('IAudioDevice` cmd 7 (undeclared, 2x)' in run.stdout, 'markdown service commands')
        run = subprocess.run(
            [sys.executable, str(SCRIPT), str(path), str(path)], capture_output=True, text=True, check=True
        )
        check(len(run.stdout.splitlines()) == 2, 'json: a line a log')
    print('logscan_test: ok')


if __name__ == '__main__':
    main()
