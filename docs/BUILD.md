<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Build

Run from checkout root. [Extract/AOT/run](USAGE.md) · [Tests/diagnostics](DEVELOPMENT.md).

## Prerequisites

- **Linux / FreeBSD:** Clang (CI: 23; FreeBSD's system Clang also works), CMake ≥3.25, Ninja,
  POSIX shell, GNU make (`gmake` on FreeBSD), NASM on x86-64, Python ≥3.12, Git, SDL3 dev files.
- **Windows:** Win10/11 x64 + Git. Bootstrap stages SHA-256-checked llvm-mingw/Clang, CMake, Ninja,
  NASM, Python and w64devkit in `deps/tools`.
- **Display:** Vulkan 1.3 driver/GPU. Headless builds omit SDL3/Vulkan.
- **FFmpeg:** avoid spaces in checkout/cache paths.

## Linux / FreeBSD

```sh
git clone https://github.com/scarletnx/ScarletNX.git && cd ScarletNX &&
cmake --preset default && cmake --build --preset default
# -> build/default/{scarletnx,hactool,snx-shaderc}
```

## Windows / PowerShell

```powershell
& {
    $ErrorActionPreference = 'Stop'
    git clone https://github.com/scarletnx/ScarletNX.git
    if ($LASTEXITCODE) { throw 'Clone failed' }
    Set-Location ScarletNX
    powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
    if ($LASTEXITCODE) { throw 'Bootstrap failed' }
    Set-ExecutionPolicy -Scope Process Bypass
    . .\scripts\env.ps1
    cmake --preset default
    if ($LASTEXITCODE) { throw 'Configure failed' }
    cmake --build --preset default
    if ($LASTEXITCODE) { throw 'Build failed' }
}
# -> build\default\{scarletnx.exe,hactool.exe,snx-shaderc.exe}
```

Each new shell, from the checkout:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
. .\scripts\env.ps1
```

`env.ps1 -Isolated` restricts PATH to bundled tools, Git and Windows.

## Rebuild, deploy, cache

```sh
cmake --build --preset default
```

Default: Ninja + Clang, `RelWithDebInfo` (`-O2 -g`). Runtime relinks deploy `games/Game[.exe]` beside
matching `Game_code.so/.dll`; no-op builds skip deployment. Recompiler/ABI edits require [module rebuilds](DEVELOPMENT.md).

CPM: [SHA-256-pinned sources](../cmake/dependencies.cmake), shared `.cache/cpm` cache, offline configure when populated.
Override via CMake/env `CPM_SOURCE_CACHE`. SDL3: system library on Unix, static build on Windows.

## Build variants

Persistent per-build-dir options (POSIX examples):

```sh
cmake --preset default -DSNX_FFMPEG_ASM=OFF && cmake --build --preset default
```

- `-DSNX_FFMPEG_ASM=OFF`: omit x86-64 NASM assembly; slower FFmpeg decoding.
- `-DSNX_H264=OFF`: disable FFmpeg, including NVDEC video and hwopus Opus decoding.
- `-DSNX_DEPLOY=OFF`: disable runtime deployment.
- `-DSNX_GAMES_DIR=/path`: override deployment dir (default: `games/`).
- `-DCPM_SOURCE_CACHE=/path`: override source-download cache.

```sh
cmake --preset default -B build/headless -DSNX_BACKEND=headless -DSNX_DEPLOY=OFF &&
cmake --build build/headless
# -> build/headless/scarletnx, no SDL3/Vulkan or game deployment

cmake --preset windows-cross && cmake --build --preset windows-cross
# -> build/windows-cross/scarletnx.exe; requires llvm-mingw on PATH
```

Cross-builds omit host hactool. `scripts/recomp.py` always uses the native `default` preset.

## Install / AppImage

```sh
cmake --install build/default --prefix stage
# -> stage/bin/, stage/share/licenses/scarletnx/
```

AppImage: run inside pkgforge's Arch container with `quick-sharun` ([CI setup](../.github/workflows/build.yml)):

```sh
cmake --preset default -B build/appimage -DSNX_DEPLOY=OFF -DCMAKE_INSTALL_PREFIX=/usr &&
cmake --build build/appimage &&
cmake --install build/appimage &&
GITHUB_REPOSITORY=scarletnx/ScarletNX scripts/make-appimage.sh
# -> dist/ScarletNX-<arch>.AppImage
```

[Nightly AppImage](https://github.com/fuddlesworth/SwitchRecomp/releases/tag/nightly) · [Launch](USAGE.md).

[Licenses/credits](CREDITS.md) · [Architecture](ARCHITECTURE.md) · [Roadmap](ROADMAP.md).
