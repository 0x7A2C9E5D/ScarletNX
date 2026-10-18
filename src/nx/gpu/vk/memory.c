/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* How big one device memory allocation may be, and images bigger than that.

   Some drivers fail allocations past 256 MiB even where maxMemoryAllocationSize is larger (NVIDIA's on FreeBSD), and
   a game's textures can be bigger: a 2D ASTC texture of 11584x11584 decodes to 512 MiB of RGBA8, a 3D one of 512^3
   too. No allocation is made bigger than g_vk.alloc_cap: the staging and readback rings stop growing there (cmd.c),
   and an image whose memory is bigger is created with sparse binding and bound to several allocations, each at most
   the cap. Shaders and views see one ordinary image: only its memory is in pieces.

   The pieces are dedicated allocations, so the cap is also what the driver is asked for. They are bound on the
   graphics queue, and the bind is waited for on the host before the image is used: a bind is not ordered with
   command buffers submitted after it, on that queue or on the compute queue, without a semaphore each of them would
   have to wait on, and an image this big is made once. */
#include <stdlib.h>
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"

#define ALLOC_CAP_DEFAULT (256ull << 20)

/* the images in pieces, each with its allocations */
typedef struct {
    VkImage img;
    u32 n;
    VmaAllocation pages[];
} Pieces;
static Pieces** g_pieces;
static u32 g_npieces, g_cappieces;

void vk_mem_init(int sparse_queue) {
    VkPhysicalDeviceMaintenance3Properties m3 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &m3};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    u64 cap = ALLOC_CAP_DEFAULT;
    /* a smaller cap for testing the pieces on a driver that takes anything (lavapipe) */
    const char* env = getenv("SNX_VK_MAX_ALLOCATION_MB");
    if (env && strtoull(env, NULL, 10)) cap = strtoull(env, NULL, 10) << 20;
    if (m3.maxMemoryAllocationSize && cap > m3.maxMemoryAllocationSize) cap = m3.maxMemoryAllocationSize;
    g_vk.alloc_cap = cap;
    g_vk.sparse = sparse_queue && g_vk.feat.sparseBinding;
    LOG("vulkan: allocations of at most %llu MiB, bigger images %s\n", (unsigned long long)(cap >> 20),
        g_vk.sparse ? "in pieces (sparse binding)" : "in one allocation (no sparse binding)");
}

static int pieces_bind(VkImage img, const VkMemoryRequirements* req, Pieces* p) {
    u64 chunk = g_vk.alloc_cap / req->alignment * req->alignment;
    if (!chunk) return 0;
    u32 n = (u32)((req->size + chunk - 1) / chunk);
    VkSparseMemoryBind* binds = calloc(n, sizeof *binds);
    if (!binds) return 0;
    int ok = 1;
    for (u32 i = 0; i < n && ok; i++) {
        u64 at = (u64)i * chunk, size = req->size - at < chunk ? req->size - at : chunk;
        VkMemoryRequirements piece = {.size = size, .alignment = req->alignment, .memoryTypeBits = req->memoryTypeBits};
        VmaAllocationCreateInfo aci = {.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT,
                                       .preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
        VmaAllocationInfo info;
        if (!VK_OK(vmaAllocateMemory(g_vk.vma, &piece, &aci, &p->pages[i], &info))) {
            ok = 0;
            break;
        }
        p->n++;
        binds[i] = (VkSparseMemoryBind){
            .resourceOffset = at, .size = size, .memory = info.deviceMemory, .memoryOffset = info.offset};
    }
    if (ok) {
        VkSparseImageOpaqueMemoryBindInfo opaque = {.image = img, .bindCount = n, .pBinds = binds};
        VkBindSparseInfo bi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .imageOpaqueBindCount = 1,
                               .pImageOpaqueBinds = &opaque};
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence;
        ok = VK_OK(vkCreateFence(g_vk.dev, &fi, NULL, &fence));
        if (ok) {
            ok = VK_OK(vkQueueBindSparse(g_vk.gfx, 1, &bi, fence)) &&
                 VK_OK(vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, UINT64_MAX));
            vkDestroyFence(g_vk.dev, fence, NULL);
        }
    }
    free(binds);
    return ok;
}

