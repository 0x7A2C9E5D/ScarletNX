/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/shader/spirv_emit.h"
#include "spirv_emitter_fixture.h"

uint32_t* snx_test_spirv_compute(size_t* words) {
    SpvEmitter e;
    spv_emit_init(&e, 1); /* force the function buffer's growth path */
    spv_emit_shader_capability(&e);
    uint32_t function = spv_emit_id(&e);
    uint32_t label = spv_emit_id(&e);
    uint32_t void_type = spv_emit_type_void(&e);
    uint32_t u32_type = spv_emit_type_u32(&e);
    uint32_t f32_type = spv_emit_type_f32(&e);
    uint32_t vec4_type = spv_emit_type_vec4(&e, f32_type);
    uint32_t function_type = spv_emit_type_function_void(&e, void_type);
    uint32_t array_type = spv_emit_type_runtime_array(&e, u32_type, 4);
    uint32_t struct_type = spv_emit_type_struct_one(&e, array_type);
    uint32_t buffer = spv_emit_storage_buffer(&e, struct_type, 0, 0);
    uint32_t pointer_type = spv_emit_type_storage_pointer(&e, u32_type);
    spv_emit_compute_entry_str(&e, function, "main", &buffer, 1);
    spv_emit_local_size(&e, function, 1, 1, 1);
    spv_emit_name_str(&e, function, "main");
    uint32_t one = spv_emit_constant_f32(&e, f32_type, 1.0f);
    uint32_t zero = spv_emit_constant_f32(&e, f32_type, 0.0f);
    uint32_t components[4] = {one, zero, zero, one};
    spv_emit_constant_vec4(&e, vec4_type, components);
    spv_emit_constant_u32(&e, u32_type, 42);
    spv_emit_type_bool(&e);
    uint32_t zero_index = spv_emit_constant_u32(&e, u32_type, 0);
    uint32_t forty = spv_emit_constant_u32(&e, u32_type, 40);
    uint32_t two = spv_emit_constant_u32(&e, u32_type, 2);
    spv_emit_function_void(&e, void_type, function, function_type);
    spv_emit_label(&e, label);
    uint32_t target = spv_emit_storage_access_u32(&e, pointer_type, buffer, zero_index, zero_index);
    uint32_t result = spv_emit_iadd_u32(&e, u32_type, forty, two);
    spv_emit_store_u32(&e, target, result);
    spv_emit_return(&e);
    spv_emit_function_end(&e);
    uint32_t* code = spv_emit_finish(&e, words);
    spv_emit_destroy(&e);
    return code;
}

uint32_t* snx_test_spirv_graphics(int fragment, size_t* words) {
    SpvEmitter e;
    spv_emit_init(&e, 64);
    spv_emit_shader_capability(&e);
    uint32_t function = spv_emit_id(&e);
    uint32_t label = spv_emit_id(&e);
    uint32_t void_type = spv_emit_type_void(&e);
    uint32_t f32_type = spv_emit_type_f32(&e);
    uint32_t u32_type = spv_emit_type_u32(&e);
    uint32_t vec4_type = spv_emit_type_vec4(&e, f32_type);
    uint32_t reg_type = spv_emit_type_function_pointer(&e, u32_type);
    uint32_t function_type = spv_emit_type_function_void(&e, void_type);
    uint32_t output = fragment ? spv_emit_output_vec4(&e, vec4_type, 0) : spv_emit_position_output(&e, vec4_type);
    uint32_t output2 = fragment ? spv_emit_output_vec4(&e, vec4_type, 1) : 0;
    uint32_t interfaces[2] = {output, output2};
    spv_emit_entry_str(&e, fragment ? SPV_STAGE_FRAGMENT : SPV_STAGE_VERTEX, function, "main", interfaces,
                       fragment ? 2 : 1);
    if (fragment) spv_emit_fragment_origin_upper_left(&e, function);
    uint32_t zero = spv_emit_constant_f32(&e, f32_type, 0.0f);
    uint32_t one = spv_emit_constant_f32(&e, f32_type, 1.0f);
    uint32_t half_bits = spv_emit_constant_u32(&e, u32_type, 0x3f000000);
    uint32_t vertex_components[4] = {zero, zero, zero, one};
    uint32_t vertex_color = spv_emit_constant_vec4(&e, vec4_type, vertex_components);
    spv_emit_function_void(&e, void_type, function, function_type);
    spv_emit_label(&e, label);
    uint32_t color = vertex_color;
    if (fragment) {
        uint32_t reg = spv_emit_function_variable(&e, reg_type);
        spv_emit_store(&e, reg, half_bits);
        uint32_t bits = spv_emit_load(&e, u32_type, reg);
        uint32_t half = spv_emit_bitcast(&e, f32_type, bits);
        uint32_t sum = spv_emit_fadd(&e, f32_type, half, half);
        uint32_t product = spv_emit_fmul(&e, f32_type, half, half);
        spv_emit_no_contraction(&e, sum);
        spv_emit_no_contraction(&e, product);
        uint32_t components[4] = {sum, product, zero, one};
        color = spv_emit_vec4_construct(&e, vec4_type, components);
    }
    spv_emit_store(&e, output, color);
    if (fragment) spv_emit_store(&e, output2, color);
    spv_emit_return(&e);
    spv_emit_function_end(&e);
    uint32_t* code = spv_emit_finish(&e, words);
    spv_emit_destroy(&e);
    return code;
}
