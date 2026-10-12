# Guest buffers on unified memory

ScarletNX does not currently import guest RAM into Vulkan. `vk_stage` maps a Vulkan buffer that the GPU reads directly,
but `vk_buf_copy` first copies bytes out of guest RAM. Exporting the final image as a dma-buf, as desktop compositors do,
would not remove this copy: ScarletNX already presents its Vulkan image without an app-managed dma-buf handoff.

## Measured workload

With the constant-buffer cache enabled, a 600-present Tears of the Kingdom cave interval staged 3,688,879,936
uncached bytes in 63,098 calls. The largest call was 58,848 bytes and none were zero-filled. The only nonzero-address
`vk_buf_copy` call site with that cache enabled is the writable graphics storage-buffer branch in `stage_set`. A
per-draw wait or allocation is therefore not a viable replacement for the staging ring. The test machine has a
discrete RTX 2080; it cannot establish a unified-memory speedup.

## Correctness requirements

The input to each draw is a snapshot. Guest CPU threads can write the same pages while recorded GPU work is in flight,
and GPU virtual ranges can map fragmented host pages. The current asynchronous userfaultfd write tracker reports writes
after they happen; it does not freeze pages. Guest mapping and decommit operations can also change backing memory.
Importing `g_mem` directly would let the GPU see later CPU writes, and a shader could write guest memory before the
guest's synchronization point. A direct import must therefore own and pin the imported pages, intercept writes to
them or use copy-on-write, and wait for GPU completion before remapping or releasing them.

Graphics storage writes need a coherent path even without import. A graphics output must retain its input bytes,
record its destination range and GPU buffer, and make shader writes available to host reads. On an overlapping guest
read, guest write, mapping change, or stream synchronization point, it must wait for the output, invalidate noncoherent
memory, and land only bytes the shader changed where the guest still holds the input value. Overlapping GPU draws must
reuse the GPU result with a storage barrier or land it before taking a new guest snapshot. Frame-ring reuse must not
destroy pending output buffers. The compute path already uses this changed-byte rule; `vk_write_changed` is shared with
the graphics implementation.

## Import eligibility

`VK_EXT_external_memory_host` imports a host pointer, not a dma-buf. An import candidate needs a contiguous,
committed, correctly aligned host range and a buffer-compatible, host-visible memory type reported by
`vkGetMemoryHostPointerPropertiesEXT`. The pointer and its backing storage must stay valid for the entire Vulkan
allocation lifetime. Import is enabled only on a unified-memory device after the page-ownership and synchronization
rules above are implemented. The existing snapshot staging path remains the fallback for fragmented, mutable, or
unsupported ranges and for discrete GPUs. `VK_EXT_map_memory_placed` may help place Vulkan-owned allocations inside
the guest address reservation, but changing `mem.c` to use such allocations is a separate memory-system change.

## Validation gate

Before enabling import by default, run the same TotK cave and Super Mario Odyssey scenes on an actual unified-memory
device with Vulkan validation. Compare frame hashes or aligned screenshots, graphics and compute storage output,
CPU writes during in-flight draws, unmap/decommit behavior, buffer-copy counts, frame time, and memory use against the
staging fallback. A device reporting the extension alone is not evidence of a faster or safe path.

Vulkan references: [host pointer import](https://docs.vulkan.org/spec/latest/chapters/memory.html#host-external-memory),
[host/device synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html), and
[placed memory mapping](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_map_memory_placed.html).
