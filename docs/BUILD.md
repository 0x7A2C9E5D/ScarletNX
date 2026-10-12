# Build + run

## Unix — Linux / FreeBSD

```sh
git clone https://github.com/scarletnx/ScarletNX.git
cd ScarletNX
cmake --preset default && cmake --build --preset default
# -> build/default/{scarletnx,hactool,snx-shaderc}

# Game.xci + prod.keys in checkout root
python3 scripts/extract.py "Game.xci" --name Game
# -> work/Game/{exefs,romfs}
python3 scripts/recomp.py work/Game Game
# -> games/{Game,Game_code.so,Game_data/,Game.ini}
(cd games && ./Game)
```

## Windows — PowerShell

```powershell
git clone https://github.com/scarletnx/ScarletNX.git
cd ScarletNX
powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
Set-ExecutionPolicy -Scope Process Bypass
. .\scripts\env.ps1

cmake --preset default
cmake --build --preset default
# -> build\default\{scarletnx.exe,hactool.exe,snx-shaderc.exe}

# Game.xci + prod.keys in checkout root
python scripts\extract.py "Game.xci" --name Game
# -> work\Game\{exefs,romfs}
python scripts\recomp.py work\Game Game
# -> games\{Game.exe,Game_code.dll,Game_data\,Game.ini}
Push-Location games
.\Game.exe
Pop-Location
```

## Requirements + build behavior

