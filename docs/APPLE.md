<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# macOS and iOS

Both build on a Mac with Xcode, for Apple silicon (arm64), with Xcode's Clang, and the CI builds both on every push
(`.github/workflows/build.yml`). They are new and have only been built by CI. No game has run on either yet, for the
reasons under [What is missing](#what-is-missing).

## The frontend

The menu and the runtime are SDL's window, as on Android. The parts the system does are a thin frontend over it, in the
same shape as Android's `MainActivity` and `src/gui/android.c`:

- `src/gui/apple.m` (Objective-C) is the menu's side: the data folder, the game the menu plays, and the SDL window's
  `UIWindow` or `NSWindow` for the Swift to present over.
- `dist/apple/Frontend.swift` (Swift) is the system's side, called from `apple.m` through `@_cdecl` functions: on iOS,
  the document picker SDL lacks there and the share sheet; on macOS, the Finder showing the saved log.

### macOS

`ScarletNX.app` is the desktop program in a bundle: `scarletnx` in `Contents/MacOS`, the themes and the
licenses in `Contents/share`, where an installed copy looks for them. It works as on Linux. The menu extracts and
recompiles through the driver as a child process, with SDL's own file and folder dialogs. Its data folder is
`~/Library/Application Support/scarletnx`. *Share log* saves the log and selects it in a Finder window. The app is
signed ad hoc, as Apple silicon runs no unsigned code. A copy from a download may need *Open* from the Finder's menu
the first time.

The Vulkan driver is MoltenVK, Ryujinx's build of it ([V380-Ori/Ryujinx.MoltenVK](https://github.com/V380-Ori/Ryujinx.MoltenVK)),
which carries the patches an emulator's pipelines need and mainline MoltenVK lacks. Both apps carry it in their
`Frameworks`, where the runtime has SDL load it (`backend/sdl3.c`). The bare `scarletnx` outside the app uses the
system's Vulkan loader instead (Homebrew's `molten-vk` and `vulkan-loader`).

### iOS

An iOS app runs one process and may not start another, so the iOS app works like Android's game activity without its
menu's driver:

