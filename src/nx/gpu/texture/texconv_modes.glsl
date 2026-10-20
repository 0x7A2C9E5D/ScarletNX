/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* texconv.comp's conversions, by its mode push constant: C (vk/texconv.c) and GLSL (texconv.glsl) include this */
#ifndef SNX_TEXCONV_MODES
#define SNX_TEXCONV_MODES

/* 2 byte texels to RGBA8, a pair an invocation */
#define TC_RGBA4 0u    /* R in the low 4 bits */
#define TC_RGB5A1 1u   /* R in the low 5 bits, A the top bit */
#define TC_A5B5G5R1 2u /* R the low bit, then G, B and A of 5 */
/* RGB32F to RGBA32F with A 1.0, a texel an invocation */
#define TC_RGB32F 3u
/* packed depth and stencil to a depth aspect of 4 bytes a texel and a stencil aspect of 1, four texels an
   invocation: the depth in 24 bits (as they are, or as a float when the push constant zfloat says so) */
#define TC_D24S8 4u /* depth in the low 24 bits, stencil the top 8 */
#define TC_S8D24 5u /* stencil in the low 8 bits, depth the top 24 */
#define TC_D24X8 6u /* depth in the low 24 bits, stencil 0 */
#define TC_D32S8 7u /* 8 bytes: float depth, then the stencil byte */
/* ETC2 and EAC 4x4 blocks to pixels, a block an invocation (gpu/texture/etc2.c is the reference) */
#define TC_ETC2_RGB 8u
#define TC_ETC2_PTA 9u
#define TC_ETC2_RGBA 10u
#define TC_EAC_R 11u
#define TC_EAC_R_SNORM 12u
#define TC_EAC_RG 13u
#define TC_EAC_RG_SNORM 14u
/* the pixel formats of frames vi presents from memory (Android's, high bits first) to RGBA8: 2 byte ones a pair an
   invocation, 4 byte ones a pixel */
#define TC_VI_RGB565 15u
#define TC_VI_RGBA5551 16u
#define TC_VI_RGBA4444 17u
#define TC_VI_RGBX8888 18u
#define TC_VI_BGRA8888 19u

#endif
