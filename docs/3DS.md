<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Nintendo 3DS port

## Status: stage 1 only

`scarletnx-3ds` is a **board/launcher foundation**, not a working 3DS emulator
or CIA-to-native compiler. Stages 2–6 below are not implemented. No CIA contents,
firmware, keys, Azahar CPU interpreter/JIT, DSP or PICA renderer are included.

Implemented:

- `SNX_BUILD_3DS` (default ON), independently buildable/installable `scarletnx-3ds`.
- `src/3ds/board/`: Old/New 3DS physical-region descriptions (128/256 MiB
  FCRAM, VRAM, DSP RAM, AXI WRAM, IO/MPCore, New 3DS extra RAM), hardware
  ARM11 core counts, base-clock tick accounting, reset/shutdown lifecycle.
- Physical-range lookup rejects empty, crossing, out-of-region and wrapping
  ranges. These descriptions allocate **no memory** and expose no host pointers.
- `scarletnx-gui`: **Load 3DS CIA**, driver discovery beside the GUI then on
  PATH, existing drop/process/output handling, CIA-only browser in `games/`, and
  a distinct `3ds` driver in saved recents. Uppercase `.CIA` and non-executable
  CIA files are supported. A CIA is never launched as a native executable or
  passed to the Switch/Linux driver. Switch settings are not forwarded.

The driver accepts `--model old|new` (default Old 3DS), `--board-info`, `--help`
and one CIA input (use `--` for a filename beginning with `-`). `--board-info`
prints physical addresses, not process virtual addresses. Exit codes:

- 0: help or board information.
- 2: invalid arguments or missing/unreadable input.
- 3: CIA pipeline is not implemented yet. This is an intentional hard failure,
  not success. No extraction/cache/output directories are created and no
  `ScarletNX - ...` launch banner is printed.

### Build and test

With the normal Clang/CMake prerequisites (no LLVM SDK, SDL or media dependencies
needed for the stage-1 board-only configuration):

```sh
cmake -S . -B build/3ds -DSNX_BUILD_NX=OFF -DSNX_BUILD_GNUABI=OFF -DSNX_BUILD_GUI=OFF
cmake --build build/3ds
ctest --test-dir build/3ds --output-on-failure
build/3ds/scarletnx-3ds --model new --board-info
```

Use `-DSNX_BUILD_GUI=ON` to include the launcher and its regression tests; SDL3
is then required/fetched as usual. `-DSNX_BUILD_3DS=OFF` omits the driver (the
GUI diagnoses its absence). CPU/backend execution tests are not registered in
a board/GUI-only configuration, because neither AOT runtime is configured.

## Source provenance and licensing

The initial shallow checkouts were:

- ScarletNX: `a9258f1eaa3a975f84ad2216950b3f62ddc92ba0`.
- Azahar: `68202bc4ff7edf9d7280ec0dcd6572b7cd1ae26e`.

Stage 1 uses physical-layout facts from Azahar `src/core/memory.h`, the tick
constants from `src/core/core_timing.h`, and the system-initialization design in
`src/core/core.cpp`. Board code is new C code; Azahar is not a build dependency.
Azahar's root license is GPLv3; individual files carry GPLv2-or-later or
GPLv3-or-later notices. Future adaptations must retain each source's notices
and record its origin/revision here; select the GPLv3-compatible terms where
applicable. Audit third-party/external components separately rather than assuming
that every dependency has the root license.

## Boundaries and prerequisites for stage 2

The 3DS application CPU is ARM11/ARMv6K with VFP, **not** the Cortex-A57
ARMv8-A AArch32 model currently implemented in `src/common/cpu/arm/`. That
backend's user-mode blocks also use a single flat `mem`/`mem_size` range.
Mapping a sparse 3DS address space into that ABI would make holes accessible and
would bypass process permissions, aliases and device callbacks.

Before running guest code:

1. Add an ARM11 ISA profile and validation (reject ARMv7/v8-only instructions,
   Thumb-2, NEON, etc.; verify ARM11 VFP/unaligned-access behavior).
2. Add an A32 memory-access ABI for translated process mappings with checked
   access and MMIO callbacks, including atomics/exclusives and multiword accesses.
   Do not pass an A64 `CPU*` or an NX module to an A32 block.
3. Define/version the CTR native-module ABI and AOT block discovery/dispatch,
   including ARM/Thumb state, indirect branches, execute checks and scheduling.
   No silent interpreter or runtime-JIT substitution for static recompilation.
4. Keep all CTR code under `src/3ds/`, with only `common` dependencies. Extract
   reusable host FS/tree operations from NX into common rather than linking the
   NX kernel/services/Maxwell runtime into CTR. Keep 3DS archive IDs, permissions,
   title identity and saves in the CTR adapter. Architecture tests enforce the
   runtime boundary.

The tick domain remains 268,111,856 Hz, with 4,481,136 base ticks per frame, as
in Azahar. New 3DS CPU scaling is a separate future scheduler concern. Stage 1
has no CPU execution, interrupts, event callbacks, ARM9 emulation or boot ROM.

