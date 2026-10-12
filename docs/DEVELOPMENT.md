<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Development

See [architecture](ARCHITECTURE.md) for component ownership and [build options](BUILD.md)
for rebuilding. `CLAUDE.md` contains repository rules for coding agents.
Commands below assume POSIX. Use the equivalent Python/path spelling on Windows.

| Task | Command |
| --- | --- |
| Format / check formatting | `scripts/format.sh` / `scripts/format.sh --check` |
| Test indexed filesystems | `python3 tests/run_filesystem_tree.py --cc clang --sanitize` |
| Name guest addresses | `python3 tools/symbols.py work/Game cf34380` |
| Disassemble a function | `python3 tools/disasm.py work/Game NvRmStreamFlushSync` |
| Scripted run and screenshot | `./games/Game --seconds=60 --script=15000:400,16000:1 --shot=out` |
| Repeat runs and summarize | `python3 tools/run_game.py Game --runs 5 --dump work/Game` |
| Record translated shaders | `SNX_SHADER_CORPUS=dir ./games/Game` |
| Validate a shader corpus | `build/default/snx-shaderc dir --validate` |
| Precompile NVN shader files | `build/default/snx-shaderc --nvn DIR --cache games/shader_cache/spirv` |

Formatting requires clang-format 23 and `uv` for the pinned Ruff formatter.
Shader validation requires `spirv-val`. Recompilation already precompiles supported NVN shaders.
Script entries are milliseconds plus a hexadecimal button mask. Presses last 300 ms by default
and can be changed with `--press_len`.

## Verification

CI checks formatting, builds on Linux and Windows, syntax-checks ABI helpers, and runs
filesystem regression tests and Python syntax/help checks. It does not run a game.

Verify runtime fixes through the affected scene. Repeat intermittent-crash tests, compare
screenshots when relevant, and record what was tested. Do not rebuild during timing-sensitive
runs because builds deploy runtimes. Add synthetic regression tests for semantics and malformed inputs.

## Upstream imports

```bash
scripts/import-upstream.sh SwitchRecomp_v143.zip v143
```

The `upstream` branch stores each MZ Labs release and its formatted copy. Tags `upstream-vN`
point to original imports. Port changes into `main` by hand because the layouts differ.
The fork started from `upstream-v142`.

Keep game files, keys, generated modules and third-party build trees out of Git.
Preserve provenance in file headers and [credits](CREDITS.md).
