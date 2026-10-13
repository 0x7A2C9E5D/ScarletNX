/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A C SPIR-V writer. Instructions append words directly to their logical sections;
   spv_emit_finish orders the sections into a Vulkan shader module. */
#ifndef SNX_SPIRV_EMIT_H
#define SNX_SPIRV_EMIT_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t* words;
    size_t count, capacity;
} SpvWords;

typedef struct {
    SpvWords capabilities, entry_points, execution_modes, debug, decorations, types, functions;
    uint32_t next_id;
} SpvEmitter;

typedef enum { SPV_STAGE_VERTEX = 0, SPV_STAGE_FRAGMENT = 4, SPV_STAGE_COMPUTE = 5 } SpvShaderStage;

void spv_emit_init(SpvEmitter* e, size_t estimated_function_words);
void spv_emit_destroy(SpvEmitter* e);
uint32_t spv_emit_id(SpvEmitter* e);
void spv_emit_shader_capability(SpvEmitter* e);
void spv_emit_entry_str(SpvEmitter* e, SpvShaderStage stage, uint32_t function, const char* name,
                        const uint32_t* interfaces, size_t interface_count);
void spv_emit_compute_entry_str(SpvEmitter* e, uint32_t function, const char* name, const uint32_t* interfaces,
                                size_t interface_count);
void spv_emit_fragment_origin_upper_left(SpvEmitter* e, uint32_t function);
void spv_emit_local_size(SpvEmitter* e, uint32_t function, uint32_t x, uint32_t y, uint32_t z);
void spv_emit_name_str(SpvEmitter* e, uint32_t id, const char* name);
uint32_t spv_emit_type_void(SpvEmitter* e);
uint32_t spv_emit_type_bool(SpvEmitter* e);
uint32_t spv_emit_type_u32(SpvEmitter* e);
uint32_t spv_emit_type_f32(SpvEmitter* e);
uint32_t spv_emit_type_vec4(SpvEmitter* e, uint32_t component_type);
uint32_t spv_emit_type_runtime_array(SpvEmitter* e, uint32_t element_type, uint32_t stride);
uint32_t spv_emit_type_struct_one(SpvEmitter* e, uint32_t member_type);
uint32_t spv_emit_type_storage_pointer(SpvEmitter* e, uint32_t pointee_type);
uint32_t spv_emit_type_function_void(SpvEmitter* e, uint32_t void_type);
uint32_t spv_emit_constant_u32(SpvEmitter* e, uint32_t type, uint32_t value);
uint32_t spv_emit_constant_f32(SpvEmitter* e, uint32_t type, float value);
uint32_t spv_emit_constant_vec4(SpvEmitter* e, uint32_t type, const uint32_t components[4]);
uint32_t spv_emit_storage_buffer(SpvEmitter* e, uint32_t struct_type, uint32_t set, uint32_t binding);
uint32_t spv_emit_output_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t location);
uint32_t spv_emit_position_output(SpvEmitter* e, uint32_t vec4_type);
void spv_emit_function_void(SpvEmitter* e, uint32_t void_type, uint32_t function, uint32_t function_type);
void spv_emit_label(SpvEmitter* e, uint32_t label);
uint32_t spv_emit_storage_access_u32(SpvEmitter* e, uint32_t pointer_type, uint32_t buffer, uint32_t member_index,
                                     uint32_t element_index);
uint32_t spv_emit_iadd_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right);
void spv_emit_store_u32(SpvEmitter* e, uint32_t pointer, uint32_t value);
void spv_emit_store(SpvEmitter* e, uint32_t pointer, uint32_t value);
void spv_emit_return(SpvEmitter* e);
void spv_emit_function_end(SpvEmitter* e);
/* The caller owns the returned words. A module is assembled once after all functions are emitted. */
uint32_t* spv_emit_finish(const SpvEmitter* e, size_t* word_count);
#endif
