<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Usage

The [README](../README.md) covers extraction, recompilation and launch.
Extraction creates `work/Game/{exefs,romfs}`. Recompilation creates the runtime copy,
`Game_code.so` (`.dll` on Windows), `Game_data/` and `Game.ini` in `games/`.

Apply a same-title update during extraction with `--update update.nsp`.
Use `--keys /path/prod.keys` and `--title-keys /path/title.keys` to override key discovery.
For an NRO, run `python3 -m scarletnx app.nro -o App --outdir games` directly.

## Controls

SDL gamepads map buttons by their position on a Switch controller. All connected pads and
the keyboard currently drive one guest controller.

| Keyboard | Switch control |
| --- | --- |
| Arrows | D-pad |
| WASD / IJKL | Left / right stick |
| X or Space / Z / C / V | A / B / X / Y |
| Q / E / 1 / 3 | L / R / ZL / ZR |
| Enter / Backspace | Plus / Minus |

F1 shows help, F3 saves a screenshot, F4 toggles the FPS limit, F5 logs thread states,
F11 or Alt+Enter toggles fullscreen, and Escape quits.
On Wayland, `SDL_VIDEO_DRIVER=x11` selects X11.

## Settings and logs

The runtime rewrites `Game.ini` with option help at startup. Most changes apply when saved.
`docked`, `language`, `fullscreen`, `resolution`, `memory_gb`, `heap_max_mb` and `profile`
require a restart. `--key=value` overrides a setting for the current run.

| Setting | Meaning |
| --- | --- |
| `docked=1` | TV mode instead of handheld |
| `fullscreen=0` | Start in a window |
| `resolution=1` | Native internal resolution, range 0.25 to 4 |
| `fps_limit=1` | Cap at 60 FPS. `0` is unlimited, `60` to `240` selects a cap. |
| `shadows=0` | Skip shadow maps |
| `language=1` | English (US). The INI lists other values. |
| `log=2` | Verbose diagnostics, with additional runtime overhead |

`Game.log` contains diagnostics. `threads.txt`, rewritten every 15 seconds, records guest
thread states. Screenshots go into `captures/` beside the executable.
Include the triggering scene and relevant log lines in bug reports. Keep dumps and keys local.
