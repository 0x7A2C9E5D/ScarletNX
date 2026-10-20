#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""engines.c's g_m3d_acts names every 3D method below the macros' that does more than store its value.
maxwell3d_method stores any other method without its switch, so a case added to the switch, to keeps_draw or to
shadowed's exceptions without its entry in the table would be skipped silently. Source only, no compiler."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def body(src, start):
    """The text of the function whose definition starts with start, to its closing brace at column 0."""
    at = src.index(start)
    return src[at:src.index('\n}\n', at)]


def main():
    src = (ROOT / 'src/nx/gpu/maxwell/engines.c').read_text()
    table_src = src[src.index('static const u8 g_m3d_acts'):]
    table_src = table_src[:table_src.index('};')]
    table = {int(m, 16) for m in re.findall(r'\[(0x[0-9A-Fa-f]+)\] = 1', table_src)}
    cases = {}
    for name, start in (('the method switch', 'void maxwell3d_method('), ('keeps_draw', 'static int keeps_draw(')):
        found = {int(m, 16) for m in re.findall(r'case (0x[0-9A-Fa-f]+):', body(src, start))}
        if len(found) < 10:
            sys.exit(f'{name}: only {len(found)} case labels found, the parse no longer fits engines.c')
        cases[name] = found
    shadowed = body(src, 'static int shadowed(')
    cases['shadowed'] = {int(m, 16) for m in re.findall(r'm != (0x[0-9A-Fa-f]+)', shadowed)}
    for lo, hi in re.findall(r'm >= (0x[0-9A-Fa-f]+) && m <= (0x[0-9A-Fa-f]+)', shadowed):
        cases['shadowed'] |= set(range(int(lo, 16), int(hi, 16) + 1))
    missing = []
    for name, found in cases.items():
        missing += [f'{m:#x} ({name})' for m in sorted(found) if m < 0xE00 and m not in table]
    if missing:
        sys.exit('g_m3d_acts lacks methods that act: ' + ', '.join(missing))
    print(f'g_m3d_acts covers all {len(set().union(*cases.values()))} methods that act')


if __name__ == '__main__':
    main()