- **Unix:** Clang 23 (FreeBSD's Clang also works), CMake ≥3.25, Ninja, GNU make (`gmake` on FreeBSD),
  NASM on x86-64, Python ≥3.12, Git, SDL3 development files. Display: Vk 1.3.
- **Windows:** Win10/11 x64, Git, Vk 1.3. Bootstrap installs Clang/llvm-mingw, CMake, Ninja, NASM, Python +
  w64devkit under `deps/tools`, SHA-256 checked; no system install.
- **Paths:** avoid spaces in checkout/cache paths when building FFmpeg.
- **Runtime deps:** CPM fetches pinned, SHA-256-checked sources into `.cache/cpm` (`CPM_SOURCE_CACHE` overrides).
  SDL3 is system-provided on Unix, built on Windows. Pins: [`cmake/dependencies.cmake`](../cmake/dependencies.cmake).
- **Extraction + recompilation:** Python scripts under `scripts/`; run them from the checkout.

New Windows shell:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
. .\scripts\env.ps1
```

`env.ps1 -Isolated` restricts PATH to bundled tools, Git + Windows.

## Inputs + outputs

Use dumps + keys from your own console; none are included.

- `scripts/extract.py`: XCI/NSP → ExeFS + RomFS via `build/default/hactool`; builds hactool if missing.
- Keys: explicit `--keys` / `--title-keys`, else checkout root → `~/.switch` (Windows: `%USERPROFILE%\.switch`).
  Common NSP tickets supplement title keys; malformed prod-key lengths are filtered from the temporary copy.
- Extraction refuses an existing `exefs`. Temp storage: system temp if free space ≥1.5× inputs, else output parent.
  Failure removes temp files and newly created output, not a pre-existing output directory.
- `scripts/recomp.py`: default-preset runtime build → ARM64-to-C AOT → native module → NVN shader prebuild.
  Object caching avoids unchanged chunks. Shader-prebuild failures are nonfatal; missing shaders compile during play.
- `games/Game[.exe]`: runtime; `Game_code.so/.dll`: game module; `Game_data/`: game data + saves;
  `Game.ini`: settings; `Game_build/`: generated code + object cache; `shader_cache/spirv/`: shader cache.

Runtime-only rebuild, from checkout root:

```sh
cmake --build --preset default
# -> refreshes runtime copies in games/
```

Recompiler or `runtime/abi/` edits require rerunning `scripts/recomp.py`.

## Extra options

Examples below use Unix paths; on Windows, use `python` in place of `python3`.

### Extraction + recompile

```sh
python3 scripts/extract.py Game.nsp --update update.nsp --name Game \
  --keys /path/prod.keys --title-keys /path/title.keys
# -> work/Game/{exefs,romfs}, update applied

python3 scripts/recomp.py work/Game Game -- --native --jobs 8
# -> game module optimized for this CPU, up to 8 compile jobs

python3 -m scarletnx --help
# -> full recompiler options (Windows: python)
```

- `--update`: same title as the base; apply during extraction, not at runtime.
- `--name`: defaults to dump stem. `--out DIR`: overrides `work/NAME`.
- Recompiler flags follow `--`: `--native` adds `-march=native`; `--cc` overrides Clang; `-O` selects optimization.

### Build variants

Pass options when configuring, then build the same folder:

```sh
cmake --preset default -DSNX_FFMPEG_ASM=OFF
cmake --build --preset default
```

- `SNX_FFMPEG_ASM=OFF`: Build FFmpeg without NASM. Video decoding is slower.
- `SNX_H264=OFF`: Disable FFmpeg, including NVDEC video and hwopus Opus decoding.
- `SNX_DEPLOY=OFF`: Disable copying runtimes to the games folder after builds.
- `SNX_GAMES_DIR=/path`: Set the deployment folder.
- `CPM_SOURCE_CACHE=/path`: Set the shared download cache.

```sh
cmake --preset default -B build/headless -DSNX_BACKEND=headless -DSNX_DEPLOY=OFF
cmake --build build/headless
# -> headless runtime; no SDL3/Vk; normal game runtime copies untouched

cmake --preset windows-cross
cmake --build --preset windows-cross
# -> Windows runtime in build/windows-cross/; llvm-mingw must be on PATH
```

Configure flags persist in their build directory. Build changed `default` settings with
`cmake --build --preset default`. Cross-build excludes host hactool; `scripts/recomp.py` uses the native `default` preset,
not `windows-cross`.

### Linux AppImage + display

```sh
chmod +x ScarletNX-x86_64.AppImage
./ScarletNX-x86_64.AppImage --game=games/Game
# -> loads games/Game_code.so + games/Game_data/

SDL_VIDEO_DRIVER=x11 games/Game
# -> X11 instead of Wayland
```

[Nightly AppImage](https://github.com/fuddlesworth/SwitchRecomp/releases/tag/nightly): bundled runtime and libraries.
Alternatively, name it `games/Game.AppImage` to select the adjacent module. Packaging:
[`scripts/make-appimage.sh`](../scripts/make-appimage.sh), using pkgforge/quick-sharun after install into `/usr`.

### Settings + logs

Edit `games/Game.ini`; runtime rewrites it with option descriptions at startup. Most settings hot-reload.
Restart for `docked`, `language`, `fullscreen`, `resolution`, `memory_gb`, `heap_max_mb`, `profile`.

- `docked=1`: TV mode; `0`: handheld.
- `fullscreen=0`: windowed; default `1`.
- `resolution=1`: internal scale, 0.25–4.
- `fps_limit=1`: 60 FPS cap; `0`: uncapped.
- `shadows=0`: skip shadow maps.
- `language=1`: English (US); IDs listed in INI.
- `log=2`: verbose diagnostics.

Logs: `games/Game.log`; thread snapshots: `games/threads.txt`, refreshed every 15s.

### Controls

SDL controllers use Switch button positions. Keyboard:

- D-pad: arrows; sticks: WASD / IJKL.
- A: X/Space; B: Z; X: C; Y: V; L/R: Q/E; ZL/ZR: 1/3; Plus/Minus: Enter/Backspace.
- F1: help; F3: screenshot; F4: FPS cap; F5: thread dump; F11/Alt+Enter: fullscreen; Escape: quit.

### Diagnostics + shaders

```sh
(cd games && ./Game --seconds=60 --script=15000:400,16000:1 --shot=out)
# -> timed run + games/out.ppm; scripted presses at 15s/16s

python3 tools/run_game.py Game --runs 5 --dump work/Game
# -> per-run logs/screenshots + exit, crash site, frame/FPS summary

python3 tools/symbols.py work/Game cf34380
# -> guest addr symbol
python3 tools/disasm.py work/Game NvRmStreamFlushSync
# -> guest disasm

SNX_SHADER_CORPUS=corpus games/Game
build/default/snx-shaderc corpus --validate
# -> recorded shader validation; requires spirv-val

build/default/snx-shaderc --nvn DIR --cache games/shader_cache/spirv
# -> prebuilt SPIR-V cache; scripts/recomp.py normally handles this
```

`--script` entries are `milliseconds:hexbuttons`. `run_game.py` defaults to its game-specific New Game sequence;
use `--script ""` to disable it. Windows: `python` + `.exe`; set env vars with `$env:NAME='value'`.

## Development + references

```sh
scripts/format.sh --check
# -> C/C++/Python formatting check; clang-format 23 + uv required
scripts/import-upstream.sh SwitchRecomp_v143.zip v143
# -> tagged upstream import; port changes to main manually
```

Architecture: [ARCHITECTURE.md](ARCHITECTURE.md). Status: [ROADMAP.md](../ROADMAP.md).
Licensing: [GPL-3.0-or-later](../LICENSE), upstream [Apache-2.0](../LICENSE.apache), [NOTICE](../NOTICE).
Installed dependency licenses: `share/licenses/scarletnx`; AppImage bundled-library licenses: `share/licenses/`.