static void pieces_free(Pieces* p) {
    for (u32 i = 0; i < p->n; i++) vmaFreeMemory(g_vk.vma, p->pages[i]);
    free(p);
}

/* an image whose memory is more than one allocation may be: bound to pieces of at most g_vk.alloc_cap */
static int image_in_pieces(const VkImageCreateInfo* ii, VkImage* img) {
    VkImageCreateInfo si = *ii;
    si.flags |= VK_IMAGE_CREATE_SPARSE_BINDING_BIT;
    if (!VK_OK(vkCreateImage(g_vk.dev, &si, NULL, img))) return 0;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_vk.dev, *img, &req);
    u64 chunk = req.alignment ? g_vk.alloc_cap / req.alignment * req.alignment : 0;
    u32 n = chunk ? (u32)((req.size + chunk - 1) / chunk) : 0;
    Pieces* p = n ? calloc(1, sizeof *p + n * sizeof p->pages[0]) : NULL;
    if (g_npieces == g_cappieces && p) {
        u32 cap = g_cappieces ? g_cappieces * 2 : 8;
        Pieces** grown = realloc(g_pieces, cap * sizeof *grown);
        if (grown) {
            g_pieces = grown;
            g_cappieces = cap;
        }
    }
    if (!p || g_npieces == g_cappieces || !pieces_bind(*img, &req, p)) {
        WARN("vulkan: cannot bind a %llu MiB image in %u pieces of %llu MiB\n", (unsigned long long)(req.size >> 20), n,
             (unsigned long long)(chunk >> 20));
        vkDestroyImage(g_vk.dev, *img, NULL);
        *img = VK_NULL_HANDLE;
        if (p) pieces_free(p);
        return 0;
    }
    p->img = *img;
    g_pieces[g_npieces++] = p;
    LOG_LIMIT(8, "vulkan: a %ux%ux%u image of %llu MiB, bound in %u pieces\n", ii->extent.width, ii->extent.height,
              ii->extent.depth * ii->arrayLayers, (unsigned long long)(req.size >> 20), n);
    return 1;
}

int vk_image_create(const VkImageCreateInfo* ii, VkImage* img, VmaAllocation* mem) {
    *img = VK_NULL_HANDLE;
    *mem = VK_NULL_HANDLE;
    VkDeviceImageMemoryRequirements q = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
                                         .pCreateInfo = ii};
    VkMemoryRequirements2 req = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetDeviceImageMemoryRequirements(g_vk.dev, &q, &req);
    if (req.memoryRequirements.size > g_vk.alloc_cap) {
        if (g_vk.sparse) return image_in_pieces(ii, img);
        /* without sparse binding the one allocation is the only way: the driver may still take it */
        LOG_LIMIT(4, "vulkan: a %llu MiB image in one allocation, past the %llu MiB cap\n",
                  (unsigned long long)(req.memoryRequirements.size >> 20), (unsigned long long)(g_vk.alloc_cap >> 20));
    }
    VmaAllocationCreateInfo aci = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    return VK_OK(vmaCreateImage(g_vk.vma, ii, &aci, img, mem, NULL));
}

void vk_image_destroy(VkImage img, VmaAllocation mem) {
    if (mem || !img) {
        vmaDestroyImage(g_vk.vma, img, mem);
        return;
    }
    for (u32 i = 0; i < g_npieces; i++)
        if (g_pieces[i]->img == img) {
            vkDestroyImage(g_vk.dev, img, NULL);
            pieces_free(g_pieces[i]);
            g_pieces[i] = g_pieces[--g_npieces];
            return;
        }
    vkDestroyImage(g_vk.dev, img, NULL); /* made without memory */
}
