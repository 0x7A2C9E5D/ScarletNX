# Build + run

## Unix — Linux / FreeBSD

```sh
git clone https://github.com/scarletnx/ScarletNX.git
cd ScarletNX
cmake --preset default && cmake --build --preset default
# -> build/default/{scarletnx,hactool,snx-shaderc}

cmake -S scripts -B build/host-tools -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/host-tools
# -> build/host-tools/{snx-extract,snx-recomp}

# Game.xci + prod.keys in checkout root
build/host-tools/snx-extract "Game.xci" --name Game
# -> work/Game/{exefs,romfs}
build/host-tools/snx-recomp work/Game Game
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

cmake -S scripts -B build/host-tools -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build/host-tools
# -> build\host-tools\{snx-extract.exe,snx-recomp.exe}

# Game.xci + prod.keys in checkout root
.\build\host-tools\snx-extract.exe "Game.xci" --name Game
# -> work\Game\{exefs,romfs}
.\build\host-tools\snx-recomp.exe work\Game Game
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
- **Host tools:** separate C90 build under `scripts/`; not part of the root build. They need the checkout.
  The helpers are C90; the recompiler remains Python.

New Windows shell:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
. .\scripts\env.ps1
```

`env.ps1 -Isolated` restricts PATH to bundled tools, Git + Windows.

## Inputs + outputs

Use dumps + keys from your own console; none are included.

- `snx-extract`: XCI/NSP → ExeFS + RomFS via `build/default/hactool`; builds hactool if missing.
- Keys: explicit `--keys` / `--title-keys`, else checkout root → `~/.switch` (Windows: `%USERPROFILE%\.switch`).
  Common NSP tickets supplement title keys; malformed prod-key lengths are filtered from the temporary copy.
- Extraction refuses an existing `exefs`. Temp storage: system temp if free space ≥1.5× inputs, else output parent.
  Failure removes temp files and newly created output, not a pre-existing output directory.
- `snx-recomp`: default-preset runtime build → ARM64-to-C AOT → native module → NVN shader prebuild.
  Object caching avoids unchanged chunks. Shader-prebuild failures are nonfatal; missing shaders compile during play.
- `games/Game[.exe]`: runtime; `Game_code.so/.dll`: game module; `Game_data/`: game data + saves;
  `Game.ini`: settings; `Game_build/`: generated code + object cache; `shader_cache/spirv/`: shader cache.

Runtime-only rebuild, from checkout root:

```sh
cmake --build --preset default
# -> refreshes runtime copies in games/
```

Recompiler or `runtime/abi/` edits require rerunning `snx-recomp`.

## Extra options

Examples below use Unix paths; Windows host tools are under `build\host-tools\` with `.exe`.

### Extraction + recompile

```sh
build/host-tools/snx-extract Game.nsp --update update.nsp --name Game \
  --keys /path/prod.keys --title-keys /path/title.keys
# -> work/Game/{exefs,romfs}, update applied

build/host-tools/snx-recomp work/Game Game -- --native --jobs 8
# -> game module optimized for this CPU, up to 8 compile jobs

python3 -m scarletnx --help
# -> full recompiler options (Windows: python)
```

- `--update`: same title as the base; apply during extraction, not at runtime.
- `--name`: defaults to dump stem. `--out DIR`: overrides `work/NAME`.
- Recompiler flags follow `--`: `--native` adds `-march=native`; `--cc` overrides Clang; `-O` selects optimization.
- `SNX_PYTHON`: Python executable; defaults to `python3` on Unix, `python` on Windows.
  `SNX_REPO`: checkout override for either host tool.

### Build variants

```sh
cmake --preset default -DSNX_FFMPEG_ASM=OFF
# -> no NASM requirement; slower videodec

cmake --preset default -DSNX_H264=OFF
# -> no FFmpeg; black video + no Opus decode

cmake --preset default -B build/headless -DSNX_BACKEND=headless -DSNX_DEPLOY=OFF
cmake --build build/headless
# -> headless runtime; no SDL3/Vk; normal game runtime copies untouched

cmake --preset windows-cross
cmake --build --preset windows-cross
# -> Windows runtime in build/windows-cross/; llvm-mingw must be on PATH
```

Configure flags persist in their build directory. Build changed `default` settings with
`cmake --build --preset default`. Cross-build excludes host hactool; `snx-recomp` uses the native `default` preset,
not `windows-cross`.

`SNX_DEPLOY=OFF` disables runtime copy-out; `SNX_GAMES_DIR` changes its destination.

### Linux AppImage + display

```sh
chmod +x ScarletNX-x86_64.AppImage
./ScarletNX-x86_64.AppImage --game=games/Game
# -> loads games/Game_code.so + games/Game_data/

SDL_VIDEO_DRIVER=x11 games/Game
# -> X11 instead of Wayland
```

[Nightly AppImage](https://github.com/fuddlesworth/SwitchRecomp/releases/tag/nightly): bundled runtime, not an
extractor/recompiler. `--game` takes a recompiled game path, not XCI/NSP.
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
# -> prebuilt SPIR-V cache; snx-recomp normally handles this
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
