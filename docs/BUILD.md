<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Build

Run from checkout root. [Extract/AOT/run](USAGE.md) · [Tests/diagnostics](DEVELOPMENT.md).

## Prerequisites

- **Linux / FreeBSD:** Clang (CI: 23; FreeBSD's system Clang also works), CMake ≥3.25, Ninja,
  POSIX shell, GNU make (`gmake` on FreeBSD), NASM on x86-64, Git, SDL3 dev files.
- **Windows:** Win10/11 x64 + Git. Bootstrap stages SHA-256-checked llvm-mingw/Clang, CMake, Ninja,
  NASM and w64devkit in `deps/tools`.
- **Display:** Vulkan 1.3 driver/GPU. Headless builds omit SDL3/Vulkan.
- **FFmpeg:** avoid spaces in checkout/cache paths.

## Linux / FreeBSD

```sh
git clone https://github.com/scarletnx/ScarletNX.git && cd ScarletNX &&
cmake --preset default -B build && cmake --build build
# -> build/{scarletnx,hactool,snx-shaderc}
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
    cmake --preset default -B build
    if ($LASTEXITCODE) { throw 'Configure failed' }
    cmake --build build
    if ($LASTEXITCODE) { throw 'Build failed' }
}
# -> build\{scarletnx.exe,hactool.exe,snx-shaderc.exe}
```

Each new shell, from the checkout:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
. .\scripts\env.ps1
```

`env.ps1 -Isolated` restricts PATH to bundled tools, Git and Windows.

## Rebuild, deploy, cache

```sh
cmake --build build
```

Default: Ninja + Clang, `RelWithDebInfo` (`-O2 -g`). Runtime relinks deploy `games/Game[.exe]` beside
matching `Game_code.so/.dll`; no-op builds skip deployment. Recompiler/ABI edits require [module rebuilds](DEVELOPMENT.md).

CPM: [SHA-256-pinned sources](../cmake/dependencies.cmake), shared `.cache/cpm` cache, offline configure when populated.
Override via CMake/env `CPM_SOURCE_CACHE`. SDL3: system library on Unix, static build on Windows.

## Build variants

Persistent per-build-dir options (POSIX examples):

```sh
cmake --preset default -B build -DSNX_FFMPEG_ASM=OFF && cmake --build build
```

- `-DSNX_FFMPEG_ASM=OFF`: omit x86-64 NASM assembly; slower FFmpeg decoding.
- `-DSNX_H264=OFF`: disable FFmpeg, including NVDEC video and hwopus Opus decoding.
- `-DSNX_DEPLOY=OFF`: disable runtime deployment.
- `-DSNX_GAMES_DIR=/path`: override deployment dir (default: `games/`).
- `-DCPM_SOURCE_CACHE=/path`: override source-download cache.

```sh
# Vulkan without SDL3, a display server, a surface or a swapchain:
cmake --preset default -B build/headless-vk -DSNX_BACKEND=headless -DSNX_VULKAN=ON -DSNX_DEPLOY=OFF &&
cmake --build build/headless-vk
# -> build/headless-vk/scarletnx, Vulkan renderer, always headless

# Lightweight null renderer (does not render the game's 3D graphics):
cmake --preset default -B build/headless -DSNX_BACKEND=headless -DSNX_VULKAN=OFF -DSNX_DEPLOY=OFF &&
cmake --build build/headless
# -> build/headless/scarletnx, no SDL3/Vulkan or game deployment

cmake --preset windows-cross && cmake --build --preset windows-cross
# -> build/windows-cross/scarletnx.exe; requires llvm-mingw on PATH
```

The normal SDL3 build also supports `--headless` at runtime, with `--headless=0` returning to windowed startup.
The no-SDL3 builds cannot switch to a window. [Software Vulkan / lavapipe setup](USAGE.md#headless-vulkan--lavapipe).

Offscreen integration test, with an installed Vulkan 1.3 ICD (lavapipe works):

```sh
cmake --preset default -B build/headless-vk -DSNX_BACKEND=headless -DSNX_VULKAN=ON \
  -DSNX_DEPLOY=OFF -DSNX_TEST_VULKAN=ON && cmake --build build/headless-vk
# Set VK_DRIVER_FILES to your lavapipe manifest to force CPU rendering.
SNX_VK_VALIDATION=1 ctest --test-dir build/headless-vk -R vulkan_headless --output-on-failure
```

The test uses synthetic pixels, not a game. It checks offscreen submissions, frame recycling, cropping,
readback hashes and PPM contents without WSI. Validation requires the Khronos validation layer to be installed.
`SNX_TEST_VULKAN` is off by default so ordinary unit tests do not require an ICD.

Cross-builds omit host hactool. Use a native driver for `--extract`; `--recompile --target win64`
uses llvm-mingw and builds the cross runtime in `build/win64-recomp`.

Host tools only, without SDL/Vulkan/FFmpeg or Python:

```sh
cmake --preset default -B build/tools -DSNX_TOOLS_ONLY=ON && cmake --build build/tools
build/tools/scarletnx --recompile --help
ctest --test-dir build/tools --output-on-failure
```

Extraction builds missing hactool separately. Recompilation deploys the running full runtime;
the tools-only build creates one in `build/native-recomp`, or accepts `--runtime FILE`.
`--repo DIR` and `--build-dir DIR` before the command override compiled-in checkout/tool paths.
Python ≥3.12 is optional for `tools/*.py` diagnostics.

## Install / AppImage

```sh
cmake --install build --prefix stage
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
