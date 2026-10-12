# Architecture

ScarletNX AOT-compiles Switch ARM64 -> C -> `<Game>_code.so` (Windows: `.dll`). Runtime: HOS kernel/services,
GPU exec + videodec. File map: `CLAUDE.md`.

## Recompiler + module ABI

`scarletnx/` consumes `work/<Game>/{exefs,romfs}` and produces `games/<Game>`:

- `nxloader.py`: loads main + emulates runtime loader.
- `codegen.py`, `a64.py`: function discovery + chunked ARM64 -> C. Guest functions take `CPU` state.
- `module.py`: module metadata + guest addr -> C function mappings for runtime dispatch.
- `toolchain.py`: shared-module build + object cache. `data.py`: romfs copy + default `<Game>.ini`.

`snx-recomp` (`scripts/recomp.c`): CMake runtime build -> Python recompiler -> shader prebuild.

`runtime/abi/cpu.h`: `CPU`, indirect-branch lookup (`snx_lookup`), `RtAPI` callbacks (SVC, undefined instruction,
BRK, system counter, FPCR, WFE). Modules receive `RtAPI` at load; no named runtime imports.
`cpu_helpers.c` + `helpers/`: non-inline SIMD, crypto + FP conversions.

`SNX_ABI` mismatch rejects the module. Changes under `runtime/abi/` require rebuilding every game.

## Runtime ownership

Paths below are relative to `runtime/`.

| Directory | Responsibility |
| --- | --- |
| `core/` | Module loading, `<Game>.ini`, guest clock, `RtAPI`, crash reporting |
| `kernel/` | HOS objects, handles, threads, sync, SVC, guest memory + IPC |
| `hle/` | IPC service families and nvdrv device files |
| `gpu/` | GPFIFO, Maxwell engines, shader translation, renderer interface, Vk + null |
| `video/` | host1x: NVDEC/FFmpeg videodec + VIC compositing |
| `platform/` | POSIX/Windows host APIs, SDL3/headless backends, scripted input, timed exit and screenshots |

### Guest exec

Guest thread = host thread + CPU state + TLS. `core_enter` (`kernel/thread.c`) serializes exec on four guest cores,
preserving games' unlocked per-core state. Core release: kernel blocking, sleep/yield, IPC, WFE. Waiters are
priority-ordered; owners yield at the next SVC when a higher-priority thread waits.

`g_mem` (`kernel/mem.c`): single guest address-space reservation. `GP(addr)` maps guest offsets into it;
generated code accesses guest memory directly.

SVC: `RtAPI.svc` -> `kernel/svc.c`. IPC: `svcSendSyncRequest` -> `kernel/ipc.c` -> session's service object in `hle/`.

### GPU submit/present

1. Guest NVN builds Maxwell command buffers in guest memory. GPFIFO submit: `/dev/nvhost-gpu` ioctl ->
   `hle/nvdrv.c` -> `gpu_submit` (`gpu/gpu.c`).
2. `gpu_submit` copies entries to the GPU queue, preserving cross-channel submit order. Fence waits stay on the
   GPU thread only for GPU-channel fences satisfiable by queued submissions; otherwise they block the submitter
   (including host1x/CPU fences).
3. The GPU thread dispatches methods to 3D (`engines.c`, `maxwell3d.c`), macros (`mme.c`), compute (`compute.c`),
   2D (`fermi2d.c`), DMA (`dma.c`) and inline-to-memory engines. Draws reach `g_renderer` through `gpu/renderer.h`.
   Shader translation is Maxwell SASS -> GLSL -> SPIR-V, using `gpu/shader/` and glslang.
4. `stream_incr` publishes GPU results to guest memory before advancing syncpoints.
5. vi (`hle/vi.c`) waits for frame fences not promised by queued submissions, then queues present. After preceding
   submissions, the GPU thread checks remaining fences and calls `present_buffer`: vi crop from the matching RT,
   or guest-memory conversion if no RT holds the buffer.

### Renderer coherence + lifetime

- **Vk** (`gpu/vk/`): requires 1.3; guest buffer mirrors + separate compute queue.
- **null** (`gpu/null/`): headless; command exec, syncpoints + semaphores remain active.

Texture/buffer invalidation: host write tracking + `mem_unmap` -> `gpu_mem_changed` (`gpu/gpu.h`). The latter covers
zeroed unmap/heap-return pages without host writes; Vk marks those ranges written at the next cache check.

`Renderer.save`: optional, Vk pipeline cache. Called by the frontend every 5s (off GPU thread) and on normal exit.
Exit leaves guest threads running; no renderer teardown.

### Videodec/VIC channels

Per nvdrv fd (`/dev/nvhost-nvdec`, `/dev/nvhost-vic`): `host1x_open` channel, registers + syncpoints; NVDEC also
owns a decoder per channel. NVDEC writes job-specified guest surfaces; VIC composites them into the game's output.
