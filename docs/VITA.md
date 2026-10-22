<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# PS Vita driver

`scarletnx-vita` runs a homebrew VPK the way the other drivers run their games. The VPK is extracted once into the
data folder's `recompiled/<title ID>/` (`app/` holds the package, `ux0/` the writable storage), the eboot's code is
compiled ahead of time into `<title ID>_code.so`, and a copy of the runtime named after the title ID plays it. Code
the offline pass didn't find is compiled at run time by the LLVM backend's executor, and the addresses it compiled are
kept as hints (`<title ID>.hints`) for the next offline compile.

```sh
build/scarletnx-vita game.vpk                    # extract, compile and play
build/scarletnx-vita -x game.vpk                 # extract only
build/scarletnx-vita --game=~/.local/share/scarletnx/recompiled/LVBT00001 --seconds=30 --shot=/tmp/frame
```

The first homebrew it targets is the Lua Vita Tester (lpp-vita with vita2d). It draws its screen, reads the buttons
and plays its sound test.

## How it fits together

| Part | Files |
| --- | --- |
| VPK, SELF and ELF loading (zip and zlib, relocations, imports patched to SVCs by index) | `src/vita/loader/` |
| The offline compiler and the module it makes (the A57-profile AArch32 backend, `src/common/cpu/arm/`) | `src/vita/aot/` |
| Process, threads (cooperative, one host thread), waits, memory, the dispatcher, run-time compiles | `src/vita/runtime/` |
| The system libraries, answered by NID | `src/vita/hle/` (`kernel.c`, `io.c`, `ctrl.c`, `display.c`, `audio.c`, `misc.c`, `gxm.c`) |
| sceGxm on Vulkan: guest memory imported with `VK_EXT_external_memory_host`, USSE programs translated to SPIR-V through `src/common/utilities/spirv_emit.c` | `src/vita/gpu/` (`render.c`, `usse.c`, `vk.c`, `present.c`) |
| Keyboard, mouse (touch), gamepads and scripted presses (`--script=ms:hex,...`, SceCtrl's button bits) | `src/vita/platform/input.c` |

The driver's settings (`--key=value`) are the `g_vita_debug_ini` table of `src/nx/runtime/settings.txt`: `vita_hle_trace` prints each import call,
`vita_jit_trace` each block compiled at run time, `vita_gxm_trace` each scene and draw.

## Checking the CPU backend

Configure with `-DSNX_VITA_LOCKSTEP=ON` (and `SNX_UNICORN_INCLUDE`, `SNX_UNICORN_LIBRARY` pointing at Unicorn 2) and
run with `VITA_LOCKSTEP=N`: from the Nth import call on, every instruction group runs in Unicorn too, and the first
difference in registers, flags or written memory stops the run with both states. This is how the Thumb backend's
bugs this driver ran into were found.

## Limits

- Only the formats vita2d uses are drawn: A8B8G8R8 color surfaces, linear U8U8U8U8 and U8 textures. Swizzled, tiled
  and compressed textures, MSAA and stencil are not yet.
- The USSE translator covers the instructions vita2d's shaders use (`VMOV`, `VMAD`, `VNMAD`, `VPCK`, `SMLSI`, texture
  reads). Any other instruction makes the program fail to translate with its name in the log.
- One guest thread runs at a time.
- PGF fonts, networking and the common dialogs answer as unavailable.

## Sources

The GXM semantics, the GXP layout and the USSE translator are ported from Vita3K (GPL-2.0-or-later). The NIDs and
error codes come from vita-headers (MIT). vios was read as a quick reference. See [CREDITS](CREDITS.md).
