/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_VK_H
#define SNX_CTR_VK_H
#include "3ds/memory/ctr_memory.h"
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <volk.h>
/* The 3DS's one Vulkan device, which the PICA renderer, the transfer engine and the presenter share (device.c).
   The board's RAM regions (FCRAM, VRAM, DSP RAM, the New 3DS's extra RAM) are the host memory the guest runs in,
   imported into Vulkan as buffers (VK_EXT_external_memory_host): vertex and index data, textures, render targets'
   load and store, transfers and the shown framebuffers are read and written in place on the GPU, and nothing of them
   is copied by the CPU. Work is recorded into one command buffer until snx_ctr_vk_finish submits it and waits: the
   GX commands' ends do, as the guest's CPU may read what they wrote once their interrupt comes. */
typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint32_t base, size; /* the physical range it holds */
} SnxCtrVkRam;
typedef struct {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceMemoryProperties memory_properties;
    bool index8, logic_op; /* VK_EXT_index_type_uint8; the logicOp feature */
    SnxCtrVkRam ram[8];
    unsigned ram_count;
    SDL_Window* window;
    VkSurfaceKHR surface;
    /* the open recording, and what it allocated that lives until it is submitted and done */
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    VkDescriptorPool descriptors;
    bool recording;
} SnxCtrVk;
/* the device, once snx_ctr_vk_init made it; NULL before */
SnxCtrVk* snx_ctr_vk(void);
/* The device and the window (unless headless or none can be made), with memory's RAM regions imported. False, with
   the reason in error, when the host has no Vulkan device that can: Vulkan 1.2, 8-bit storage buffers and
   VK_EXT_external_memory_host */
bool snx_ctr_vk_init(SnxCtrMemory* memory, char* error, size_t error_size);
void snx_ctr_vk_free(void);
/* the open command buffer, begun if none was */
VkCommandBuffer snx_ctr_vk_commands(void);
/* the recording submitted and done: its writes are in guest memory, its descriptor sets free again. Nothing when
   nothing was recorded */
bool snx_ctr_vk_finish(void);
/* the same, the submission waiting for wait first and signaling signal when done (a swapchain's acquire and present) */
bool snx_ctr_vk_finish_with(VkSemaphore wait, VkSemaphore signal);
/* a descriptor set of the layout, valid until the next snx_ctr_vk_finish */
VkDescriptorSet snx_ctr_vk_set(VkDescriptorSetLayout layout);
/* the imported region that holds [pa, pa + size), and pa's offset into it; NULL when no RAM region holds it whole */
const SnxCtrVkRam* snx_ctr_vk_ram(uint32_t pa, uint32_t size, VkDeviceSize* offset);
/* A storage-buffer window of the RAM holding [pa, pa + size): the descriptor's offset at the alignment the device
   wants, and *inside where pa falls in it. False when no RAM region holds the span whole, or the window that holds it
   is past the device's largest storage range */
typedef struct {
    VkBuffer buffer;
    VkDeviceSize offset, range;
} SnxCtrVkWindow;
bool snx_ctr_vk_window(uint32_t pa, uint64_t size, SnxCtrVkWindow* window, uint32_t* inside);
/* a memory type of the bits with the properties, UINT32_MAX for none */
uint32_t snx_ctr_vk_memory_type(uint32_t bits, VkMemoryPropertyFlags properties);
/* a compute pipeline from SPIR-V words: bindings storage buffers (then images storage images) at set 0, push
   constants of push bytes. The layouts come back with it */
typedef struct {
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSetLayout set;
} SnxCtrVkCompute;
bool snx_ctr_vk_compute(const uint32_t* words, size_t count, unsigned buffers, unsigned images, uint32_t push,
                        SnxCtrVkCompute* out);
void snx_ctr_vk_compute_free(SnxCtrVkCompute* compute);
/* a buffer write the next commands may read or write, or one the host reads after snx_ctr_vk_finish */
void snx_ctr_vk_barrier(VkCommandBuffer cb);
#endif
