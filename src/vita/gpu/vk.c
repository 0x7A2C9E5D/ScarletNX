/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/gpu/vk.h"
#include <SDL3/SDL_vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    VitaVk vk;
    bool up;
    void (*retire)(unsigned slot);
    /* the presenter's copy of the framebuffer, which the swapchain image is blitted from */
    VkImage frame;
    VkDeviceMemory frame_memory;
    uint32_t frame_width, frame_height;
    VkImageLayout frame_layout;
} g;

VitaVk* vita_vk(void) { return g.up ? &g.vk : NULL; }
void vita_vk_on_retire(void (*retire)(unsigned slot)) { g.retire = retire; }
uint32_t vita_vk_memory_type(uint32_t bits, VkMemoryPropertyFlags properties) {
    const VkPhysicalDeviceMemoryProperties* p = &g.vk.memory_properties;
    for (uint32_t i = 0; i < p->memoryTypeCount; ++i)
        if ((bits >> i & 1) && (p->memoryTypes[i].propertyFlags & properties) == properties) return i;
    return UINT32_MAX;
}
static bool has_extension(const VkExtensionProperties* list, uint32_t count, const char* name) {
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(list[i].extensionName, name)) return true;
    return false;
}
static bool make_device(VitaVk* v, VkPhysicalDevice phys, uint32_t family) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(phys, NULL, &count, NULL) != VK_SUCCESS || !count) return false;
    VkExtensionProperties* list = calloc(count, sizeof *list);
    if (!list) return false;
    vkEnumerateDeviceExtensionProperties(phys, NULL, &count, list);
    const char* extensions[2];
    uint32_t n = 0;
    bool ok = has_extension(list, count, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    extensions[n++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    if (v->surface) {
        ok = ok && has_extension(list, count, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        extensions[n++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    }
    free(list);
    if (!ok) return false;
    VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features v12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &v13};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v12};
    vkGetPhysicalDeviceFeatures2(phys, &features);
    if (!v13.dynamicRendering || !v13.synchronization2) return false;
    VkPhysicalDeviceVulkan13Features want13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                               .dynamicRendering = VK_TRUE,
                                               .synchronization2 = VK_TRUE};
    VkPhysicalDeviceVulkan12Features want12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                               .pNext = &want13,
                                               .shaderFloat16 = v12.shaderFloat16};
    VkPhysicalDeviceFeatures2 want = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &want12};
    want.features.fragmentStoresAndAtomics = features.features.fragmentStoresAndAtomics;
    want.features.shaderClipDistance = features.features.shaderClipDistance;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = family,
                                  .queueCount = 1,
                                  .pQueuePriorities = &priority};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .pNext = &want,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi,
                             .enabledExtensionCount = n,
                             .ppEnabledExtensionNames = extensions};
    if (vkCreateDevice(phys, &di, NULL, &v->device) != VK_SUCCESS) return false;
    v->phys = phys;
    v->family = family;
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceVulkan12Properties p12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
                                              .pNext = &host};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12};
    vkGetPhysicalDeviceProperties2(phys, &properties);
    v->host_alignment = host.minImportedHostPointerAlignment ? host.minImportedHostPointerAlignment : 4096;
    v->preserve_infnan = p12.shaderSignedZeroInfNanPreserveFloat32;
    return true;
}
static void swapchain_free(VitaVk* v) {
    if (!v->swapchain) return;
    vkDeviceWaitIdle(v->device);
    vkDestroySwapchainKHR(v->device, v->swapchain, NULL);
    v->swapchain = VK_NULL_HANDLE;
    v->swap_count = 0;
}
/* the window's swapchain, made anew for its size */
static bool swapchain_make(VitaVk* v) {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v->phys, v->surface, &caps) != VK_SUCCESS) return false;
    VkSurfaceFormatKHR formats[32];
    uint32_t count = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(v->phys, v->surface, &count, formats);
    if (!count) return false;
    VkSurfaceFormatKHR format = formats[0];
    for (uint32_t i = 0; i < count; ++i)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            format = formats[i];
            break;
        }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        int w = 960, h = 544;
        SDL_GetWindowSizeInPixels(v->window, &w, &h);
        extent = (VkExtent2D){(uint32_t)w, (uint32_t)h};
    }
    if (!extent.width || !extent.height) return false;
    uint32_t images = caps.minImageCount + 1;
    if (caps.maxImageCount && images > caps.maxImageCount) images = caps.maxImageCount;
    VkSwapchainKHR old = v->swapchain;
    VkSwapchainCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                   .surface = v->surface,
                                   .minImageCount = images,
                                   .imageFormat = format.format,
                                   .imageColorSpace = format.colorSpace,
                                   .imageExtent = extent,
                                   .imageArrayLayers = 1,
                                   .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                   .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                   .preTransform = caps.currentTransform,
                                   .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                   .presentMode = VK_PRESENT_MODE_FIFO_KHR,
                                   .clipped = VK_TRUE,
                                   .oldSwapchain = old};
    if (old) vkDeviceWaitIdle(v->device);
    VkResult r = vkCreateSwapchainKHR(v->device, &si, NULL, &v->swapchain);
    if (old) vkDestroySwapchainKHR(v->device, old, NULL);
    if (r != VK_SUCCESS) {
        v->swapchain = VK_NULL_HANDLE;
        return false;
    }
    v->swap_format = format.format;
    v->swap_extent = extent;
    v->swap_count = 8;
    vkGetSwapchainImagesKHR(v->device, v->swapchain, &v->swap_count, v->swap_images);
    return true;
}
bool vita_vk_init(const char* title, char* error, size_t error_size) {
    VitaVk* v = &g.vk;
    if (g.up) return true;
    memset(v, 0, sizeof *v);
    if (volkInitialize() != VK_SUCCESS) {
        snprintf(error, error_size, "no Vulkan loader");
        return false;
    }
    if (SDL_InitSubSystem(SDL_INIT_VIDEO))
        v->window = SDL_CreateWindow(title ? title : "ScarletNX Vita", 960, 544, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "scarletnx-vita",
                             .apiVersion = VK_API_VERSION_1_3};
    Uint32 extension_count = 0;
    const char* const* extensions = v->window ? SDL_Vulkan_GetInstanceExtensions(&extension_count) : NULL;
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app,
                               .enabledExtensionCount = v->window ? extension_count : 0,
                               .ppEnabledExtensionNames = v->window ? extensions : NULL};
    if (vkCreateInstance(&ii, NULL, &v->instance) != VK_SUCCESS) {
        if (v->window) SDL_DestroyWindow(v->window);
        v->window = NULL;
        ii.enabledExtensionCount = 0;
        ii.ppEnabledExtensionNames = NULL;
        if (vkCreateInstance(&ii, NULL, &v->instance) != VK_SUCCESS) {
            snprintf(error, error_size, "no Vulkan 1.3 instance");
            return false;
        }
    }
    volkLoadInstance(v->instance);
    if (v->window && !SDL_Vulkan_CreateSurface(v->window, v->instance, NULL, &v->surface)) {
        SDL_DestroyWindow(v->window);
        v->window = NULL;
    }
    uint32_t count = 0;
    VkPhysicalDevice devices[16];
    if (vkEnumeratePhysicalDevices(v->instance, &count, NULL) != VK_SUCCESS || !count) {
        snprintf(error, error_size, "no Vulkan device");
        vita_vk_free();
        return false;
    }
    if (count > 16) count = 16;
    vkEnumeratePhysicalDevices(v->instance, &count, devices);
    for (uint32_t i = 0; i < count && !v->device; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t families = 32;
        VkQueueFamilyProperties props[32];
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, props);
        for (uint32_t f = 0; f < families && !v->device; ++f) {
            VkBool32 present = VK_TRUE;
            if (v->surface && vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], f, v->surface, &present) != VK_SUCCESS)
                present = VK_FALSE;
            if (!(props[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            make_device(v, devices[i], f);
        }
    }
    if (!v->device) {
        snprintf(error, error_size, "no Vulkan 1.3 device with dynamic rendering and VK_EXT_external_memory_host");
        vita_vk_free();
        return false;
    }
    volkLoadDevice(v->device);
    vkGetDeviceQueue(v->device, v->family, 0, &v->queue);
    vkGetPhysicalDeviceProperties(v->phys, &v->properties);
    vkGetPhysicalDeviceMemoryProperties(v->phys, &v->memory_properties);
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                  .queueFamilyIndex = v->family};
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 8192},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 * 4096}};
    VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = 4096,
                                     .poolSizeCount = sizeof sizes / sizeof *sizes,
                                     .pPoolSizes = sizes};
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    bool made = vkCreateCommandPool(v->device, &pi, NULL, &v->pool) == VK_SUCCESS;
    ai.commandPool = v->pool;
    for (unsigned i = 0; i < VITA_VK_SLOTS && made; ++i)
        made = vkCreateFence(v->device, &fi, NULL, &v->slots[i].fence) == VK_SUCCESS &&
               vkCreateDescriptorPool(v->device, &dp, NULL, &v->slots[i].descriptors) == VK_SUCCESS &&
               vkAllocateCommandBuffers(v->device, &ai, &v->slots[i].cb) == VK_SUCCESS &&
               vkCreateSemaphore(v->device, &semaphore, NULL, &v->acquired[i]) == VK_SUCCESS;
    for (unsigned i = 0; i < 8 && made; ++i)
        made = vkCreateSemaphore(v->device, &semaphore, NULL, &v->rendered[i]) == VK_SUCCESS;
    if (!made) {
        snprintf(error, error_size, "cannot make the Vulkan device's command pool");
        vita_vk_free();
        return false;
    }
    g.up = true;
    if (v->surface && !swapchain_make(v)) fprintf(stderr, "scarletnx-vita: no swapchain for the window yet\n");
    return true;
}
void vita_vk_free(void) {
    VitaVk* v = &g.vk;
    if (v->device) {
        vkDeviceWaitIdle(v->device);
        swapchain_free(v);
        if (g.frame) vkDestroyImage(v->device, g.frame, NULL);
        if (g.frame_memory) vkFreeMemory(v->device, g.frame_memory, NULL);
        for (unsigned i = 0; i < VITA_VK_SLOTS; ++i) {
            if (v->slots[i].descriptors) vkDestroyDescriptorPool(v->device, v->slots[i].descriptors, NULL);
            if (v->slots[i].fence) vkDestroyFence(v->device, v->slots[i].fence, NULL);
            if (v->acquired[i]) vkDestroySemaphore(v->device, v->acquired[i], NULL);
        }
        for (unsigned i = 0; i < 8; ++i)
            if (v->rendered[i]) vkDestroySemaphore(v->device, v->rendered[i], NULL);
        if (v->pool) vkDestroyCommandPool(v->device, v->pool, NULL);
        vkDestroyDevice(v->device, NULL);
    }
    if (v->surface) vkDestroySurfaceKHR(v->instance, v->surface, NULL);
    if (v->instance) vkDestroyInstance(v->instance, NULL);
    if (v->window) SDL_DestroyWindow(v->window);
    memset(v, 0, sizeof *v);
    g.frame = VK_NULL_HANDLE, g.frame_memory = VK_NULL_HANDLE, g.frame_width = g.frame_height = 0;
    g.up = false;
}
static void retire(VitaVk* v, unsigned i) {
    vkResetFences(v->device, 1, &v->slots[i].fence);
    vkResetCommandBuffer(v->slots[i].cb, 0);
    vkResetDescriptorPool(v->device, v->slots[i].descriptors, 0);
    if (v->slots[i].serial > v->completed) v->completed = v->slots[i].serial;
    if (g.retire) g.retire(i);
    v->slots[i].serial = 0;
}
static unsigned oldest(const VitaVk* v, unsigned n) { return (v->slot + n) % VITA_VK_SLOTS; }
uint64_t vita_vk_poll(void) {
    VitaVk* v = &g.vk;
    if (!g.up) return 0;
    for (unsigned n = 0; n < VITA_VK_SLOTS; ++n) {
        unsigned i = oldest(v, n);
        if (!v->slots[i].serial || (v->recording && i == v->slot)) continue;
        if (vkGetFenceStatus(v->device, v->slots[i].fence) != VK_SUCCESS) break;
        retire(v, i);
    }
    return v->completed;
}
bool vita_vk_wait(uint64_t serial) {
    VitaVk* v = &g.vk;
    if (!g.up || serial <= v->completed) return true;
    if (serial > v->submitted) vita_vk_submit();
    for (unsigned n = 0; n < VITA_VK_SLOTS && v->completed < serial; ++n) {
        unsigned i = oldest(v, n);
        if (!v->slots[i].serial || v->slots[i].serial <= v->completed || (v->recording && i == v->slot)) continue;
        if (vkWaitForFences(v->device, 1, &v->slots[i].fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) return false;
        retire(v, i);
    }
    return v->completed >= serial;
}
VkCommandBuffer vita_vk_commands(void) {
    VitaVk* v = &g.vk;
    if (!v->recording) {
        if (v->slots[v->slot].serial) vita_vk_wait(v->slots[v->slot].serial);
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(v->slots[v->slot].cb, &bi);
        v->recording = true;
    }
    return v->slots[v->slot].cb;
}
unsigned vita_vk_slot(void) { return g.vk.slot; }
static uint64_t submit_with(VkSemaphore wait, VkSemaphore signal) {
    VitaVk* v = &g.vk;
    if (!g.up) return 0;
    if (!v->recording && !wait && !signal) return v->submitted;
    VkCommandBuffer cb = vita_vk_commands();
    /* what the GPU wrote is the host's to read once the fence is signaled */
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                            .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0,
                         NULL);
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .waitSemaphoreCount = wait ? 1 : 0,
                       .pWaitSemaphores = &wait,
                       .pWaitDstStageMask = &stage,
                       .commandBufferCount = 1,
                       .pCommandBuffers = &cb,
                       .signalSemaphoreCount = signal ? 1 : 0,
                       .pSignalSemaphores = &signal};
    v->recording = false;
    if (vkEndCommandBuffer(cb) != VK_SUCCESS || vkQueueSubmit(v->queue, 1, &si, v->slots[v->slot].fence) != VK_SUCCESS) {
        vkResetCommandBuffer(cb, 0);
        return v->submitted;
    }
    v->slots[v->slot].serial = ++v->submitted;
    v->slot = (v->slot + 1) % VITA_VK_SLOTS;
    return v->submitted;
}
uint64_t vita_vk_submit(void) { return submit_with(VK_NULL_HANDLE, VK_NULL_HANDLE); }
VkDescriptorSet vita_vk_set(VkDescriptorSetLayout layout) {
    VitaVk* v = &g.vk;
    vita_vk_commands();
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = v->slots[v->slot].descriptors,
                                      .descriptorSetCount = 1,
                                      .pSetLayouts = &layout};
    VkDescriptorSet set = VK_NULL_HANDLE;
    return vkAllocateDescriptorSets(v->device, &ai, &set) == VK_SUCCESS ? set : VK_NULL_HANDLE;
}
static void image_barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                          VkPipelineStageFlags2 src_stage, VkAccessFlags2 src, VkPipelineStageFlags2 dst_stage,
                          VkAccessFlags2 dst) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = src_stage,
                               .srcAccessMask = src,
                               .dstStageMask = dst_stage,
                               .dstAccessMask = dst,
                               .oldLayout = from,
                               .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = image,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}
