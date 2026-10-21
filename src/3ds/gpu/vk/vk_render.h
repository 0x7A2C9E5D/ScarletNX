/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_VK_RENDER_H
#define SNX_CTR_VK_RENDER_H
#include "3ds/gpu/pica.h"
#include <stdbool.h>
/* The PICA200's draws on the GPU (render.c): the vertex program and the fragment pipeline as generated shaders
   (vsgen.c, fsgen.c), the output merger as the pipeline's fixed state, drawn into images of the render targets the
   guest has in memory. A target is loaded from guest memory when a draw first uses it and stored back when the
   recording reaches anything else that reads or writes memory (snx_ctr_vk_render_flush): a transfer, a fill, a
   texture over it, the end of the recording. Textures are decoded from guest memory on the GPU (shaders/texture.comp)
   once in a recording. Vertex and index data are read in place from the imported RAM. */
/* the renderer of SnxCtrPicaHost (memory is the device's, imported): the draw triggers, DrawArrays indexed or not;
   one immediate-mode vertex, pica->immediate_vertex, kept until snx_ctr_vk_vertices_end draws the ones kept */
SnxCtrPicaResult snx_ctr_vk_draw(SnxCtrPica* pica, SnxCtrMemory* memory, bool indexed);
SnxCtrPicaResult snx_ctr_vk_vertex(SnxCtrPica* pica, SnxCtrMemory* memory);
SnxCtrPicaResult snx_ctr_vk_vertices_end(SnxCtrPica* pica, SnxCtrMemory* memory);
/* the open render target stored to guest memory and the rendering ended: the commands recorded next see what the draws
   wrote. The device calls it before a submission, the transfer engine and the presenter before their work */
void snx_ctr_vk_render_flush(void);
/* a submission done: what the recording used is free again (the device calls it) */
void snx_ctr_vk_render_done(void);
void snx_ctr_vk_render_free(void);
#endif