- **The data folder** is the app's Documents, which the Files app (*On My iPhone > ScarletNX*) and the Finder show.
  Games go in its `recompiled/`, each as `recompiled/<id>/<id>_code.snxobj` with its `.ini` and `_data/`, prepared on
  a Mac ([Running a game on iOS](#running-a-game-on-ios)).
- **The menu** lists those games. *Load* and the folder settings open the system's document picker. Preparing a
  dump says it needs a computer: extraction and recompilation run the driver as a program of its own.
- **A game** runs in the app's process once the menu closes (`src/nx/runtime/entry.c`): the runtime starts once per
  process, so the app ends with the game.
- **Share log** opens the share sheet with `scarletnx.log`.
- **Logs.** Everything the app prints goes to `scarletnx-console.log` in its Documents, after a banner with the device,
  the system and the limits the app has, and a fatal error is also written to `scarletnx-error.log`. A phone shows no
  console, so these are what to read when a run fails.

The IPA is unsigned: AltStore, SideStore or a developer account signs it for the device, with
`dist/apple/ScarletNX.entitlements` (the larger address space and memory limit a game needs). It holds MoltenVK's
framework.

Code compiled while a game runs on macOS, 32-bit games' blocks and the runtime JIT's functions, goes through the
runtime's own loader (`src/common/cpu/recompiler/llvm/code_loader.c`). LLVM writes it as ELF objects with Darwin's
calling convention, and the loader places them in `MAP_JIT` memory, which each thread sees either writable or
executable. The app is signed without the hardened runtime, so `MAP_JIT` needs no entitlement.

### Running a game on iOS

A game is prepared on a **Mac** today, then copied to the iPhone. Nothing else in the process needs one.

1. **The app.** Take `ScarletNX-ios-arm64-<commit>.ipa` from a CI run, or build it (`cmake --preset ios`,
   [Building](#building)), and install it with a sideloading tool that signs it with an Apple ID: iloader or
   SideStore. A free account signs an app for 7 days, up to three apps at a time (the app, SideStore and StikDebug).
   Installing again removes the app's Documents with some tools, so the games are copied again after it.
2. **JIT.** iOS makes memory executable only for an app with a debugger attached. Install StikDebug and LocalDevVPN,
   give StikDebug the pairing file (SideStore keeps one: *Storage Explorer*, `ALTPairingFile.mobiledevicepairing`),
   hold ScarletNX in StikDebug's list, *Assign Script* `universal.js`, and *Enable JIT* before each run. The runtime
   checks for the debugger first and says what to enable when there is none.
3. **Prepare the game on a Mac**, where the recompiler has AArch64's code generator (a Windows or Linux x86 build
   has x86's alone, and `--target ios` fails there with "No available targets are compatible"):

   ```sh
   scarletnx -x "Game.nsp" --name Game
   scarletnx -r work/Game --target ios --outdir recompiled
   ```

   This writes `recompiled/<id>/` with `<id>_code.snxobj`, `<id>.ini` and `<id>_data/` (RomFS and ExeFS). 64-bit
   games only. The pack holds the module's code without the runtime library, which the app brings.
4. **Copy `recompiled/<id>/` to the iPhone**, into *ScarletNX > recompiled*: the Files app, the Finder, or a USB copy
   into the app's Documents.
5. **Run.** *Enable JIT* in StikDebug, open ScarletNX, *Games*, the game.

Limits found on an iPhone 14 Pro (iOS 26.6.1): without the paid extended-virtual-addressing entitlement the app has
about 13 GiB of address space and about 3 GB of memory, so the runtime takes the largest free run it can (128 GiB
down to 5 GiB) and lays the guest out to fit, with no alias region below 12 GiB.

## What is missing

Apple silicon's 16 KiB pages no longer stop the runtime: it commits guest memory in whole host pages, as on
PowerPC64 ([PowerPC64](./PPC64.md)), and views of memory are copies there, as Windows has them. What still keeps
games from running:

- **Game modules on a Mac, untested.** The recompiler links an arm64 Mach-O dylib
  (`src/common/cpu/recompiler/llvm/link_macho.c`) with the ad hoc signature ld64 gives one, which Apple silicon
  needs to run it. Its layout, rebases, exports and signature were checked on Linux against LLVM's tools and
  `ld64.lld`, and its code was run there under qemu. No Mac has loaded one yet.
- **Game modules on iOS, untested on a device.** iOS loads no code it did not ship with, so a game's code is a pack of
  objects (`scarletnx -r Game --target ios` writes `<id>_code.snxobj`, 64-bit games only) that the app loads into
  memory a debugger made executable (`load_pack` in `src/nx/runtime/main.c`, the JIT loader's iOS arena in
  `code_loader.c`). The pack is made on a computer, since nothing runs the recompiler inside the app yet.
- **32-bit games on iOS.** Their blocks are compiled at run time (`src/nx/runtime/arm.c`,
  `src/common/cpu/arm/exec.c`), which needs memory that becomes executable. iOS gives that only to an app that may JIT.
- **Preparing without a Mac.** Only a Mac makes the iOS pack today. Windows and Linux x86 builds carry x86's code
  generator alone, to keep their size. A pack could come from them with AArch64's code generator added to their LLVM,
  or from the iPhone once the driver runs in the app's process (its LLVM has AArch64's already).
- **The 3DS and Linux drivers** are not built for iOS (`SNX_BUILD_3DS`, `SNX_BUILD_GNUABI`), nor the Linux one for
  macOS.

## Building

LLVM's libraries first, once for each system, with AArch64's code generator alone, the one the recompiler uses
(about 10 minutes on an M1). `scripts/apple-llvm.sh` builds them, and the CI caches them:

```sh
scripts/apple-llvm.sh macos ~/apple-llvm/macos
scripts/apple-llvm.sh ios ~/apple-llvm/ios
```

macOS, then the app:

```sh
cmake --preset macos -DLLVM_DIR=$HOME/apple-llvm/macos/lib/cmake/llvm
cmake --build --preset macos
cmake --install build/macos --prefix stage
scripts/apple-app.sh macos stage out moltenvk   # out/ScarletNX.app
```

`moltenvk` is the release's `MoltenVK-macos.tar`, extracted (`MoltenVK-ios.tar` for iOS). The CI pins the release and
its SHA-256 (`.github/workflows/build.yml`). Homebrew's LLVM works too, with every code generator:
`-DSNX_LLVM_CONFIG="$(brew --prefix llvm)/bin/llvm-config"` in place of `LLVM_DIR`.

iOS, then the IPA:

```sh
cmake --preset ios -DLLVM_DIR=$HOME/apple-llvm/ios/lib/cmake/llvm
cmake --build --preset ios
cmake --install build/ios --prefix stage
scripts/apple-app.sh ios stage out moltenvk     # out/ScarletNX.ipa
```

`MACOS_VERSION` (default 13.0) and `IOS_VERSION` (default 16.0) are the oldest systems `apple-llvm.sh` builds for,
as the presets' `CMAKE_OSX_DEPLOYMENT_TARGET` are the apps'. FFmpeg, OpenSSL and the recompiler's runtime library are built outside
CMake's compiles and get the SDK, architecture and oldest version from `cmake/apple.cmake`.