/* the presenter's image of the framebuffer's size */
static bool frame_image(VitaVk* v, uint32_t width, uint32_t height) {
    if (g.frame && g.frame_width == width && g.frame_height == height) return true;
    vkDeviceWaitIdle(v->device);
    if (g.frame) vkDestroyImage(v->device, g.frame, NULL);
    if (g.frame_memory) vkFreeMemory(v->device, g.frame_memory, NULL);
    g.frame = VK_NULL_HANDLE, g.frame_memory = VK_NULL_HANDLE;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {width, height, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(v->device, &ii, NULL, &g.frame) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(v->device, g.frame, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = vita_vk_memory_type(req.memoryTypeBits,
                                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = vita_vk_memory_type(req.memoryTypeBits, 0);
    if (vkAllocateMemory(v->device, &ai, NULL, &g.frame_memory) != VK_SUCCESS ||
        vkBindImageMemory(v->device, g.frame, g.frame_memory, 0) != VK_SUCCESS)
        return false;
    g.frame_width = width, g.frame_height = height;
    g.frame_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}
void vita_vk_present(VkBuffer buffer, VkDeviceSize offset, uint32_t pitch, uint32_t width, uint32_t height) {
    VitaVk* v = &g.vk;
    if (!g.up || !v->surface) return;
    if (!v->swapchain && !swapchain_make(v)) return;
    if (!frame_image(v, width, height)) return;
    unsigned slot = v->slot;
    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(v->device, v->swapchain, UINT64_MAX, v->acquired[slot], VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_make(v);
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;
    VkCommandBuffer cb = vita_vk_commands();
    image_barrier(cb, g.frame, g.frame_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy = {.bufferOffset = offset,
                              .bufferRowLength = pitch,
                              .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {width, height, 1}};
    vkCmdCopyBufferToImage(cb, buffer, g.frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    image_barrier(cb, g.frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT);
    g.frame_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkImage target = v->swap_images[index];
    image_barrier(cb, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    image_barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT);
    /* the frame scaled to the window, its aspect kept */
    uint32_t w = v->swap_extent.width, h = v->swap_extent.height;
    uint32_t sw = w, sh = (uint32_t)((uint64_t)w * height / width);
    if (sh > h) sh = h, sw = (uint32_t)((uint64_t)h * width / height);
    int32_t x = (int32_t)(w - sw) / 2, y = (int32_t)(h - sh) / 2;
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .srcOffsets = {{0, 0, 0}, {(int32_t)width, (int32_t)height, 1}},
                        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                        .dstOffsets = {{x, y, 0}, {x + (int32_t)sw, y + (int32_t)sh, 1}}};
    vkCmdBlitImage(cb, g.frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_LINEAR);
    image_barrier(cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
    submit_with(v->acquired[slot], v->rendered[index]);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &v->rendered[index],
                           .swapchainCount = 1,
                           .pSwapchains = &v->swapchain,
                           .pImageIndices = &index};
    r = vkQueuePresentKHR(v->queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchain_make(v);
}
