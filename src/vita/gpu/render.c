/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sceGxm's scenes on the host's Vulkan device (vk.h). The memory the game maps for the GPU (sceGxmMapMemory) is
   imported as buffers: a scene's color surface is an image loaded from its memory when the scene begins and stored
   back when it ends, vertices and indices are read where the game wrote them, textures are copied from their memory
   once a scene, all on the GPU. Each program becomes a SPIR-V shader (usse.h) at its first draw. The uniform buffers
   a draw reads are copied into a staging ring of the submission's, as the console's reserved ring space is the
   game's to reuse once the draw is made. A scene is submitted when it ends; the next begins once the GPU is done with
   it, so the game's next writes to its vertices and textures can't race the draws reading them */
#include "vita/gpu/render.h"
#include "vita/gpu/gxp.h"
#include "vita/gpu/usse.h"
#include "vita/gpu/vk.h"
#include "snx_settings.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMPORTS   (GXM_MAPPINGS + 16u)
#define SURFACES  16u
#define TEXTURES  256u
#define PIPELINES 256u
#define STAGING   (4u << 20)

typedef struct {
    uint32_t address, size;
    VkBuffer buffer;
    VkDeviceMemory memory;
} Import;
typedef struct {
    uint32_t address, width, height, stride, format;
    bool depth;
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    VkImageLayout layout;
} Surface;
typedef struct {
    uint32_t address, format, width, height, stride, type;
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    VkImageLayout layout;
    uint64_t scene; /* the scene it was copied in last */
} Texture;
typedef struct {
    VkShaderModule module;
    UsseShader meta; /* without its words */
    bool failed;
} Shader;
typedef struct {
    uint64_t key;
    VkPipeline pipeline;
} Pipeline;
struct VitaRender {
    VitaVk* vk;
    Import imports[IMPORTS];
    Surface surfaces[SURFACES];
    Texture textures[TEXTURES];
    Pipeline pipelines[PIPELINES];
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkSampler samplers[2][2][4]; /* min/mag linear, mip, address mode */
    struct {
        VkBuffer buffer;
        VkDeviceMemory memory;
        unsigned char* map;
        VkDeviceSize used;
    } staging[VITA_VK_SLOTS];
    /* the scene being recorded */
    bool rendering;
    Surface *color, *depth;
    uint32_t width, height;
    uint64_t scene;
    unsigned logged; /* each problem reported once: a bit each */
};

static struct VitaRender* render_of(VitaProcess* p) { return p->gxm ? p->gxm->render : NULL; }
static void once(struct VitaRender* r, unsigned bit, const char* format, ...) __attribute__((format(printf, 3, 4)));
static void once(struct VitaRender* r, unsigned bit, const char* format, ...) {
    if (r->logged & (1u << bit)) return;
    r->logged |= 1u << bit;
    va_list list;
    va_start(list, format);
    printf("gxm: ");
    vprintf(format, list);
    putchar('\n');
    va_end(list);
}
static void retire(unsigned slot);
static struct VitaRender* g_retiring; /* the renderer whose staging the device's retire frees */

