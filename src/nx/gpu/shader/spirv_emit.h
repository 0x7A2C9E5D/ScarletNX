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
    uint32_t storage, pointee, id;
} SpvPointerType;

typedef struct {
    SpvWords capabilities, extensions, imports, entry_points, execution_modes, debug, decorations, types, functions;
    uint32_t next_id;
    SpvPointerType* pointer_types;
    size_t pointer_count, pointer_capacity;
    uint32_t sampled_image_types[2], image_types[2], sampled_image_pointee;
} SpvEmitter;

typedef enum { SPV_STAGE_VERTEX = 0, SPV_STAGE_FRAGMENT = 4, SPV_STAGE_COMPUTE = 5 } SpvShaderStage;
typedef enum {
    SPV_GLSL_FABS = 4,
    SPV_GLSL_COS = 14,
    SPV_GLSL_SIN = 13,
    SPV_GLSL_EXP2 = 29,
    SPV_GLSL_LOG2 = 30,
    SPV_GLSL_SQRT = 31,
    SPV_GLSL_INVERSE_SQRT = 32,
    SPV_GLSL_FMIN = 37,
    SPV_GLSL_FMAX = 40,
    SPV_GLSL_FCLAMP = 43,
    SPV_GLSL_FMA = 50
} SpvGlsl450Op;

void spv_emit_init(SpvEmitter* e, size_t estimated_function_words);
void spv_emit_destroy(SpvEmitter* e);
uint32_t spv_emit_id(SpvEmitter* e);
uint32_t spv_emit_glsl450_import(SpvEmitter* e);
void spv_emit_shader_capability(SpvEmitter* e);
void spv_emit_entry_str(SpvEmitter* e, SpvShaderStage stage, uint32_t function, const char* name,
                        const uint32_t* interfaces, size_t interface_count);
void spv_emit_compute_entry_str(SpvEmitter* e, uint32_t function, const char* name, const uint32_t* interfaces,
                                size_t interface_count);
void spv_emit_fragment_origin_upper_left(SpvEmitter* e, uint32_t function);
void spv_emit_float_controls(SpvEmitter* e, uint32_t function, int flush_denorms, int preserve_infnan);
void spv_emit_local_size(SpvEmitter* e, uint32_t function, uint32_t x, uint32_t y, uint32_t z);
void spv_emit_name_str(SpvEmitter* e, uint32_t id, const char* name);
uint32_t spv_emit_type_void(SpvEmitter* e);
uint32_t spv_emit_type_bool(SpvEmitter* e);
uint32_t spv_emit_type_u32(SpvEmitter* e);
uint32_t spv_emit_type_f32(SpvEmitter* e);
uint32_t spv_emit_type_vec4(SpvEmitter* e, uint32_t component_type);
uint32_t spv_emit_type_vec2(SpvEmitter* e, uint32_t component_type);
uint32_t spv_emit_type_runtime_array(SpvEmitter* e, uint32_t element_type, uint32_t stride);
uint32_t spv_emit_type_struct_one(SpvEmitter* e, uint32_t member_type);
uint32_t spv_emit_type_storage_pointer(SpvEmitter* e, uint32_t pointee_type);
uint32_t spv_emit_type_function_pointer(SpvEmitter* e, uint32_t pointee_type);
uint32_t spv_emit_type_function_void(SpvEmitter* e, uint32_t void_type);
uint32_t spv_emit_constant_u32(SpvEmitter* e, uint32_t type, uint32_t value);
uint32_t spv_emit_constant_bool(SpvEmitter* e, uint32_t type, int value);
uint32_t spv_emit_constant_f32(SpvEmitter* e, uint32_t type, float value);
uint32_t spv_emit_constant_vec4(SpvEmitter* e, uint32_t type, const uint32_t components[4]);
uint32_t spv_emit_storage_buffer(SpvEmitter* e, uint32_t struct_type, uint32_t set, uint32_t binding);
uint32_t spv_emit_uniform_vec4_array(SpvEmitter* e, uint32_t vec4_type, uint32_t f32_type, uint32_t u32_type,
                                     uint32_t set, uint32_t binding, uint32_t elements,
                                     uint32_t* component_pointer_type);
uint32_t spv_emit_uniform_component_f32(SpvEmitter* e, uint32_t pointer_type, uint32_t buffer, uint32_t member,
                                        uint32_t element, uint32_t component, uint32_t f32_type);
uint32_t spv_emit_push_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t f32_type, uint32_t offset,
                            uint32_t* component_pointer_type);
uint32_t spv_emit_push_component_f32(SpvEmitter* e, uint32_t pointer_type, uint32_t block, uint32_t member,
                                     uint32_t component, uint32_t f32_type);
