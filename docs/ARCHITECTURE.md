# Architecture

ScarletNX AOT: AArch64 -> C -> native module; HOS HLE, Maxwell execution, host1x videodec.
Source map: [CLAUDE.md](../CLAUDE.md).

## Recompiler + module ABI

`scarletnx/`: `work/<Game>/{exefs,romfs}` -> `games/<Game>[.exe]`, `<Game>_code.{so,dll}`, `<Game>_data/`:

- `driver.c`: CLI dispatch only; `extract.c` and `recompile.c` own their workflows.
- `loader.c`: NSO/NRO, LZ4, SHA-256, NPDM, MOD0, symbols/relocs/FDEs.
- `a64.c` + `a64_templates.inc`: shared instruction semantics for AOT and interpreter.
- `codegen.c`: function/CFG discovery; `module.c`: chunks, dispatch tables, `SnxModule`.
- `toolchain.c`: parallel Clang jobs, ABI/source-keyed object cache, linking, shader prebuild.
- `common.c`: host files/processes; argv-based subprocesses, isolated extraction key environment.

Host tooling uses C90 syntax with SDK fixed-width types, formatting and OS APIs.
The runtime and generated guest code retain their existing Clang/ABI requirements.
Normal builds link the driver into the runtime; `SNX_TOOLS_ONLY` builds the same CLI independently.

ABI (`runtime/abi/`): guest functions take `CPU*`; `snx_lookup` dispatches indirect branches, with interpreter fallback.
`CPU.rt` supplies `RtAPI` callbacks: SVC, UNDEF, BRK, hints, CNTVCT, FPCR, SEV/WFE; no named runtime imports.
`cpu_helpers.c` + `helpers/`: SIMD, crypto, FP.

Rebuild game modules after `runtime/abi/` changes; bump `SNX_ABI` for ABI changes. Version mismatches reject loading.

## Runtime ownership

| `runtime/` directory | Responsibility |
| --- | --- |
| `core/` | Module loading, INI, clock, `RtAPI`, crashes |
| `kernel/` | Objects, threads, sync, SVC, memory, IPC |
| `hle/` | Services, nvdrv |
| `gpu/` | GPFIFO, Maxwell, shaders, Vk/null renderers |
| `video/` | host1x, NVDEC/FFmpeg, VIC |
| `platform/` | Host APIs, SDL3/headless, input, screenshots, timed exit |

### Guest exec

Guest thread = host thread + `CPU` + TLS. `kernel/thread.c`: one owner/core, four cores; release on kernel block,
sleep/yield, IPC, WFE. Waiters: priority then FIFO; higher-priority preemption at the next SVC.
SVC/WFE-free spins retain their core.

`kernel/mem.c`: one guest VA reservation; `GP(addr) = g_mem + addr`, accessed directly by generated code.

SVC -> `kernel/svc.c`; `svcSendSyncRequest` -> `kernel/ipc.c` -> session's HLE service object.

[Filesystem](FILESYSTEM.md): startup scan; indexed lookup, metadata, enumeration, mutations.

### GPU submit/present

1. NVN command buffers -> `/dev/nvhost-gpu` GPFIFO ioctl -> `hle/nvdrv.c` -> `gpu_submit` (`gpu/gpu.c`).
2. Copy entries to GPU queue; `nv.submit_lk` orders fence promises and cross-channel submits.
   Promised GPU-channel fences wait on the GPU thread; host1x/CPU/unpromised fences wait on the submitter.
3. GPU-thread dispatch: 3D, MME, compute, 2D, DMA, inline-to-memory. Draws -> `g_renderer` (`gpu/renderer.h`).
   Shaders: Maxwell SASS -> GLSL -> SPIR-V (`gpu/shader/`, glslang).
4. Vk `stream_incr` lands compute writes in guest memory before incrementing the syncpoint.
5. vi waits on unpromised fences, then queues present after preceding submits. GPU thread checks remaining fences;
   `present_buffer` uses matching RT + vi crop, or converts guest memory.

### Renderer coherence + lifetime

- **Vk**: Vulkan 1.3, guest buffer mirrors; separate compute queue when available, graphics queue otherwise.
- **null**: headless; executes command streams, syncpoints, semaphores.

Invalidation: host write tracking + `mem_unmap` -> `gpu_mem_changed`. Vk treats zeroed unmap/heap-return ranges
as written at the next cache check.

`Renderer.save`: optional pipeline-cache persistence every 300 frontend ticks (~5s) + exit, concurrent with execution.
Exit skips renderer teardown and guest-thread joins.

### Videodec/VIC channels

Each NVDEC/VIC fd owns a `host1x_open` channel, registers and syncpoints; NVDEC also owns a decoder.
NVDEC writes guest surfaces; VIC composites output.
