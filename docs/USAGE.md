<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Usage

Setup: [BUILD](BUILD.md). Checkout CWD; own dumps/keys.

## Extract → AOT → run

```sh
python3 scripts/extract.py "Game.xci" --name Game
python3 scripts/recomp.py work/Game Game
games/Game --fullscreen=0
```

```powershell
python scripts/extract.py "Game.xci" --name Game
python scripts/recomp.py work/Game Game
.\games\Game.exe --fullscreen=0
```

Remaining examples: POSIX.

```sh
# NSP + same-title update
python3 scripts/extract.py Game.nsp --update update.nsp --name Game \
  --keys /path/prod.keys --title-keys /path/title.keys

# Recompiler options go after --
python3 scripts/recomp.py work/Game Game -- --native --jobs 8
python3 -m scarletnx app.nro -o App --outdir games
python3 -m scarletnx --help
```

- **Keys:** flags → checkout → `~/.switch` (`%USERPROFILE%\.switch` on Windows). Common NSP tickets supplement title keys.
- **Extraction:** defaults to `work/<dump stem>`; `--name`/`--out DIR` override. Refuses existing `exefs`;
  builds missing hactool. [Temp cleanup/key filtering](../scripts/extract.py).
- **Recompile:** preset `default` build → AOT/NVN prebuild. Cached objects;
  NVN prebuild failures defer compilation to runtime. `--native` = `-march=native`;
  `--cc CLANG` / `-O 0..3` also accepted.
- **Data updates:** ExeFS refreshes; `Game_data/romfs` is reused.
  Replace cached RomFS after asset changes; preserve `Game_data/save/`.

| Output | Contents |
| --- | --- |
| `work/Game/{exefs,romfs}` | Extracted input |
| `games/Game[.exe]`, `Game_code.so/.dll` | Runtime + AOT module |
| `games/Game_data/`, `Game.ini` | Data/saves + settings |
| `games/Game_build/` | Generated C + object cache |
| `games/shader_cache/spirv/` | SPIR-V cache |

## AppImage / display

```sh
chmod +x ScarletNX-x86_64.AppImage
./ScarletNX-x86_64.AppImage --game=games/Game
SDL_VIDEO_DRIVER=x11 games/Game
```

`--game` selects `Game_code.so`/`Game_data/`; or name the image `games/Game.AppImage` to load adjacent files.
X11 bypasses Wayland. [Licenses](CREDITS.md).

## Settings / input

`Game.ini`: help rewritten at startup, most settings hot-reload. `--key=value` overrides one run.
Restart for `docked`, `language`, `fullscreen`, `resolution`, `memory_gb`, `heap_max_mb`, `profile`.

| Setting | Effect |
| --- | --- |
| `docked=1`, `fullscreen=0` | TV mode, windowed |
| `resolution=1` | Native scale; range 0.25–4 |
| `fps_limit=1` | 60 FPS; `0` uncapped; `60..240` explicit cap |
| `shadows=0` | Skip shadow maps |
| `language=1` | English (US); other IDs in INI |
| `log=2` | Verbose, adds runtime overhead |

SDL gamepads use Switch positions; pads + keyboard share one guest controller.

| Keyboard | Switch |
| --- | --- |
| Arrows; WASD / IJKL | D-pad; L/R sticks |
| X or Space / Z / C / V | A / B / X / Y |
| Q / E / 1 / 3 | L / R / ZL / ZR |
| Enter / Backspace | Plus / Minus |

F1 help; F3 screenshot; F4 FPS cap; F5 thread dump; F11/Alt+Enter fullscreen; Escape quit.
Beside the executable: `Game.log`, `threads.txt` (15 s refresh), `captures/` (screenshots).
Scripts/disasm/shaders: [DEVELOPMENT](DEVELOPMENT.md).