## Remaining staged patches (not yet implemented)

Each stage should be one independently reviewable commit/diff, with all earlier
stages as its base. Stubs must return explicit unsupported errors, not pretend
that an unimplemented service/device succeeded.

### 2. CIA extraction, memory controller and static execution

Reference Azahar `src/core/file_sys/{cia_container,ticket,title_metadata,
ncch_container,romfs_reader}.*`, `src/core/hle/service/am/am.cpp` (installation),
`src/core/loader/ncch.*`, `src/core/memory.*` and kernel process/VM code.

- CIA is an installable container, not directly executable code. Validate all
  section sizes/alignment, signature-dependent ticket/TMD body offsets, content
  bitmap/indices and hashes; select the application NCCH, not the first content
  unconditionally. Distinguish updates/DLC, absent contents and encrypted layers.
- Implement the ARM11/memory/module prerequisites above. Split NCCH ExHeader,
  ExeFS `.code` (including BLZ), RomFS, segment permissions, BSS and TLS. Preserve
  metadata needed for process setup. Fail closed for unsupported crypto/formats;
  accept user-supplied keys only, never distribute keys/firmware.
- Always materialize extracted content into `work/3ds/<title>/...`, using the
  shared NX-style host filesystem utilities. Extract RomFS safely into a tree;
  no mount-on-demand of the original CIA. Reject traversal, duplicates, malformed
  offsets, oversized/decompression-bomb inputs and symlink escapes.
- Pipeline: **extract -> recompile -> run**. Reuse stage supervision and shared
  FS/tree logic, not NX's NSO/A64 loader. Publish staged extraction/compiler
  outputs atomically. Cache identity includes input hashes, keys/configuration
  identity without storing secrets, ISA, module ABI and compiler version; never
  reuse an incomplete build. Launch only compatible native modules.
- Gate: synthetic CIA/NCCH/RomFS fixtures, malformed/truncated/overflow cases,
  memory permissions/alias/MMIO tests, ARM/Thumb native witness, resume/cache
  invalidation and an asset-free end-to-end extract/AOT/run test.

### 3. Basic HLE OS and a SwIPC-derived generator

Reference Azahar kernel/SVC/IPC code and `src/core/hle/service/`. Use
`tools/swipc/swipc.c` as the parser/schema/codegen basis, with a **separate CTR
wire adapter**: 3DS TLS command words, normal/translation counts, handles,
static/mapped buffers and response layout are not Switch HIPC/CMIF.

- Threads, handles, events, waits, scheduling, SVCs, TLS, shared memory and result
  codes; checked guest buffers and handle lifetimes at every IPC boundary.
- Start with `srv:`, `fs:USER`, `APT:U`, configuration/time and resource services;
  implement shared-memory/event plumbing needed by later DSP/GSP/HID stages.
- Gate: generated dispatch against hand-encoded requests, malformed descriptors,
  unknown commands, handle transfer/close, IPC-buffer bounds, blocking/wakeup,
  process exit and a small homebrew OS/filesystem witness.

### 4. Audio DSP

Reference `src/audio_core/{dsp_interface.*,hle/*}` and
`src/core/hle/service/{dsp,csnd}/`. Port HLE DSP shared memory, voices, decoding,
resampling, mixing/effects, pipes/semaphores and interrupt events; adapt output to
ScarletNX's host audio boundary. Do not substitute silence for implemented DSP.
LLE/firmware behavior needs a separately licensed/dependency-audited integration.
Gate: asset-free PCM/ADPCM decoding/mixing golden tests, pipe/event lifecycle,
ring-buffer wrap, sample-rate timing and deterministic offline rendering.

### 5. Video graphics

Reference Azahar GSP/GPU/LCD, `src/video_core/pica/`, shader and software renderer
first, then Vulkan as an acceleration path. PICA200 is **not Maxwell**.
Implement GSP shared queues/interrupts, GPU command lists, physical-memory access,
DMA/fills/display transfers, texture formats, shader behavior, framebuffer layout
and top/bottom screen presentation. Integrate renderer cache invalidation with
CPU/DSP memory writes rather than copying the NX GPU frontend.
Gate: deterministic software-rendered golden scenes, GPU command/memory bounds,
transfer/coherency tests, two-screen layouts, then Vulkan comparisons.

### 6. HID and remaining peripherals

Reference `src/core/hle/service/{hid,ir,cam,mic,nfc,ptm,mcu,qtm}/` and the Azahar
frontend/input interfaces. Wire host input to 3DS shared-memory sample rings,
pad/circle-pad/touch, accelerometer/gyro, IR/New 3DS inputs and events with correct
timestamps and screen-coordinate transforms. Camera/microphone/NFC and lid/power
policies require explicit support or documented unsupported results; never claim
that every peripheral is wired when only button input works.
Gate: injected input/touch tests, ring wrap/events, disconnect/reconnect, New/Old
model differences, camera/mic bounds and end-to-end homebrew interaction.