/* ---- guest memory as buffers ---- */
static bool import_range(struct VitaRender* r, VitaProcess* p, uint32_t address, uint32_t size, Import* out) {
    VitaVk* v = r->vk;
    VkExternalMemoryBufferCreateInfo external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                                 .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .pNext = &external,
                             .size = size,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    void* host = vita_ptr(p, address);
    VkMemoryHostPointerPropertiesEXT pointer = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (((uintptr_t)host | size) & (v->host_alignment - 1)) return false;
    if (vkGetMemoryHostPointerPropertiesEXT(v->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host,
                                            &pointer) != VK_SUCCESS)
        return false;
    if (vkCreateBuffer(v->device, &bi, NULL, &out->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(v->device, out->buffer, &req);
    VkImportMemoryHostPointerInfoEXT info = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                             .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                             .pHostPointer = host};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .pNext = &info,
                               .allocationSize = size,
                               .memoryTypeIndex = vita_vk_memory_type(req.memoryTypeBits & pointer.memoryTypeBits,
                                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(v->device, &ai, NULL, &out->memory) != VK_SUCCESS ||
        vkBindBufferMemory(v->device, out->buffer, out->memory, 0) != VK_SUCCESS) {
        vkDestroyBuffer(v->device, out->buffer, NULL);
        if (out->memory) vkFreeMemory(v->device, out->memory, NULL);
        *out = (Import){0};
        return false;
    }
    out->address = address, out->size = size;
    return true;
}
void vita_render_map(VitaProcess* p, uint32_t address, uint32_t size) {
    struct VitaRender* r = render_of(p);
    if (!r) return;
    for (unsigned i = 0; i < IMPORTS; ++i)
        if (!r->imports[i].size) {
            if (!import_range(r, p, address, size, &r->imports[i]))
                once(r, 0, "the device cannot import guest memory at 0x%08" PRIx32, address);
            return;
        }
}
static void import_free(struct VitaRender* r, Import* m) {
    vkDestroyBuffer(r->vk->device, m->buffer, NULL);
    vkFreeMemory(r->vk->device, m->memory, NULL);
    *m = (Import){0};
}
void vita_render_unmap(VitaProcess* p, uint32_t address) {
    struct VitaRender* r = render_of(p);
    if (!r) return;
    vita_render_idle(p, true);
    for (unsigned i = 0; i < IMPORTS; ++i)
        if (r->imports[i].size && r->imports[i].address == address) import_free(r, &r->imports[i]);
}
/* the buffer holding [address, address + size) and the address's offset there; a range the game didn't map for the
   GPU (a framebuffer the CPU drew) is imported whole, as its memory block */
static const Import* buffer_of(struct VitaRender* r, VitaProcess* p, uint32_t address, uint32_t size, VkDeviceSize* at) {
    for (unsigned i = 0; i < IMPORTS; ++i) {
        const Import* m = &r->imports[i];
        if (m->size && address >= m->address && (uint64_t)address + size <= (uint64_t)m->address + m->size) {
            *at = address - m->address;
            return m;
        }
    }
    for (unsigned i = 0; i < VITA_ALLOCATIONS; ++i) {
        const VitaAllocation* a = &p->allocations[i];
        if (!a->size || address < a->address || (uint64_t)address + size > (uint64_t)a->address + a->size) continue;
        for (unsigned j = 0; j < IMPORTS; ++j)
            if (!r->imports[j].size) {
                if (!import_range(r, p, a->address, a->size, &r->imports[j])) return NULL;
                *at = address - a->address;
                return &r->imports[j];
            }
    }
    return NULL;
}

/* ---- images ---- */
static bool image_make(struct VitaRender* r, VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage,
                       VkImageAspectFlags aspect, VkComponentMapping components, VkImage* image, VkImageView* view,
                       VkDeviceMemory* memory) {
    VitaVk* v = r->vk;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = format,
                            .extent = {width, height, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(v->device, &ii, NULL, image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(v->device, *image, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = vita_vk_memory_type(req.memoryTypeBits,
                                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = vita_vk_memory_type(req.memoryTypeBits, 0);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = format,
                                .components = components,
                                .subresourceRange = {aspect, 0, 1, 0, 1}};
    if (vkAllocateMemory(v->device, &ai, NULL, memory) != VK_SUCCESS ||
        vkBindImageMemory(v->device, *image, *memory, 0) != VK_SUCCESS)
        return false;
    vi.image = *image;
    return vkCreateImageView(v->device, &vi, NULL, view) == VK_SUCCESS;
}
static void barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from,
                               .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = image,
                               .subresourceRange = {aspect, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}
static void memory_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}
/* the surface for a color surface's memory (or a depth surface), made at its first scene */
static Surface* surface(struct VitaRender* r, uint32_t address, uint32_t width, uint32_t height, uint32_t stride,
                        uint32_t format, bool depth) {
    Surface* free_slot = NULL;
    for (unsigned i = 0; i < SURFACES; ++i) {
        Surface* s = &r->surfaces[i];
        if (s->image && s->address == address && s->width == width && s->height == height && s->stride == stride &&
            s->format == format && s->depth == depth)
            return s;
        if (!s->image && !free_slot) free_slot = s;
    }
    if (!free_slot) return NULL;
    Surface* s = free_slot;
    VkComponentMapping identity = {0};
    bool ok = depth ? image_make(r, VK_FORMAT_D32_SFLOAT, width, height, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                 VK_IMAGE_ASPECT_DEPTH_BIT, identity, &s->image, &s->view, &s->memory)
                    : image_make(r, VK_FORMAT_R8G8B8A8_UNORM, width, height,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 VK_IMAGE_ASPECT_COLOR_BIT, identity, &s->image, &s->view, &s->memory);
    if (!ok) return NULL;
    s->address = address, s->width = width, s->height = height, s->stride = stride, s->format = format;
    s->depth = depth;
    s->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return s;
}

/* ---- the renderer ---- */
bool vita_render_init(VitaProcess* p) {
    VitaVk* v = vita_vk();
    if (!p->gxm || !v) return false;
    struct VitaRender* r = calloc(1, sizeof *r);
    if (!r) return false;
    r->vk = v;
    VkDescriptorSetLayoutBinding bindings[2 + USSE_TEXTURES];
    for (unsigned i = 0; i < 2; ++i)
        bindings[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1,
                                                     i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT, NULL};
    for (unsigned i = 0; i < USSE_TEXTURES; ++i)
        bindings[2 + i] = (VkDescriptorSetLayoutBinding){USSE_TEXTURE_BINDING + i,
                                                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                         VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 2 + USSE_TEXTURES,
                                          .pBindings = bindings};
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(UssePush)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &r->set_layout,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    bool ok = vkCreateDescriptorSetLayout(v->device, &si, NULL, &r->set_layout) == VK_SUCCESS &&
              vkCreatePipelineLayout(v->device, &li, NULL, &r->layout) == VK_SUCCESS;
    for (unsigned s = 0; ok && s < VITA_VK_SLOTS; ++s) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                 .size = STAGING,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        ok = vkCreateBuffer(v->device, &bi, NULL, &r->staging[s].buffer) == VK_SUCCESS;
        VkMemoryRequirements req;
        if (ok) vkGetBufferMemoryRequirements(v->device, r->staging[s].buffer, &req);
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .allocationSize = ok ? req.size : 0,
                                   .memoryTypeIndex = ok ? vita_vk_memory_type(req.memoryTypeBits,
                                                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                                         : 0};
        ok = ok && vkAllocateMemory(v->device, &ai, NULL, &r->staging[s].memory) == VK_SUCCESS &&
             vkBindBufferMemory(v->device, r->staging[s].buffer, r->staging[s].memory, 0) == VK_SUCCESS &&
             vkMapMemory(v->device, r->staging[s].memory, 0, STAGING, 0, (void**)&r->staging[s].map) == VK_SUCCESS;
    }
    for (unsigned lin = 0; ok && lin < 2; ++lin)
        for (unsigned mip = 0; ok && mip < 2; ++mip)
            for (unsigned mode = 0; ok && mode < 4; ++mode) {
                static const VkSamplerAddressMode modes[4] = {
                    VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
                    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
                VkSamplerCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                          .magFilter = lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                          .minFilter = lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                          .mipmapMode = mip ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                            : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                          .addressModeU = modes[mode],
                                          .addressModeV = modes[mode],
                                          .addressModeW = modes[mode],
                                          .maxLod = VK_LOD_CLAMP_NONE,
                                          .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
                ok = vkCreateSampler(v->device, &ci, NULL, &r->samplers[lin][mip][mode]) == VK_SUCCESS;
            }
    p->gxm->render = r;
    g_retiring = r;
    vita_vk_on_retire(retire);
    if (!ok) {
        printf("gxm: cannot set up the renderer: scenes are not drawn\n");
        vita_render_free(p);
        return false;
    }
    return true;
}
static void retire(unsigned slot) {
    if (g_retiring) g_retiring->staging[slot].used = 0;
}
void vita_render_shader_free(VitaProcess* p, void* shader) {
    struct VitaRender* r = render_of(p);
    Shader* s = shader;
    if (!s) return;
    if (r && s->module) vkDestroyShaderModule(r->vk->device, s->module, NULL);
    free(s);
}
void vita_render_free(VitaProcess* p) {
    struct VitaRender* r = render_of(p);
    if (!r) return;
    VkDevice d = r->vk->device;
    vkDeviceWaitIdle(d);
    vita_vk_on_retire(NULL);
    g_retiring = NULL;
    for (unsigned i = 0; i < GXM_PROGRAMS; ++i) {
        vita_render_shader_free(p, p->gxm->vertex[i].shader);
        vita_render_shader_free(p, p->gxm->fragment[i].shader);
        p->gxm->vertex[i].shader = p->gxm->fragment[i].shader = NULL;
    }
    for (unsigned i = 0; i < PIPELINES; ++i)
        if (r->pipelines[i].pipeline) vkDestroyPipeline(d, r->pipelines[i].pipeline, NULL);
    for (unsigned i = 0; i < SURFACES; ++i)
        if (r->surfaces[i].image) {
            vkDestroyImageView(d, r->surfaces[i].view, NULL);
            vkDestroyImage(d, r->surfaces[i].image, NULL);
            vkFreeMemory(d, r->surfaces[i].memory, NULL);
        }
    for (unsigned i = 0; i < TEXTURES; ++i)
        if (r->textures[i].image) {
            vkDestroyImageView(d, r->textures[i].view, NULL);
            vkDestroyImage(d, r->textures[i].image, NULL);
            vkFreeMemory(d, r->textures[i].memory, NULL);
        }
    for (unsigned i = 0; i < IMPORTS; ++i)
        if (r->imports[i].size) import_free(r, &r->imports[i]);
    for (unsigned s = 0; s < VITA_VK_SLOTS; ++s) {
        if (r->staging[s].buffer) vkDestroyBuffer(d, r->staging[s].buffer, NULL);
        if (r->staging[s].memory) vkFreeMemory(d, r->staging[s].memory, NULL);
    }
    for (unsigned i = 0; i < 16; ++i) {
        VkSampler s = (&r->samplers[0][0][0])[i];
        if (s) vkDestroySampler(d, s, NULL);
    }
    if (r->layout) vkDestroyPipelineLayout(d, r->layout, NULL);
    if (r->set_layout) vkDestroyDescriptorSetLayout(d, r->set_layout, NULL);
    free(r);
    p->gxm->render = NULL;
}

/* ---- scenes ---- */
/* SceGxmColorFormat's A8B8G8R8 family: the bytes R, G, B, A, the one surface format drawn yet */
static bool color_format_known(uint32_t format) { return (format & 0xffff0000u) == 0x00000000u; }
void vita_render_begin_scene(VitaProcess* p, GxmContext* c) {
    struct VitaRender* r = render_of(p);
    if (!r) return;
    /* the scene before drawn first: the game's next writes are its own (the description above says why) */
    vita_vk_wait(r->vk->submitted);
    ++r->scene;
    r->rendering = false;
    r->color = r->depth = NULL;
    if (!c->has_color) {
        once(r, 1, "a scene without a color surface is not drawn");
        return;
    }
    if (!color_format_known(c->color.format)) {
        once(r, 2, "color format 0x%08" PRIx32 " is not drawn yet", c->color.format);
        return;
    }
    const GxmTarget* target = &p->gxm->targets[c->target];
    r->width = c->color.width < target->width ? c->color.width : target->width;
    r->height = c->color.height < target->height ? c->color.height : target->height;
    r->color = surface(r, c->color.data, c->color.width, c->color.height, c->color.stride, c->color.format, false);
    VkDeviceSize at;
    const Import* m = r->color ? buffer_of(r, p, c->color.data, c->color.stride * 4 * c->color.height, &at) : NULL;
    if (!m) {
        once(r, 3, "the color surface at 0x%08" PRIx32 " is not in memory the GPU can reach", c->color.data);
        r->color = NULL;
        return;
    }
    if (c->has_depth && c->depth.depth)
        r->depth = surface(r, c->depth.depth, c->color.width, c->color.height, 0, 0, true);
    VkCommandBuffer cb = vita_vk_commands();
    /* the surface's memory into the image: what the CPU or earlier scenes left there */
    barrier(cb, r->color->image, VK_IMAGE_ASPECT_COLOR_BIT, r->color->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy = {.bufferOffset = at,
                              .bufferRowLength = c->color.stride,
                              .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {c->color.width, c->color.height, 1}};
    vkCmdCopyBufferToImage(cb, m->buffer, r->color->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(cb, r->color->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    r->color->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (r->depth) {
        barrier(cb, r->depth->image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        r->depth->layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }
    VkRenderingAttachmentInfo color = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                       .imageView = r->color->view,
                                       .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                       .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingAttachmentInfo depth = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                       .imageView = r->depth ? r->depth->view : VK_NULL_HANDLE,
                                       .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                       .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                       .storeOp = VK_ATTACHMENT_STORE_OP_STORE, /* a texture's copy restarts the pass */
                                       .clearValue.depthStencil = {c->depth.background_depth, 0}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{0, 0}, {r->width, r->height}},
                          .layerCount = 1,
                          .colorAttachmentCount = 1,
                          .pColorAttachments = &color,
                          .pDepthAttachment = r->depth ? &depth : NULL};
    vkCmdBeginRendering(cb, &ri);
    r->rendering = true;
}
uint64_t vita_render_end_scene(VitaProcess* p, GxmContext* c) {
    struct VitaRender* r = render_of(p);
    if (!r) return 0;
    if (r->rendering) {
        VkCommandBuffer cb = vita_vk_commands();
        vkCmdEndRendering(cb);
        /* the image back into the surface's memory, which the display and the game read */
        VkDeviceSize at;
        const Import* m = buffer_of(r, p, c->color.data, c->color.stride * 4 * c->color.height, &at);
        barrier(cb, r->color->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        r->color->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkBufferImageCopy copy = {.bufferOffset = at,
                                  .bufferRowLength = c->color.stride,
                                  .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {c->color.width, c->color.height, 1}};
        if (m) vkCmdCopyImageToBuffer(cb, r->color->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m->buffer, 1, &copy);
        memory_barrier(cb);
        r->rendering = false;
    }
    return vita_vk_submit();
}
bool vita_render_done(VitaProcess* p, uint64_t serial) {
    struct VitaRender* r = render_of(p);
    return !r || vita_vk_poll() >= serial;
}
bool vita_render_idle(VitaProcess* p, bool wait) {
    struct VitaRender* r = render_of(p);
    if (!r) return true;
    if (wait) return vita_vk_wait(r->vk->submitted);
    return vita_vk_poll() >= r->vk->submitted;
}
bool vita_render_show(VitaProcess* p, uint32_t address, uint32_t pitch, uint32_t width, uint32_t height) {
    struct VitaRender* r = render_of(p);
    if (!r || r->rendering) return false;
    VkDeviceSize at;
    const Import* m = buffer_of(r, p, address, pitch * 4 * height, &at);
    if (!m) return false;
    vita_vk_present(m->buffer, at, pitch, width, height);
    return true;
}
uint64_t vita_render_submitted(VitaProcess* p) {
    struct VitaRender* r = render_of(p);
    return r ? r->vk->submitted : 0;
}

/* ---- draws ---- */
static Shader* shader_for(struct VitaRender* r, VitaProcess* p, void** slot, uint32_t program, const UsseHints* hints) {
    if (*slot) return *slot;
    Shader* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    *slot = s;
    char error[256];
    UsseShader out;
    if (!usse_translate(vita_ptr(p, program), gxp_u32(vita_ptr(p, program + 8)), hints, &out, error, sizeof error)) {
        printf("gxm: the program at 0x%08" PRIx32 " is not drawn: %s\n", program, error);
        s->failed = true;
        return s;
    }
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = out.count * 4,
                                   .pCode = out.words};
    if (vkCreateShaderModule(r->vk->device, &mi, NULL, &s->module) != VK_SUCCESS) {
        printf("gxm: the device refuses the shader of the program at 0x%08" PRIx32 "\n", program);
        s->failed = true;
    }
    free(out.words);
    out.words = NULL;
    s->meta = out;
    return s;
}
/* SceGxmAttributeFormat and a component count as a vertex format */
static VkFormat attribute_format(unsigned format, unsigned n) {
    static const VkFormat formats[10][4] = {
        {VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT},
        {VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT},
        {VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT},
        {VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT},
        {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM},
        {VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM},
        {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM},
        {VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM},
        {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT},
        {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT}};
    return format < 10 && n >= 1 && n <= 4 ? formats[format][n - 1] : VK_FORMAT_UNDEFINED;
}
/* SceGxmBlendFactor and SceGxmBlendFunc as Vulkan's */
static VkBlendFactor blend_factor(unsigned f) {
    static const VkBlendFactor factors[] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_DST_ALPHA /* DST_ALPHA_SATURATE */};
    return f < sizeof factors / sizeof *factors ? factors[f] : VK_BLEND_FACTOR_ONE;
}
static VkBlendOp blend_op(unsigned f) {
    static const VkBlendOp ops[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT,
                                    VK_BLEND_OP_MIN, VK_BLEND_OP_MAX};
    return f < sizeof ops / sizeof *ops ? ops[f] : VK_BLEND_OP_ADD;
}
static uint64_t hash(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001b3ull; }
static VkPipeline pipeline_for(struct VitaRender* r, GxmContext* c, const GxmVertexProgram* vp, Shader* vs,
                               const GxmFragmentProgram* fp, Shader* fs, VkPrimitiveTopology topology) {
    uint64_t key = 0xcbf29ce484222325ull;
    key = hash(key, (uintptr_t)vs), key = hash(key, (uintptr_t)fs), key = hash(key, fp->blend_info | (uint64_t)fp->blend << 32);
    key = hash(key, topology), key = hash(key, c->cull_mode), key = hash(key, r->depth != NULL);
    key = hash(key, c->depth_func), key = hash(key, c->depth_write);
    for (unsigned i = 0; i < vp->stream_count; ++i) key = hash(key, vp->streams[i].stride | (uint64_t)vp->streams[i].index_source << 16);
    for (unsigned i = 0; i < vp->attribute_count; ++i) {
        const GxmAttribute* a = &vp->attributes[i];
        key = hash(key, a->stream | (uint64_t)a->offset << 8 | (uint64_t)a->format << 24 | (uint64_t)a->components << 32 |
                            (uint64_t)a->reg << 40);
    }
    unsigned slot = (unsigned)(key % PIPELINES);
    for (unsigned probe = 0; probe < PIPELINES; ++probe, slot = (slot + 1) % PIPELINES) {
        if (r->pipelines[slot].key == key && r->pipelines[slot].pipeline) return r->pipelines[slot].pipeline;
        if (!r->pipelines[slot].pipeline) break;
    }
    if (r->pipelines[slot].pipeline) {
        once(r, 4, "the pipeline cache is full");
        return VK_NULL_HANDLE;
    }
    VkVertexInputBindingDescription bindings[GXM_STREAMS];
    VkVertexInputAttributeDescription attributes[GXM_ATTRIBUTES];
    unsigned binding_count = 0, attribute_count = 0;
    for (unsigned i = 0; i < vp->stream_count; ++i)
        bindings[binding_count++] = (VkVertexInputBindingDescription){
            i, vp->streams[i].stride,
            vp->streams[i].index_source & 1 ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
    for (unsigned i = 0; i < vp->attribute_count; ++i) {
        int location = vs->meta.attribute_location[i];
        if (location < 0) continue;
        const GxmAttribute* a = &vp->attributes[i];
        VkFormat format = attribute_format(a->format, a->components);
        if (format == VK_FORMAT_UNDEFINED) {
            once(r, 5, "vertex attribute format %u is not drawn yet", a->format);
            return VK_NULL_HANDLE;
        }
        attributes[attribute_count++] = (VkVertexInputAttributeDescription){(uint32_t)location, a->stream, format,
                                                                            a->offset};
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vs->module, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fs->module, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                  .vertexBindingDescriptionCount = binding_count,
                                                  .pVertexBindingDescriptions = bindings,
                                                  .vertexAttributeDescriptionCount = attribute_count,
                                                  .pVertexAttributeDescriptions = attributes};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = topology};
    VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                  .viewportCount = 1,
                                                  .scissorCount = 1};
    /* SceGxmCullMode: none, clockwise, counter-clockwise; the viewport flips y, so a face's winding is the console's */
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = c->cull_mode ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
        .frontFace = c->cull_mode == 1 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    static const VkCompareOp compares[8] = {VK_COMPARE_OP_NEVER,         VK_COMPARE_OP_LESS,
                                            VK_COMPARE_OP_EQUAL,         VK_COMPARE_OP_LESS_OR_EQUAL,
                                            VK_COMPARE_OP_GREATER,       VK_COMPARE_OP_NOT_EQUAL,
                                            VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = r->depth != NULL,
        .depthWriteEnable = r->depth != NULL && c->depth_write == 0, /* SCE_GXM_DEPTH_WRITE_ENABLED is 0 */
        .depthCompareOp = compares[(c->depth_func >> 22) & 7]};
    uint32_t b = fp->blend_info;
    VkPipelineColorBlendAttachmentState blend = {
        .blendEnable = fp->blend,
        .srcColorBlendFactor = blend_factor((b >> 16) & 15),
        .dstColorBlendFactor = blend_factor((b >> 20) & 15),
        .colorBlendOp = blend_op((b >> 8) & 15),
        .srcAlphaBlendFactor = blend_factor((b >> 24) & 15),
        .dstAlphaBlendFactor = blend_factor((b >> 28) & 15),
        .alphaBlendOp = blend_op((b >> 12) & 15),
        /* SceGxmColorMask: A, R, G, B in bits 0 to 3 */
        .colorWriteMask = (b & 2 ? VK_COLOR_COMPONENT_R_BIT : 0) | (b & 4 ? VK_COLOR_COMPONENT_G_BIT : 0) |
                          (b & 8 ? VK_COLOR_COMPONENT_B_BIT : 0) | (b & 1 ? VK_COLOR_COMPONENT_A_BIT : 0)};
    VkPipelineColorBlendStateCreateInfo blending = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                    .attachmentCount = 1,
                                                    .pAttachments = &blend};
    VkDynamicState states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                .dynamicStateCount = 2,
                                                .pDynamicStates = states};
    VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                               .colorAttachmentCount = 1,
                                               .pColorAttachmentFormats = &color_format,
                                               .depthAttachmentFormat = r->depth ? VK_FORMAT_D32_SFLOAT
                                                                                 : VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .pNext = &rendering,
                                       .stageCount = 2,
                                       .pStages = stages,
                                       .pVertexInputState = &input,
                                       .pInputAssemblyState = &assembly,
                                       .pViewportState = &viewport,
                                       .pRasterizationState = &raster,
                                       .pMultisampleState = &multisample,
                                       .pDepthStencilState = &depth,
                                       .pColorBlendState = &blending,
                                       .pDynamicState = &dynamic,
                                       .layout = r->layout};
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(r->vk->device, VK_NULL_HANDLE, 1, &gi, NULL, &pipeline) != VK_SUCCESS) {
        once(r, 6, "the device refuses a pipeline");
        return VK_NULL_HANDLE;
    }
    r->pipelines[slot].key = key, r->pipelines[slot].pipeline = pipeline;
    return pipeline;
}
/* SceGxmTextureFormat's U8U8U8U8 (four bytes, R in the first for ABGR) and U8 (one) with their swizzles as an image
   format and the view's mapping; false for the formats not drawn yet */
