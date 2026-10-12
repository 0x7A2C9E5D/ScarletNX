<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Repository audit

Reviewed `aa51d23` on 2026-10-12 across the recompiler, ABI, runtime, services, GPU/video,
platform, build scripts and documentation. Findings below remain unfixed in production code.
**R** = reproduced with synthetic CPU inputs. **S** = confirmed by source inspection.

## Fix first

| Location | Finding and suggested fix | Evidence |
| --- | --- | --- |
| [ipc.c:262](../runtime/kernel/ipc.c#L262) | IPC counts/lengths can read outside the message. Pass its actual capacity, bound every field and derive `raw_size`. | R: crafted 256-byte request crashed. |
| [mem.c:293](../runtime/kernel/mem.c#L293) | Heap shrink unlocks before unmapping, so a concurrent successful grow is decommitted. Keep resize and unmap under one lock. | R: reported 4 MiB heap contained free pages. |
| [fs.c:466](../runtime/hle/fs.c#L466) | Symlinks/junctions escape the filesystem root. Recursive deletion also follows a requested top-level link. Use rooted operations that reject links throughout the path. | R: temporary junction read escaped root. Deletion not executed. |
| [svc_mem.c:50](../runtime/kernel/svc_mem.c#L50) | `MapMemory` copies an unchecked source after mapping the destination. Validate source state/permissions before mutation. | R: unmapped source crashed. |
| [svc_mem.c:133](../runtime/kernel/svc_mem.c#L133) | Shared-memory writers retain pointers while unmap decommits them. Pin mappings or lock across service writes and unmap. | R: retained-pointer write crashed. |
| [nvdrv.c:1008](../runtime/hle/nvdrv.c#L1008), [1055](../runtime/hle/nvdrv.c#L1055) | Ioctl/QueryEvent use channels/events after dropping lifetime protection. Concurrent Close can free them. Acquire references under the fd lock. | S |
| [fermi2d.c:82](../runtime/gpu/fermi2d.c#L82) | `width * bpp` wraps before allocation. Validate dimensions with checked arithmetic. | R: width `0x40000001`, 4-byte pixels caused an ASan heap overread. |
| [astc.c:644](../runtime/gpu/texture/astc.c#L644) | An 11-bit value is checked for bit 11, rejecting valid [solid-color ASTC blocks](https://github.com/KhronosGroup/DataFormat/blob/main/astc.txt). Read reserved bits directly. | R: red decoded as magenta. Scratch correction returned red. |
| [integer.c:46](../runtime/abi/helpers/integer.c#L46) | Clang 22.1.3 can inline the `cx16` helper into baseline code, emitting an unresolved `__atomic_compare_exchange_16`. Preserve its target with `noinline`. | R: interpreter DLL link failed, then passed with scratch fix. Clang 23 not tested. |

## Correctness follow-ups

| Location | Finding and suggested fix | Evidence |
| --- | --- | --- |
| [fs.c:435](../runtime/hle/fs.c#L435) | Path reads ignore descriptor length. Bound reads by that length and reject missing terminators. | R: one-byte path at page boundary crashed. |
| [data.py:32](../scarletnx/data.py#L32) | Recompiling an update refreshes ExeFS but keeps old RomFS. Synchronize changed/deleted assets while preserving saves. | R: new code paired with old assets. |
| [nxloader.py:302](../scarletnx/nxloader.py#L302) | Uncompressed NSOs use compressed-size fields. Select size from the compression flag, matching the runtime and [hactool](https://raw.githubusercontent.com/SciresM/hactool/master/nso.h). | R: valid uncompressed segment rejected. |
| [integer.c:62](../runtime/abi/helpers/integer.c#L62) | FP arithmetic does not accumulate guest FPSR exception flags. Track them per guest thread without host-runtime contamination. | R: divide-by-zero left DZC clear. |
| [surface.c:345](../runtime/gpu/vk/surface.c#L345) | I2M/compute writes to render-target memory only produce a warning. Reload/invalidate the stale image before reads. | S |
| [surface.c:386](../runtime/gpu/vk/surface.c#L386) | Eviction discards dirty render targets without write-back. Preserve GPU-only contents before freeing, including parked aliases. | S |
| [texture.c:190](../runtime/gpu/vk/texture.c#L190) | Fragmented textures hash only 64 bytes per page, missing other changes even after dirtiness is detected. Hash all bytes or upload dirty ranges. | R: modifying byte 100 preserved the hash. |
| [compute.c:443](../runtime/gpu/vk/compute.c#L443) | Chained subranges can violate [storage-buffer alignment](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html). Check device alignment and copy incompatible ranges. | S: offset +16 can violate 256-byte alignment. |
| [cmd.c:169](../runtime/gpu/vk/cmd.c#L169) | Staging growth retires allocations that submission never flushes. [Flush each noncoherent allocation](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/memory_mapping.html) before submission. | S |
| [translate.c:2326](../runtime/gpu/shader/translate.c#L2326) | `MEMBAR` is discarded and `VOTE`/`SHFL` approximate one lane. Implement proper barriers/warp behavior or report unsupported translation. | S |
| [nvdec.c:437](../runtime/video/nvdec.c#L437) | Receive/output-write failures still produce successful decode status. Propagate failures into the job result. | S |
| [nvdec.c:381](../runtime/video/nvdec.c#L381) | Decoder packets lack [FFmpeg's required 64-byte zero padding](https://www.ffmpeg.org/doxygen/9.0/group__lavc__decoding.html). Allocate and clear the full padding. | S: API-contract violation, no decoder overread reproduced. |

GPU state also needs isolation: [constant-buffer bindings](../runtime/gpu/vk/renderer.c#L73)
must be stored per channel, and [overlapping compute descriptors](../runtime/gpu/vk/compute.c#L495)
must share backing memory so writes through one alias are visible through another.

Additional source-verified races: make [StartThread](../runtime/kernel/svc.c#L172) an atomic state
transition, use exclusive [file creation/rename](../runtime/hle/fs.c#L650), and serialize shared
[audio-renderer updates](../runtime/hle/audren.c#L123).
The Python [LZ4 fallback](../scarletnx/nxloader.py#L54) also accepts invalid backreferences through
negative indexing. Validate offsets and input/output bounds.

## Verification and improvements

- Native CPU probes reproduced the marked failures. Six kernel probes used isolated functions
  or controlled scheduling, not live concurrency stress. ASTC and the 2D blitter used production code.
- Against Unicorn: 83,200 scalar, 10,140 integer SIMD and 1,800 crypto cases matched. Another
  15,928 SIMD FP cases matched apart from documented NaN-sign differences. Scalar probes used
  a scratch `cx16` workaround. Exception-state coverage found the FPSR issue above.
- Python syntax/CLI checks and CMake preset parsing passed. No full runtime build, game run,
  GPU validation or cross-platform stress test was performed.
- Add these synthetic cases to CI. Current checks mainly establish buildability, not guest behavior.

Documentation was reorganized: README now holds identity, setup and basic usage. Build/AppImage
notes moved to [BUILD](BUILD.md), controls/settings to [USAGE](USAGE.md), tooling/imports to
[DEVELOPMENT](DEVELOPMENT.md), and attribution to [CREDITS](CREDITS.md). Architecture and roadmap
were condensed. Keep one explanation per topic and use Git history for completed-work detail.
