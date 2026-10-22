/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sceGxm's state, which the API's functions (vita/hle/gxm.c) keep and the renderer (render.c) draws with */
#ifndef SNX_VITA_GXM_H
#define SNX_VITA_GXM_H
#include "vita/gpu/texture.h"
#include <stdbool.h>
#include <stdint.h>

#define GXM_STREAMS          16u
#define GXM_ATTRIBUTES       16u
#define GXM_TEXTURES         16u
#define GXM_UNIFORM_BUFFERS  15u /* the default one is GXP_DEFAULT_CONTAINER, 14 */
#define GXM_PROGRAMS         256u
#define GXM_TARGETS          16u
#define GXM_MAPPINGS         128u
#define GXM_DISPLAY_QUEUE    16u
/* the handles the API gives for what the runtime keeps (programs, render targets, sync objects, the patcher): addresses
   in the runtime's page, never read by the game */
#define GXM_HANDLE_VERTEX    0x80004000u
#define GXM_HANDLE_FRAGMENT  0x80005000u
#define GXM_HANDLE_TARGET    0x80006000u
#define GXM_HANDLE_SYNC      0x80007000u
#define GXM_HANDLE_PATCHER   0x80008000u

typedef struct {
    uint16_t stream, offset;
    uint8_t format, components;
    uint16_t reg; /* the attribute's resource index */
} GxmAttribute;
typedef struct {
    uint16_t stride, index_source;
} GxmStream;
typedef struct {
    bool used;
    uint32_t program; /* the GXP, in guest memory */
    GxmAttribute attributes[GXM_ATTRIBUTES];
    GxmStream streams[GXM_STREAMS];
    uint32_t attribute_count, stream_count;
    void* shader; /* the renderer's */
} GxmVertexProgram;
typedef struct {
    bool used, blend;
    uint32_t program, vertex_program; /* GXPs */
    uint32_t output_format, multisample;
    /* SceGxmBlendInfo: the color mask (bits 0 to 7), color and alpha functions (8 and 12), color source and destination
       (16, 20), alpha source and destination (24, 28) */
    uint32_t blend_info;
    void* shader;
} GxmFragmentProgram;
/* a color surface as sceGxmColorSurfaceInit made it (the guest's SceGxmColorSurface holds these words) */
typedef struct {
    uint32_t flags, width, height, stride, data, format, type, output_register_size;
} GxmColorSurface;
typedef struct {
    uint32_t control, depth, stencil;
    float background_depth;
    uint32_t background_control;
} GxmDepthSurface;
typedef struct {
    uint32_t width, height, flags, multisample;
} GxmTarget;
typedef struct {
    bool used;
    uint32_t address; /* the context's host memory, its handle */
    uint32_t vertex_ring, vertex_ring_size, vertex_ring_used;
    uint32_t fragment_ring, fragment_ring_size, fragment_ring_used;
    /* the state draws take */
    uint32_t vertex_program, fragment_program; /* handles */
    uint32_t streams[GXM_STREAMS];
    GxmTexture textures[GXM_TEXTURES]; /* the fragment program's */
    GxmTexture vertex_textures[4];
    uint32_t vertex_uniform[GXM_UNIFORM_BUFFERS], fragment_uniform[GXM_UNIFORM_BUFFERS]; /* addresses */
    uint32_t front_polygon_mode, back_polygon_mode, cull_mode, two_sided;
    uint32_t region_clip_mode, clip_x0, clip_y0, clip_x1, clip_y1;
    bool viewport_enabled;
    float viewport_offset[3], viewport_scale[3];
    /* depth and stencil, front and back */
    uint32_t depth_func, depth_write, stencil_func[2], stencil_ops[2][3], stencil_masks[2][2], stencil_ref[2];
    /* the scene */
    bool in_scene;
    uint32_t target;
    GxmColorSurface color;
    bool has_color;
    GxmDepthSurface depth;
    bool has_depth;
} GxmContext;
typedef struct VitaGxm {
    bool initialized;
    uint32_t notification_region; /* sceGxmGetNotificationRegion's, made at its first call */
    uint32_t callback, data_size, max_pending;
    /* the display queue: entries' copies of the game's data in guest memory, oldest first, each with the GPU
       submission it shows the results of */
    uint32_t queue_memory;
    struct {
        uint32_t data;
        uint64_t serial;
    } queue[GXM_DISPLAY_QUEUE];
    uint32_t queue_head, queue_count;
    uint32_t display_thread;
    GxmContext context;
    GxmVertexProgram vertex[GXM_PROGRAMS];
    GxmFragmentProgram fragment[GXM_PROGRAMS];
    GxmTarget targets[GXM_TARGETS];
    struct {
        uint32_t address, size;
    } mappings[GXM_MAPPINGS];
    uint64_t scenes, draws;
    struct VitaRender* render; /* render.c's */
} VitaGxm;
#endif
