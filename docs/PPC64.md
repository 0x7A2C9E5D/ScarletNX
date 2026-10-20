<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# PowerPC64

## Index

* [Building on an x86-64 Debian machine](#building-on-an-x86-64-debian-machine)
* [Running a game under qemu](#running-a-game-under-qemu)
* [Host pages larger than 4 KiB](#host-pages-larger-than-4-kib)
* [What differs from the other hosts](#what-differs-from-the-other-hosts)
* [Big-endian hosts](#big-endian-hosts)

ScarletNX builds for 64-bit PowerPC Linux from POWER8 on. The little-endian port (ppc64le) works: the test suite
passes under qemu-user, and Super Haxagon, recompiled under qemu, played a level to its game over screen there with
lavapipe as the Vulkan driver. No real POWER machine has run it yet. CI cross-builds its AppImage
(`ScarletNX-linux-ppc64le.AppImage`, the `appimage-cross` job in `.github/workflows/build.yml`) against an Arch POWER
system root and runs it under qemu as installed; the test suite's job, `linux-ppc64le`, is commented out.
The big-endian port has a job of its own, `linux-ppc64`, which builds the Linux runtime and runs its tests under
qemu-ppc64.

On a big-endian host the Linux runtime runs AArch64 programs, with Debian ports' glibc (`linux-ppc64-cross`, ELFv1)
and with Arch POWER's (`linux-ppc64-elfv2-cross`, ELFv2): under qemu-ppc64, an AArch64-hosted GCC compiles a C file
and the program it made runs, both dynamically linked against glibc. CI packages the Arch POWER build as
`ScarletNX-linux-ppc64.AppImage`, the Linux runtime alone (`ScarletNX-linux-ppc64.AppImage ./program`). The Switch
runtime builds there but refuses to start a game. See [Big-endian hosts](#big-endian-hosts).

## Building on an x86-64 Debian machine

The tools, once:

```bash
sudo apt install clang lld llvm ninja-build cmake ccache mmdebstrap qemu-user qemu-user-binfmt binfmt-support \
    glslang-tools spirv-tools debian-ports-archive-keyring
```

A sysroot with the target's libraries and LLVM, made without root by `scripts/ppc64-sysroot.sh` (about 1 GB). It is
staged in `/var/tmp`, as `/tmp` is often too small. `ppc64` makes the big-endian one from Debian's ports archive:

```bash
scripts/ppc64-sysroot.sh ppc64el .cache/sysroot-ppc64el
export SNX_SYSROOT=$PWD/.cache/sysroot-ppc64el
```

Then the build and the tests, which run under qemu-ppc64le:

```bash
cmake --preset linux-ppc64le-cross -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build --preset linux-ppc64le-cross
ctest --preset linux-ppc64le-cross -j"$(nproc)"
```

The target's `llvm-config` is a PowerPC program, so `scripts/cross-llvm-config.sh` runs it under qemu. The presets
point pkg-config at the sysroot and set `QEMU_LD_PREFIX` for the tests. Three tests are skipped under the emulator:
`3ds_static_pipeline` and `3ds_extract` link with the compiler path built into the driver, which is the build
machine's, and `gui_process_tree` reads a resident size that qemu reports as zero.

## Running a game under qemu

The recompiler runs on the machine the module is for, so under qemu it takes a while (Super Haxagon: about 20
minutes, against 4 natively on x86-64):

```bash
export QEMU_LD_PREFIX=$SNX_SYSROOT VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json
qemu-ppc64le build/linux-ppc64le-cross/scarletnx -r app.nro -o App --outdir games/ppc64le
cd games/ppc64le/App && qemu-ppc64le ./App -H --seconds=120 --shot=/path/to/shot
```

## Host pages larger than 4 KiB

Most ppc64le distributions, Debian's among them, run kernels with 64 KiB pages, and Apple silicon and some AArch64
kernels use 16 KiB ones. The Switch maps memory in 4 KiB pages, which a host can only commit and protect in its own,
so `src/nx/memory/mem.c` keeps the guest's pages in its block list and commits whole host pages under them. Freeing a
guest page decommits its host page once nothing else in it is mapped, and zeroes the page otherwise, so it reads zero
when mapped again, as on the console. The differences from a 4 KiB host:

- A guest access to a free page that shares a host page with a mapped one doesn't fault.
- Views of memory (MapMemory, shared memory, ldr's code) are copies, as on Windows, instead of the same pages. A view
  can only share host pages with its source when both sit at the same place in a host page, and games pick their
  addresses in 4 KiB steps. The services reach shared memory through the guest's mapping while it lasts.
- The Linux runtime (`src/gnuabi/`) protects each host page as the most permissive of its guest pages, so a write to
  compiled code beside writable data in one host page isn't seen.

`SNX_HOST_PAGE=16384` or `65536` in the environment makes any machine work in those pages, which is how the tests
and CI check these paths on 4 KiB hosts (`ldr_service_64k_pages`, `gnuabi_memory_64k_pages`,
`nx_runtime_homebrew_64k_pages`); `memory_accounting` runs as on 16 and 64 KiB hosts by itself.

## What differs from the other hosts

- **The baseline is POWER8** (`cmake/cross.cmake`). Before it there is no `lqarx`/`stqcx.`, and the 128-bit
  compare-and-swap that STXP becomes would be a libatomic call, which a module doesn't link.
- **Guest tail calls go through stubs.** Without POWER10's PC-relative code, LLVM refuses a guaranteed tail call to a
  function it can't see shares the caller's TOC: another unit's function, or a pointer. A module has one TOC, so the
  jump is safe, and `ppc_tail_calls` in `src/common/cpu/recompiler/llvm/ir.c` sends those calls to a naked stub of the
  same module that branches on.
- **The linker** (`src/common/cpu/recompiler/llvm/link_elf.c`, with the relocations in `elf_object.c`) handles the
  TOC relocations, ELFv2's local entry points, ELFv1's `.opd` descriptors, and 16-byte stubs for a call past `bl`'s
  32 MiB.
- **One TOC for the Linux runtime's code.** Its just-in-time steps are loaded into one code arena
  (`code_loader.c`, `library.c`'s `arena_load`) with the runtime library once, and the arena has one TOC, which every
  step's code keeps in r2. Elsewhere each step is a module of its own, linked and loaded with `dlopen`. On PowerPC64
  that gave each step its own TOC, and a guest's jump from one step into another came back to its caller with the
  other step's TOC. Loaded code calls the runtime only through pointers (RtAPI), which LLVM calls with r2 saved. A
  forked child copies the arena's pages, which are shared memory, into its own (`snx_recompiler_forked`).
- **No runtime JIT for Switch games.** Its units call the AOT module's functions directly, and the module has a TOC
  of its own (`module_jit` in `library.c`). The A32 executor is off too: its blocks call the runtime's functions
  directly (`common/cpu/arm/exec.c`), so the Linux runtime runs no 32-bit programs on PowerPC64.
- **Rounding stays on the slow path.** LLVM 19 lowers `llvm.roundeven` to a libm call on POWER8, and fails on its
  vector form, so FRINTN and the rounding conversions use the software path. Fused multiply-add is a fast path, as
  POWER always has it.
- **SDL3 is built without AltiVec** (`cmake/dependencies.cmake`): its blitters prefetch with `dstt`, which the ISA
  phased out and qemu traps.

## Big-endian hosts

AArch64 is little-endian, so on a big-endian host guest memory keeps the guest's byte order and everything that reads
or writes a guest value as a number converts it:

- The recompiler swaps where generated code reads memory as numbers (`ir_rd`, `ir_wr`, `ir_ldacq`, `ir_strel` in
  `ir_state.c`) and where it compares in memory (`cas` in `ir_helpers.c`, `snx_cas128`). LLVM folds the swaps into
  POWER's byte-reversed loads and stores. The vector registers hold the guest's bytes too: the views that read a lane
  as a number swap it (`ir_vd`, `ir_ve`, the lane helpers of `ir_simd.c`), and Q loads, LD1 to LD4, TBL and EXT stay
  byte copies. The emitters decide by the target's data layout, so a little-endian host's code is what it was.
- The Linux runtime's system calls read and write the guest's structures through `common/utilities/endian.h`
  (`snx_ld_le32`, `snx_st_le64` and their kin), which compile to plain copies on a little-endian host.
- `scripts/le-unchanged.sh` builds a base commit and the working tree on a little-endian machine and lists every
  object whose code differs, and whether the recompiler's output for an AArch64 program changed.

The build links with GNU ld, since LLD links no ELFv1 object: `binutils-powerpc64-linux-gnu` on Debian. The system
root is Debian ports' unstable:

```bash
scripts/ppc64-sysroot.sh ppc64 .cache/sysroot-ppc64
export SNX_SYSROOT=$PWD/.cache/sysroot-ppc64
cmake --preset linux-ppc64-cross -DSNX_BUILD_NX=OFF -DSNX_BUILD_3DS=OFF -DSNX_BUILD_GUI=OFF
cmake --build --preset linux-ppc64-cross --target scarletnx-gnuabi-run
```

CI's `linux-ppc64` job builds it this way against that system root, made weekly, and runs the Linux runtime's tests
(`gnuabi_*`, `endian`, `cas128`) under qemu-ppc64. Debian ports' unstable can break for a day, so the job may fail
without failing the run.

Arch POWER's big-endian glibc is ELFv2, as LLD links it. Its preset (`linux-ppc64-elfv2-cross`, `SNX_PPC64_ELFV2`)
builds for ELFv2 with `-mabi=elfv2` and the Linux runtime alone. Its system root is pkgforge's Arch image of the
architecture with LLVM and GCC's runtime installed into it, as the `appimage-cross` job makes it. The runtime's own
code generator takes its ABI from the runtime itself (`_CALL_ELF`, `new_ir` in `ir.c`), as LLVMs differ in which
ABI their `powerpc64-*-linux-gnu` picks: Debian's 19 ELFv1, Arch POWER's 22 ELFv2.

What was run under qemu-ppc64: the Linux runtime's tests (`gnuabi_*`, `endian`, `cas128`), `tools/gnuabi_diff.py
--suite all` against qemu-aarch64 (the CPU suite covers SIMD, floating point, exclusives and unaligned accesses), a
glibc program, and the AArch64-hosted GCC 16.2 compiling a C file with a glibc system root.

The Switch runtime is not ported: its services, IPC, the GPU and the shader translator read guest memory as host
numbers. What it needs: `mem.h`'s accessors and the
synchronization primitives' atomics, the IPC generator's arguments and structures, the services' local `rd32`/`wr32`
helpers, hactool's headers, and the GPU's data, which stays in the guest's byte order in buffers and is converted
where it is read as typed values (the translated shaders, image uploads and readbacks, vertex input). Until then the
runtime refuses to start a game on a big-endian host.