static bool texture_format(uint32_t format, VkFormat* vk, unsigned* bytes, VkComponentMapping* map) {
    unsigned base = format & 0x9f000000u, swizzle = (format >> 12) & 7;
    const VkComponentSwizzle R = VK_COMPONENT_SWIZZLE_R, G = VK_COMPONENT_SWIZZLE_G, B = VK_COMPONENT_SWIZZLE_B,
                             A = VK_COMPONENT_SWIZZLE_A, O = VK_COMPONENT_SWIZZLE_ONE, Z = VK_COMPONENT_SWIZZLE_ZERO;
    if (base == 0x0c000000u) { /* the bytes read as R, G, B, A: ABGR has them in that order */
        static const VkComponentMapping maps[8] = {{R, G, B, A}, {B, G, R, A}, {A, B, G, R}, {G, B, A, R},
                                                   {R, G, B, O}, {B, G, R, O}, {A, B, G, O}, {G, B, A, O}};
        *vk = VK_FORMAT_R8G8B8A8_UNORM, *bytes = 4, *map = maps[swizzle];
        return true;
    }
    if (base == 0x00000000u) { /* SWIZZLE1: R, 000R, 111R, RRRR, 0RRR, 1RRR, R000, R111 (the letters A, B, G, R) */
        static const VkComponentMapping maps[8] = {{R, Z, Z, O}, {R, Z, Z, Z}, {R, O, O, O}, {R, R, R, R},
                                                   {R, R, R, Z}, {R, R, R, O}, {Z, Z, Z, R}, {O, O, O, R}};
        *vk = VK_FORMAT_R8_UNORM, *bytes = 1, *map = maps[swizzle];
        return true;
    }
    return false;
}
static Texture* texture_for(struct VitaRender* r, VitaProcess* p, VkCommandBuffer cb, const GxmTexture* x) {
    uint32_t type = gxm_texture_type(x), format = gxm_texture_format(x), data = gxm_texture_data(x);
    uint32_t width = gxm_texture_width(x), height = gxm_texture_height(x);
    VkFormat vk;
    unsigned bytes;
    VkComponentMapping map;
    if (type != GXM_TEXTURE_LINEAR && type != GXM_TEXTURE_LINEAR_STRIDED) {
        once(r, 7, "texture layout %u (swizzled or tiled) is not drawn yet", type);
        return NULL;
    }
    if (!texture_format(format, &vk, &bytes, &map)) {
        once(r, 8, "texture format 0x%08" PRIx32 " is not drawn yet", format);
        return NULL;
    }
    /* a linear texture's rows are 8-texel aligned (Vita3K's get_stride_in_bytes), a strided one's its own */
    uint32_t stride = type == GXM_TEXTURE_LINEAR_STRIDED ? gxm_texture_stride(x) : ((width + 7) & ~7u) * bytes;
    Texture *t = NULL, *free_slot = NULL;
    for (unsigned i = 0; i < TEXTURES && !t; ++i) {
        Texture* c = &r->textures[i];
        if (c->image && c->address == data && c->format == format && c->width == width && c->height == height &&
            c->stride == stride)
            t = c;
        else if (!c->image && !free_slot)
            free_slot = c;
    }
    if (!t) {
        if (!free_slot) { /* the oldest goes */
            free_slot = &r->textures[0];
            for (unsigned i = 1; i < TEXTURES; ++i)
                if (r->textures[i].scene < free_slot->scene) free_slot = &r->textures[i];
            vita_vk_wait(r->vk->submitted);
            vkDestroyImageView(r->vk->device, free_slot->view, NULL);
            vkDestroyImage(r->vk->device, free_slot->image, NULL);
            vkFreeMemory(r->vk->device, free_slot->memory, NULL);
            *free_slot = (Texture){0};
        }
        t = free_slot;
        if (!image_make(r, vk, width, height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, map, &t->image, &t->view, &t->memory))
            return NULL;
        t->address = data, t->format = format, t->width = width, t->height = height, t->stride = stride;
        t->type = type;
        t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    if (t->scene != r->scene) { /* its memory again, once a scene: the CPU may have changed it */
        VkDeviceSize at;
        const Import* m = buffer_of(r, p, data, stride * height, &at);
        if (!m) {
            once(r, 9, "texture memory at 0x%08" PRIx32 " is not mapped for the GPU", data);
            return NULL;
        }
        /* a copy can't run inside the rendering: it ends there and goes on after */
        vkCmdEndRendering(cb);
        barrier(cb, t->image, VK_IMAGE_ASPECT_COLOR_BIT, t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy = {.bufferOffset = at,
                                  .bufferRowLength = stride / bytes,
                                  .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {width, height, 1}};
        vkCmdCopyBufferToImage(cb, m->buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(cb, t->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        t->scene = r->scene;
        VkRenderingAttachmentInfo color = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                           .imageView = r->color->view,
                                           .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                           .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingAttachmentInfo depth = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                           .imageView = r->depth ? r->depth->view : VK_NULL_HANDLE,
                                           .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                           .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                              .renderArea = {{0, 0}, {r->width, r->height}},
                              .layerCount = 1,
                              .colorAttachmentCount = 1,
                              .pColorAttachments = &color,
                              .pDepthAttachment = r->depth ? &depth : NULL};
        vkCmdBeginRendering(cb, &ri);
    }
    return t;
}
/* a program's uniform buffers (UsseShader.buffers) from guest memory into the staging ring: the offset there, or
   UINT32_MAX without room */
static uint32_t stage_uniforms(struct VitaRender* r, VitaProcess* p, const UsseShader* meta, const uint32_t* buffers) {
    unsigned slot = vita_vk_slot();
    VkDeviceSize align = r->vk->properties.limits.minStorageBufferOffsetAlignment;
    VkDeviceSize at = (r->staging[slot].used + align - 1) / align * align, size = meta->buffer_words * 4u;
    if (!size) size = 4;
    if (at + size > STAGING) return UINT32_MAX;
    unsigned char* to = r->staging[slot].map + at;
    memset(to, 0, size);
    for (unsigned i = 0; i < meta->buffer_count; ++i) {
        uint32_t address = buffers[meta->buffers[i].container];
        if (address && vita_mapped(p, address, meta->buffers[i].words * 4))
            memcpy(to + meta->buffers[i].offset * 4, vita_ptr(p, address), meta->buffers[i].words * 4);
    }
    r->staging[slot].used = at + size;
    return (uint32_t)at;
}
void vita_render_draw(VitaProcess* p, GxmContext* c, uint32_t primitive, uint32_t index_format, uint32_t indices,
                      uint32_t count, uint32_t instances) {
    struct VitaRender* r = render_of(p);
    if (!r || !r->rendering) return;
    VitaGxm* g = p->gxm;
    GxmVertexProgram* vp = &g->vertex[(c->vertex_program - GXM_HANDLE_VERTEX) / 16];
    GxmFragmentProgram* fp = &g->fragment[(c->fragment_program - GXM_HANDLE_FRAGMENT) / 16];
    UsseHints vh = {0}, fh = {0};
    for (unsigned i = 0; i < vp->attribute_count && i < USSE_ATTRIBUTES; ++i) {
        vh.attributes[i].reg = vp->attributes[i].reg;
        vh.attributes[i].format = vp->attributes[i].format;
        vh.attributes[i].components = vp->attributes[i].components;
    }
    vh.attribute_count = vp->attribute_count;
    for (unsigned i = 0; i < USSE_TEXTURES; ++i) fh.texture_formats[i] = gxm_texture_format(&c->textures[i]);
    Shader* vs = shader_for(r, p, &vp->shader, vp->program, &vh);
    Shader* fs = shader_for(r, p, &fp->shader, fp->program, &fh);
    if (!vs || !fs || vs->failed || fs->failed) return;
    /* SceGxmPrimitiveType: triangles, lines, points, a strip, a fan (bits 26 to 28) */
    static const VkPrimitiveTopology topologies[6] = {
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    unsigned kind = primitive >> 26;
    if (kind >= 6) {
        once(r, 10, "primitive type 0x%08" PRIx32, primitive);
        return;
    }
    VkCommandBuffer cb = vita_vk_commands();
    /* the textures first: copying one ends the rendering for a moment */
    VkDescriptorImageInfo images[USSE_TEXTURES];
    for (unsigned i = 0; i < USSE_TEXTURES; ++i) {
        images[i] = (VkDescriptorImageInfo){0};
        if (!(fs->meta.sampler_mask & (1u << i))) continue;
        Texture* t = texture_for(r, p, cb, &c->textures[i]);
        if (!t) return;
        unsigned linear = gxm_texture_min_filter(&c->textures[i]) != 0 || gxm_texture_mag_filter(&c->textures[i]) != 0;
        unsigned mode = gxm_texture_uaddr(&c->textures[i]);
        images[i] = (VkDescriptorImageInfo){r->samplers[linear][0][mode == 0 ? 0 : mode == 1 ? 1 : 2], t->view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    VkPipeline pipeline = pipeline_for(r, c, vp, vs, fp, fs, topologies[kind]);
    if (!pipeline) return;
    uint32_t vertex_at = stage_uniforms(r, p, &vs->meta, c->vertex_uniform);
    uint32_t fragment_at = stage_uniforms(r, p, &fs->meta, c->fragment_uniform);
    if (vertex_at == UINT32_MAX || fragment_at == UINT32_MAX) {
        once(r, 11, "the uniform staging ring is full");
        return;
    }
    VkDescriptorSet set = vita_vk_set(r->set_layout);
    if (!set) {
        once(r, 12, "out of descriptor sets");
        return;
    }
    unsigned slot = vita_vk_slot();
    VkDescriptorBufferInfo buffers[2] = {{r->staging[slot].buffer, 0, vs->meta.buffer_words ? vs->meta.buffer_words * 4 : 4},
                                         {r->staging[slot].buffer, 0, fs->meta.buffer_words ? fs->meta.buffer_words * 4 : 4}};
    VkWriteDescriptorSet writes[2 + USSE_TEXTURES];
    unsigned n = 0;
    for (unsigned i = 0; i < 2; ++i)
        writes[n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = i,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,
                                             .pBufferInfo = &buffers[i]};
    for (unsigned i = 0; i < USSE_TEXTURES; ++i)
        if (images[i].imageView)
            writes[n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                 .dstSet = set,
                                                 .dstBinding = USSE_TEXTURE_BINDING + i,
                                                 .descriptorCount = 1,
                                                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                 .pImageInfo = &images[i]};
    vkUpdateDescriptorSets(r->vk->device, n, writes, 0, NULL);
    uint32_t offsets[2] = {vertex_at, fragment_at};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 0, 1, &set, 2, offsets);
    /* the viewport: GXM's maps [-1, 1] by its offsets and scales (y down for a negative scale), Vulkan's by an origin
       and size, which may be negative; the depth transform is the shader's (UssePush) */
    VkViewport viewport = {0, 0, (float)r->width, (float)r->height, 0, 1};
    UssePush push = {c->viewport_enabled ? 1.0f : 0.0f, (float)r->width, (float)r->height, 0.0f, 1.0f};
    if (c->viewport_enabled) {
        viewport.x = c->viewport_offset[0] - c->viewport_scale[0];
        viewport.width = 2 * c->viewport_scale[0];
        viewport.y = c->viewport_offset[1] - c->viewport_scale[1];
        viewport.height = 2 * c->viewport_scale[1];
        push.z_offset = c->viewport_offset[2], push.z_scale = c->viewport_scale[2];
    }
    vkCmdSetViewport(cb, 0, 1, &viewport);
    VkRect2D scissor = {{0, 0}, {r->width, r->height}};
    /* SceGxmRegionClipMode: none, all (0x40000000: nothing drawn), outside (0x80000000: drawing inside the rectangle
       only), inside (drawn whole, as Vita3K does) */
    if (c->region_clip_mode == 0x40000000u) return;
    if (c->region_clip_mode == 0x80000000u) {
        uint32_t x0 = c->clip_x0 < r->width ? c->clip_x0 : r->width, y0 = c->clip_y0 < r->height ? c->clip_y0 : r->height;
        uint32_t x1 = c->clip_x1 + 1 < r->width ? c->clip_x1 + 1 : r->width;
        uint32_t y1 = c->clip_y1 + 1 < r->height ? c->clip_y1 + 1 : r->height;
        scissor = (VkRect2D){{(int32_t)x0, (int32_t)y0}, {x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0}};
    }
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdPushConstants(cb, r->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, &push);
    for (unsigned i = 0; i < vp->stream_count; ++i) {
        VkDeviceSize at = 0;
        const Import* m = c->streams[i] ? buffer_of(r, p, c->streams[i], 1, &at) : NULL;
        if (!m) {
            once(r, 13, "vertex stream %u at 0x%08" PRIx32 " is not mapped for the GPU", i, c->streams[i]);
            return;
        }
        vkCmdBindVertexBuffers(cb, i, 1, &m->buffer, &at);
    }
    VkDeviceSize at = 0;
    const Import* m = buffer_of(r, p, indices, count * (index_format ? 4u : 2u), &at);
    if (!m) {
        once(r, 14, "indices at 0x%08" PRIx32 " are not mapped for the GPU", indices);
        return;
    }
    vkCmdBindIndexBuffer(cb, m->buffer, at, index_format ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cb, count, instances, 0, 0, 0);
    if (g_settings.vita_gxm_trace)
        printf("gxm: draw %u indices, primitive %u, program 0x%08" PRIx32 "/0x%08" PRIx32 "\n", (unsigned)count, kind,
               vp->program, fp->program);
}