uint32_t spv_emit_output_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t location);
uint32_t spv_emit_position_output(SpvEmitter* e, uint32_t vec4_type);
uint32_t spv_emit_point_size_output(SpvEmitter* e, uint32_t f32_type);
/* interpolation: 0 perspective, 1 flat, 2 no perspective */
uint32_t spv_emit_input_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t location, int interpolation);
uint32_t spv_emit_fragcoord_input(SpvEmitter* e, uint32_t vec4_type);
uint32_t spv_emit_sampler2d(SpvEmitter* e, uint32_t f32_type, uint32_t set, uint32_t binding, int shadow,
                            uint32_t* sampled_type);
void spv_emit_function_void(SpvEmitter* e, uint32_t void_type, uint32_t function, uint32_t function_type);
void spv_emit_label(SpvEmitter* e, uint32_t label);
uint32_t spv_emit_function_variable(SpvEmitter* e, uint32_t pointer_type);
uint32_t spv_emit_function_variable_init(SpvEmitter* e, uint32_t pointer_type, uint32_t initial_value);
uint32_t spv_emit_load(SpvEmitter* e, uint32_t result_type, uint32_t pointer);
uint32_t spv_emit_bitcast(SpvEmitter* e, uint32_t result_type, uint32_t value);
uint32_t spv_emit_fadd(SpvEmitter* e, uint32_t f32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_fmul(SpvEmitter* e, uint32_t f32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_i_equal(SpvEmitter* e, uint32_t bool_type, uint32_t left, uint32_t right);
uint32_t spv_emit_fcompare(SpvEmitter* e, uint32_t bool_type, uint32_t condition, uint32_t left, uint32_t right);
uint32_t spv_emit_bool_not(SpvEmitter* e, uint32_t bool_type, uint32_t value);
uint32_t spv_emit_bool_binary(SpvEmitter* e, uint32_t bool_type, uint32_t op, uint32_t left, uint32_t right);
uint32_t spv_emit_fdiv(SpvEmitter* e, uint32_t f32_type, uint32_t left, uint32_t right);
uint32_t spv_emit_fnegate(SpvEmitter* e, uint32_t f32_type, uint32_t value);
uint32_t spv_emit_glsl_unary(SpvEmitter* e, uint32_t f32_type, uint32_t import, SpvGlsl450Op op, uint32_t value);
uint32_t spv_emit_glsl_binary(SpvEmitter* e, uint32_t f32_type, uint32_t import, SpvGlsl450Op op, uint32_t left,
                              uint32_t right);
uint32_t spv_emit_glsl_ternary(SpvEmitter* e, uint32_t f32_type, uint32_t import, SpvGlsl450Op op, uint32_t a,
                               uint32_t b, uint32_t c);
uint32_t spv_emit_isnan(SpvEmitter* e, uint32_t bool_type, uint32_t value);
uint32_t spv_emit_select(SpvEmitter* e, uint32_t result_type, uint32_t condition, uint32_t yes, uint32_t no);
uint32_t spv_emit_vec4_extract(SpvEmitter* e, uint32_t component_type, uint32_t vector, uint32_t component);
uint32_t spv_emit_vec4_insert(SpvEmitter* e, uint32_t vec4_type, uint32_t vector, uint32_t component_value,
                              uint32_t component);
uint32_t spv_emit_vec4_construct(SpvEmitter* e, uint32_t vec4_type, const uint32_t components[4]);
uint32_t spv_emit_vec2_construct(SpvEmitter* e, uint32_t vec2_type, uint32_t x, uint32_t y);
uint32_t spv_emit_sample2d(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                           uint32_t coordinates, uint32_t depth_reference);
void spv_emit_no_contraction(SpvEmitter* e, uint32_t result);
uint32_t spv_emit_storage_access_u32(SpvEmitter* e, uint32_t pointer_type, uint32_t buffer, uint32_t member_index,
                                     uint32_t element_index);
uint32_t spv_emit_iadd_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right);
void spv_emit_store_u32(SpvEmitter* e, uint32_t pointer, uint32_t value);
void spv_emit_store(SpvEmitter* e, uint32_t pointer, uint32_t value);
void spv_emit_return(SpvEmitter* e);
void spv_emit_kill(SpvEmitter* e);
void spv_emit_selection_merge(SpvEmitter* e, uint32_t merge_label);
void spv_emit_branch_conditional(SpvEmitter* e, uint32_t condition, uint32_t yes_label, uint32_t no_label);
void spv_emit_branch(SpvEmitter* e, uint32_t label);
void spv_emit_loop_merge(SpvEmitter* e, uint32_t merge_label, uint32_t continue_label);
void spv_emit_switch_u32(SpvEmitter* e, uint32_t selector, uint32_t default_label, const uint32_t* literals,
                         const uint32_t* labels, size_t count);
void spv_emit_function_end(SpvEmitter* e);
/* The caller owns the returned words. A module is assembled once after all functions are emitted. */
uint32_t* spv_emit_finish(const SpvEmitter* e, size_t* word_count);
#endif
