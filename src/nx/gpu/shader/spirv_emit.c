/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/shader/spirv_emit.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

enum {
    OP_NAME = 5,
    OP_EXTENSION = 10,
    OP_EXT_INST_IMPORT = 11,
    OP_EXT_INST = 12,
    OP_MEMORY_MODEL = 14,
    OP_ENTRY_POINT = 15,
    OP_EXECUTION_MODE = 16,
    OP_CAPABILITY = 17,
    OP_TYPE_VOID = 19,
    OP_TYPE_BOOL = 20,
    OP_TYPE_INT = 21,
    OP_TYPE_FLOAT = 22,
    OP_TYPE_VECTOR = 23,
    OP_TYPE_IMAGE = 25,
    OP_TYPE_SAMPLED_IMAGE = 27,
    OP_TYPE_ARRAY = 28,
    OP_TYPE_RUNTIME_ARRAY = 29,
    OP_TYPE_STRUCT = 30,
    OP_TYPE_POINTER = 32,
    OP_TYPE_FUNCTION = 33,
    OP_CONSTANT_TRUE = 41,
    OP_CONSTANT_FALSE = 42,
    OP_CONSTANT = 43,
    OP_CONSTANT_COMPOSITE = 44,
    OP_FUNCTION = 54,
    OP_FUNCTION_END = 56,
    OP_VARIABLE = 59,
    OP_LOAD = 61,
    OP_STORE = 62,
    OP_ACCESS_CHAIN = 65,
    OP_DECORATE = 71,
    OP_MEMBER_DECORATE = 72,
    OP_IADD = 128,
    OP_FADD = 129,
    OP_FMUL = 133,
    OP_FDIV = 136,
    OP_I_EQUAL = 170,
    OP_IS_NAN = 156,
    OP_SELECT = 169,
    OP_LOGICAL_NOT = 168,
    OP_LOGICAL_OR = 166,
    OP_LOGICAL_AND = 167,
    OP_LOGICAL_NOT_EQUAL = 165,
    OP_FNEGATE = 127,
    OP_BITCAST = 124,
    OP_COMPOSITE_CONSTRUCT = 80,
    OP_COMPOSITE_EXTRACT = 81,
    OP_COMPOSITE_INSERT = 82,
    OP_IMAGE_SAMPLE_IMPLICIT_LOD = 87,
    OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD = 89,
    OP_LABEL = 248,
    OP_LOOP_MERGE = 246,
    OP_SELECTION_MERGE = 247,
    OP_BRANCH = 249,
    OP_BRANCH_CONDITIONAL = 250,
    OP_SWITCH = 251,
    OP_RETURN = 253,
    OP_KILL = 252,
};

static void reserve(SpvWords* s, size_t more) {
    assert(more <= SIZE_MAX - s->count);
    size_t need = s->count + more;
    if (need <= s->capacity) return;
    size_t cap = s->capacity ? s->capacity : 64;
    while (cap < need) {
        assert(cap <= SIZE_MAX / 2);
        cap *= 2;
    }
    assert(cap <= SIZE_MAX / sizeof(uint32_t));
    uint32_t* p = realloc(s->words, cap * sizeof(uint32_t));
    if (!p) abort();
    s->words = p;
    s->capacity = cap;
}

static void instruction(SpvWords* s, uint32_t op, uint32_t words) {
    assert(op <= UINT16_MAX && words > 0 && words <= UINT16_MAX);
    reserve(s, words);
    s->words[s->count++] = (words << 16) | op;
}

static void put(SpvWords* s, uint32_t word) {
    assert(s->count < s->capacity);
    s->words[s->count++] = word;
}

static size_t string_words(const char* str) {
    size_t n = strlen(str);
    assert(n <= SIZE_MAX - 4);
    return (n + 4) / 4;
}

static void put_str(SpvWords* s, const char* str) {
    size_t n = strlen(str) + 1;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t word = 0;
        for (size_t j = 0; j < 4 && i + j < n; j++) word |= (uint32_t)(uint8_t)str[i + j] << (8 * j);
        put(s, word);
    }
}

void spv_emit_init(SpvEmitter* e, size_t estimated_function_words) {
    memset(e, 0, sizeof *e);
    e->next_id = 1;
    /* Most instructions are in function bodies. Reserve from the guest program size once. */
    if (estimated_function_words) reserve(&e->functions, estimated_function_words);
}

void spv_emit_destroy(SpvEmitter* e) {
    SpvWords* sections[] = {&e->capabilities, &e->extensions,  &e->imports, &e->entry_points, &e->execution_modes,
                            &e->debug,        &e->decorations, &e->types,   &e->functions};
    for (size_t i = 0; i < sizeof sections / sizeof sections[0]; i++) free(sections[i]->words);
    free(e->pointer_types);
    memset(e, 0, sizeof *e);
}

uint32_t spv_emit_id(SpvEmitter* e) {
    assert(e->next_id && e->next_id < UINT32_MAX);
    return e->next_id++;
}

uint32_t spv_emit_glsl450_import(SpvEmitter* e) {
    const char* name = "GLSL.std.450";
    uint32_t id = spv_emit_id(e);
    instruction(&e->imports, OP_EXT_INST_IMPORT, (uint32_t)(2 + string_words(name)));
    put(&e->imports, id);
    put_str(&e->imports, name);
    return id;
}

void spv_emit_shader_capability(SpvEmitter* e) {
    instruction(&e->capabilities, OP_CAPABILITY, 2);
    put(&e->capabilities, 1); /* Shader */
}

void spv_emit_entry_str(SpvEmitter* e, SpvShaderStage stage, uint32_t function, const char* name,
                        const uint32_t* interfaces, size_t interface_count) {
    assert(stage == SPV_STAGE_VERTEX || stage == SPV_STAGE_FRAGMENT || stage == SPV_STAGE_COMPUTE);
    size_t n = string_words(name);
    assert(n <= UINT16_MAX - 3 && interface_count <= UINT16_MAX - 3 - n);
    instruction(&e->entry_points, OP_ENTRY_POINT, (uint32_t)(3 + n + interface_count));
    put(&e->entry_points, (uint32_t)stage);
    put(&e->entry_points, function);
    put_str(&e->entry_points, name);
    for (size_t i = 0; i < interface_count; i++) put(&e->entry_points, interfaces[i]);
}

void spv_emit_compute_entry_str(SpvEmitter* e, uint32_t function, const char* name, const uint32_t* interfaces,
                                size_t interface_count) {
    spv_emit_entry_str(e, SPV_STAGE_COMPUTE, function, name, interfaces, interface_count);
}

void spv_emit_fragment_origin_upper_left(SpvEmitter* e, uint32_t function) {
    instruction(&e->execution_modes, OP_EXECUTION_MODE, 3);
    put(&e->execution_modes, function);
    put(&e->execution_modes, 7); /* OriginUpperLeft */
}

void spv_emit_float_controls(SpvEmitter* e, uint32_t function, int flush_denorms, int preserve_infnan) {
    if (!flush_denorms && !preserve_infnan) return;
    const char* name = "SPV_KHR_float_controls";
    instruction(&e->extensions, OP_EXTENSION, (uint32_t)(1 + string_words(name)));
    put_str(&e->extensions, name);
    if (flush_denorms) {
        instruction(&e->capabilities, OP_CAPABILITY, 2);
        put(&e->capabilities, 4465); /* DenormFlushToZero */
        instruction(&e->execution_modes, OP_EXECUTION_MODE, 4);
        put(&e->execution_modes, function);
        put(&e->execution_modes, 4460); /* DenormFlushToZero */
        put(&e->execution_modes, 32);
    }
    if (preserve_infnan) {
        instruction(&e->capabilities, OP_CAPABILITY, 2);
        put(&e->capabilities, 4466); /* SignedZeroInfNanPreserve */
        instruction(&e->execution_modes, OP_EXECUTION_MODE, 4);
        put(&e->execution_modes, function);
        put(&e->execution_modes, 4461); /* SignedZeroInfNanPreserve */
        put(&e->execution_modes, 32);
    }
}

void spv_emit_local_size(SpvEmitter* e, uint32_t function, uint32_t x, uint32_t y, uint32_t z) {
    assert(x && y && z);
    instruction(&e->execution_modes, OP_EXECUTION_MODE, 6);
    put(&e->execution_modes, function);
    put(&e->execution_modes, 17); /* LocalSize */
    put(&e->execution_modes, x);
    put(&e->execution_modes, y);
    put(&e->execution_modes, z);
}

void spv_emit_name_str(SpvEmitter* e, uint32_t id, const char* name) {
    size_t n = string_words(name);
    assert(n <= UINT16_MAX - 2);
    instruction(&e->debug, OP_NAME, (uint32_t)(2 + n));
    put(&e->debug, id);
    put_str(&e->debug, name);
}

uint32_t spv_emit_type_void(SpvEmitter* e) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, OP_TYPE_VOID, 2);
    put(&e->types, id);
    return id;
}

uint32_t spv_emit_type_bool(SpvEmitter* e) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, OP_TYPE_BOOL, 2);
    put(&e->types, id);
    return id;
}

uint32_t spv_emit_type_u32(SpvEmitter* e) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, OP_TYPE_INT, 4);
    put(&e->types, id);
    put(&e->types, 32);
    put(&e->types, 0); /* unsigned */
    return id;
}

uint32_t spv_emit_type_f32(SpvEmitter* e) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, OP_TYPE_FLOAT, 3);
    put(&e->types, id);
    put(&e->types, 32);
    return id;
}

uint32_t spv_emit_type_vec4(SpvEmitter* e, uint32_t component_type) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, OP_TYPE_VECTOR, 4);
    put(&e->types, id);
    put(&e->types, component_type);
    put(&e->types, 4);
    return id;
}

uint32_t spv_emit_type_vec2(SpvEmitter* e, uint32_t component_type) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, OP_TYPE_VECTOR, 4);
    put(&e->types, id);
    put(&e->types, component_type);
    put(&e->types, 2);
    return id;
}

uint32_t spv_emit_type_runtime_array(SpvEmitter* e, uint32_t element_type, uint32_t stride) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->decorations, OP_DECORATE, 4);
    put(&e->decorations, id);
    put(&e->decorations, 6); /* ArrayStride */
    put(&e->decorations, stride);
    instruction(&e->types, OP_TYPE_RUNTIME_ARRAY, 3);
    put(&e->types, id);
    put(&e->types, element_type);
    return id;
}

uint32_t spv_emit_type_struct_one(SpvEmitter* e, uint32_t member_type) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->decorations, OP_DECORATE, 3);
    put(&e->decorations, id);
    put(&e->decorations, 2); /* Block */
    instruction(&e->decorations, OP_MEMBER_DECORATE, 5);
    put(&e->decorations, id);
    put(&e->decorations, 0);
    put(&e->decorations, 35); /* Offset */
    put(&e->decorations, 0);
    instruction(&e->types, OP_TYPE_STRUCT, 3);
    put(&e->types, id);
    put(&e->types, member_type);
    return id;
}

static uint32_t type_pointer(SpvEmitter* e, uint32_t storage, uint32_t pointee) {
    for (size_t i = 0; i < e->pointer_count; i++)
        if (e->pointer_types[i].storage == storage && e->pointer_types[i].pointee == pointee)
            return e->pointer_types[i].id;
    if (e->pointer_count == e->pointer_capacity) {
        size_t capacity = e->pointer_capacity ? e->pointer_capacity * 2 : 16;
        SpvPointerType* next = realloc(e->pointer_types, capacity * sizeof *next);
        if (!next) abort();
        e->pointer_types = next;
        e->pointer_capacity = capacity;
    }
    uint32_t id = spv_emit_id(e);
    e->pointer_types[e->pointer_count++] = (SpvPointerType){storage, pointee, id};
    instruction(&e->types, OP_TYPE_POINTER, 4);
    put(&e->types, id);
    put(&e->types, storage);
    put(&e->types, pointee);
    return id;
}

uint32_t spv_emit_type_storage_pointer(SpvEmitter* e, uint32_t pointee_type) {
    return type_pointer(e, 12, pointee_type);
}

uint32_t spv_emit_type_function_pointer(SpvEmitter* e, uint32_t pointee_type) {
    return type_pointer(e, 7, pointee_type);
}

uint32_t spv_emit_type_function_void(SpvEmitter* e, uint32_t void_type) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, OP_TYPE_FUNCTION, 3);
    put(&e->types, id);
    put(&e->types, void_type);
    return id;
}

uint32_t spv_emit_constant_u32(SpvEmitter* e, uint32_t type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, OP_CONSTANT, 4);
    put(&e->types, type);
    put(&e->types, id);
    put(&e->types, value);
    return id;
}

uint32_t spv_emit_constant_bool(SpvEmitter* e, uint32_t type, int value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, value ? OP_CONSTANT_TRUE : OP_CONSTANT_FALSE, 3);
    put(&e->types, type);
    put(&e->types, id);
    return id;
}

uint32_t spv_emit_constant_f32(SpvEmitter* e, uint32_t type, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return spv_emit_constant_u32(e, type, bits);
}

uint32_t spv_emit_constant_vec4(SpvEmitter* e, uint32_t type, const uint32_t components[4]) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, OP_CONSTANT_COMPOSITE, 7);
    put(&e->types, type);
    put(&e->types, id);
    for (size_t i = 0; i < 4; i++) put(&e->types, components[i]);
    return id;
}

uint32_t spv_emit_storage_buffer(SpvEmitter* e, uint32_t struct_type, uint32_t set, uint32_t binding) {
    uint32_t pointer_type = spv_emit_type_storage_pointer(e, struct_type);
    uint32_t id = spv_emit_id(e);
    instruction(&e->decorations, OP_DECORATE, 4);
    put(&e->decorations, id);
    put(&e->decorations, 34); /* DescriptorSet */
    put(&e->decorations, set);
    instruction(&e->decorations, OP_DECORATE, 4);
    put(&e->decorations, id);
    put(&e->decorations, 33); /* Binding */
    put(&e->decorations, binding);
    instruction(&e->types, OP_VARIABLE, 4);
    put(&e->types, pointer_type);
    put(&e->types, id);
    put(&e->types, 12); /* StorageBuffer */
    return id;
}

uint32_t spv_emit_uniform_vec4_array(SpvEmitter* e, uint32_t vec4_type, uint32_t f32_type, uint32_t u32_type,
                                     uint32_t set, uint32_t binding, uint32_t elements,
                                     uint32_t* component_pointer_type) {
    assert(elements && component_pointer_type);
    uint32_t length = spv_emit_constant_u32(e, u32_type, elements);
    uint32_t array = spv_emit_id(e);
    instruction(&e->decorations, OP_DECORATE, 4);
    put(&e->decorations, array);
    put(&e->decorations, 6); /* ArrayStride */
    put(&e->decorations, 16);
    instruction(&e->types, OP_TYPE_ARRAY, 4);
    put(&e->types, array);
    put(&e->types, vec4_type);
    put(&e->types, length);
    uint32_t structure = spv_emit_type_struct_one(e, array);
    uint32_t struct_pointer = type_pointer(e, 2, structure); /* Uniform */
    uint32_t variable = spv_emit_id(e);
    instruction(&e->decorations, OP_DECORATE, 4);
    put(&e->decorations, variable);
    put(&e->decorations, 34); /* DescriptorSet */
    put(&e->decorations, set);
    instruction(&e->decorations, OP_DECORATE, 4);
    put(&e->decorations, variable);
    put(&e->decorations, 33); /* Binding */
    put(&e->decorations, binding);
    instruction(&e->types, OP_VARIABLE, 4);
    put(&e->types, struct_pointer);
    put(&e->types, variable);
    put(&e->types, 2); /* Uniform */
    *component_pointer_type = type_pointer(e, 2, f32_type);
    return variable;
}

uint32_t spv_emit_uniform_component_f32(SpvEmitter* e, uint32_t pointer_type, uint32_t buffer, uint32_t member,
                                        uint32_t element, uint32_t component, uint32_t f32_type) {
    uint32_t pointer = spv_emit_id(e);
    instruction(&e->functions, OP_ACCESS_CHAIN, 7);
    put(&e->functions, pointer_type);
    put(&e->functions, pointer);
    put(&e->functions, buffer);
    put(&e->functions, member);
    put(&e->functions, element);
    put(&e->functions, component);
    return spv_emit_load(e, f32_type, pointer);
}

uint32_t spv_emit_push_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t f32_type, uint32_t offset,
                            uint32_t* component_pointer_type) {
    assert(component_pointer_type && !(offset & 15));
    uint32_t structure = spv_emit_id(e);
    instruction(&e->decorations, OP_DECORATE, 3);
    put(&e->decorations, structure);
    put(&e->decorations, 2); /* Block */
    instruction(&e->decorations, OP_MEMBER_DECORATE, 5);
    put(&e->decorations, structure);
    put(&e->decorations, 0);
    put(&e->decorations, 35); /* Offset */
    put(&e->decorations, offset);
    instruction(&e->types, OP_TYPE_STRUCT, 3);
    put(&e->types, structure);
    put(&e->types, vec4_type);
    uint32_t pointer = type_pointer(e, 9, structure); /* PushConstant */
    uint32_t variable = spv_emit_id(e);
    instruction(&e->types, OP_VARIABLE, 4);
    put(&e->types, pointer);
    put(&e->types, variable);
    put(&e->types, 9);
    *component_pointer_type = type_pointer(e, 9, f32_type);
    return variable;
}

uint32_t spv_emit_push_component_f32(SpvEmitter* e, uint32_t pointer_type, uint32_t block, uint32_t member,
                                     uint32_t component, uint32_t f32_type) {
    uint32_t pointer = spv_emit_id(e);
    instruction(&e->functions, OP_ACCESS_CHAIN, 6);
    put(&e->functions, pointer_type);
    put(&e->functions, pointer);
    put(&e->functions, block);
    put(&e->functions, member);
    put(&e->functions, component);
    return spv_emit_load(e, f32_type, pointer);
}

static uint32_t output_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t decoration, uint32_t value) {
    uint32_t pointer_type = type_pointer(e, 3, vec4_type); /* Output */
    uint32_t id = spv_emit_id(e);
    instruction(&e->decorations, OP_DECORATE, 4);
    put(&e->decorations, id);
    put(&e->decorations, decoration);
    put(&e->decorations, value);
    instruction(&e->types, OP_VARIABLE, 4);
    put(&e->types, pointer_type);
    put(&e->types, id);
    put(&e->types, 3); /* Output */
    return id;
}

uint32_t spv_emit_output_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t location) {
    return output_vec4(e, vec4_type, 30, location); /* Location */
}

uint32_t spv_emit_position_output(SpvEmitter* e, uint32_t vec4_type) {
    uint32_t id = output_vec4(e, vec4_type, 11, 0); /* BuiltIn Position */
    instruction(&e->decorations, OP_DECORATE, 3);
    put(&e->decorations, id);
    put(&e->decorations, 18); /* Invariant */
    return id;
}

uint32_t spv_emit_point_size_output(SpvEmitter* e, uint32_t f32_type) {
    return output_vec4(e, f32_type, 11, 1); /* BuiltIn PointSize */
}

static uint32_t input_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t decoration, uint32_t value) {
    uint32_t pointer_type = type_pointer(e, 1, vec4_type); /* Input */
    uint32_t id = spv_emit_id(e);
    instruction(&e->decorations, OP_DECORATE, 4);
    put(&e->decorations, id);
    put(&e->decorations, decoration);
    put(&e->decorations, value);
    instruction(&e->types, OP_VARIABLE, 4);
    put(&e->types, pointer_type);
    put(&e->types, id);
    put(&e->types, 1); /* Input */
    return id;
}

uint32_t spv_emit_input_vec4(SpvEmitter* e, uint32_t vec4_type, uint32_t location, int interpolation) {
    assert(interpolation >= 0 && interpolation <= 2);
    uint32_t id = input_vec4(e, vec4_type, 30, location); /* Location */
    if (interpolation) {
        instruction(&e->decorations, OP_DECORATE, 3);
        put(&e->decorations, id);
        put(&e->decorations, interpolation == 1 ? 14 : 13); /* Flat / NoPerspective */
    }
    return id;
}

uint32_t spv_emit_fragcoord_input(SpvEmitter* e, uint32_t vec4_type) {
    return input_vec4(e, vec4_type, 11, 15); /* BuiltIn FragCoord */
}

uint32_t spv_emit_sampler2d(SpvEmitter* e, uint32_t f32_type, uint32_t set, uint32_t binding, int shadow,
                            uint32_t* sampled_type) {
    assert(sampled_type && (shadow == 0 || shadow == 1));
    assert(!e->sampled_image_pointee || e->sampled_image_pointee == f32_type);
    e->sampled_image_pointee = f32_type;
    if (!e->sampled_image_types[shadow]) {
        uint32_t image_type = spv_emit_id(e);
        instruction(&e->types, OP_TYPE_IMAGE, 9);
        put(&e->types, image_type);
        put(&e->types, f32_type);
        put(&e->types, 1);              /* Dim2D */
        put(&e->types, shadow ? 1 : 0); /* Depth */
        put(&e->types, 0);              /* Arrayed */
        put(&e->types, 0);              /* MS */
        put(&e->types, 1);              /* Sampled */
        put(&e->types, 0);              /* Unknown format */
        e->image_types[shadow] = image_type;
        e->sampled_image_types[shadow] = spv_emit_id(e);
        instruction(&e->types, OP_TYPE_SAMPLED_IMAGE, 3);
        put(&e->types, e->sampled_image_types[shadow]);
        put(&e->types, image_type);
    }
    *sampled_type = e->sampled_image_types[shadow];
    uint32_t pointer = type_pointer(e, 0, *sampled_type); /* UniformConstant */
    uint32_t sampler = spv_emit_id(e);
    instruction(&e->decorations, OP_DECORATE, 4);
    put(&e->decorations, sampler);
    put(&e->decorations, 34); /* DescriptorSet */
    put(&e->decorations, set);
    instruction(&e->decorations, OP_DECORATE, 4);
    put(&e->decorations, sampler);
    put(&e->decorations, 33); /* Binding */
    put(&e->decorations, binding);
    instruction(&e->types, OP_VARIABLE, 4);
    put(&e->types, pointer);
    put(&e->types, sampler);
    put(&e->types, 0); /* UniformConstant */
    return sampler;
}

void spv_emit_function_void(SpvEmitter* e, uint32_t void_type, uint32_t function, uint32_t function_type) {
    instruction(&e->functions, OP_FUNCTION, 5);
    put(&e->functions, void_type);
    put(&e->functions, function);
    put(&e->functions, 0); /* FunctionControlMaskNone */
    put(&e->functions, function_type);
}

void spv_emit_label(SpvEmitter* e, uint32_t label) {
    instruction(&e->functions, OP_LABEL, 2);
    put(&e->functions, label);
}

uint32_t spv_emit_function_variable(SpvEmitter* e, uint32_t pointer_type) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, OP_VARIABLE, 4);
    put(&e->functions, pointer_type);
    put(&e->functions, id);
    put(&e->functions, 7); /* Function */
    return id;
}

uint32_t spv_emit_function_variable_init(SpvEmitter* e, uint32_t pointer_type, uint32_t initial_value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, OP_VARIABLE, 5);
    put(&e->functions, pointer_type);
    put(&e->functions, id);
    put(&e->functions, 7); /* Function */
    put(&e->functions, initial_value);
    return id;
}

uint32_t spv_emit_load(SpvEmitter* e, uint32_t result_type, uint32_t pointer) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, OP_LOAD, 4);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, pointer);
    return id;
}

uint32_t spv_emit_bitcast(SpvEmitter* e, uint32_t result_type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, OP_BITCAST, 4);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, value);
    return id;
}

static uint32_t binary(SpvEmitter* e, uint32_t opcode, uint32_t result_type, uint32_t left, uint32_t right) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, opcode, 5);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, left);
    put(&e->functions, right);
    return id;
}

uint32_t spv_emit_fadd(SpvEmitter* e, uint32_t f32_type, uint32_t left, uint32_t right) {
    return binary(e, OP_FADD, f32_type, left, right);
}

uint32_t spv_emit_fmul(SpvEmitter* e, uint32_t f32_type, uint32_t left, uint32_t right) {
    return binary(e, OP_FMUL, f32_type, left, right);
}

uint32_t spv_emit_i_equal(SpvEmitter* e, uint32_t bool_type, uint32_t left, uint32_t right) {
    return binary(e, OP_I_EQUAL, bool_type, left, right);
}

uint32_t spv_emit_fcompare(SpvEmitter* e, uint32_t bool_type, uint32_t condition, uint32_t left, uint32_t right) {
    static const uint32_t opcode[16] = {0, 184, 180, 188, 186, 182, 190, 162, 163, 185, 181, 189, 187, 183, 191, 0};
    assert(condition < 16);
    if (condition == 0 || condition == 15) return spv_emit_constant_bool(e, bool_type, condition == 15);
    return binary(e, opcode[condition], bool_type, left, right);
}

uint32_t spv_emit_bool_not(SpvEmitter* e, uint32_t bool_type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, OP_LOGICAL_NOT, 4);
    put(&e->functions, bool_type);
    put(&e->functions, id);
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_bool_binary(SpvEmitter* e, uint32_t bool_type, uint32_t op, uint32_t left, uint32_t right) {
    static const uint32_t opcode[3] = {OP_LOGICAL_AND, OP_LOGICAL_OR, OP_LOGICAL_NOT_EQUAL};
    assert(op < 3);
    return binary(e, opcode[op], bool_type, left, right);
}

uint32_t spv_emit_fdiv(SpvEmitter* e, uint32_t f32_type, uint32_t left, uint32_t right) {
    return binary(e, OP_FDIV, f32_type, left, right);
}

uint32_t spv_emit_fnegate(SpvEmitter* e, uint32_t f32_type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, OP_FNEGATE, 4);
    put(&e->functions, f32_type);
    put(&e->functions, id);
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_glsl_unary(SpvEmitter* e, uint32_t f32_type, uint32_t import, SpvGlsl450Op op, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, OP_EXT_INST, 6);
    put(&e->functions, f32_type);
    put(&e->functions, id);
    put(&e->functions, import);
    put(&e->functions, (uint32_t)op);
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_glsl_binary(SpvEmitter* e, uint32_t f32_type, uint32_t import, SpvGlsl450Op op, uint32_t left,
                              uint32_t right) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, OP_EXT_INST, 7);
    put(&e->functions, f32_type);
    put(&e->functions, id);
    put(&e->functions, import);
    put(&e->functions, (uint32_t)op);
    put(&e->functions, left);
    put(&e->functions, right);
    return id;
}

uint32_t spv_emit_isnan(SpvEmitter* e, uint32_t bool_type, uint32_t value) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, OP_IS_NAN, 4);
    put(&e->functions, bool_type);
    put(&e->functions, id);
    put(&e->functions, value);
    return id;
}

uint32_t spv_emit_select(SpvEmitter* e, uint32_t result_type, uint32_t condition, uint32_t yes, uint32_t no) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, OP_SELECT, 6);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, condition);
    put(&e->functions, yes);
    put(&e->functions, no);
    return id;
}

uint32_t spv_emit_glsl_ternary(SpvEmitter* e, uint32_t f32_type, uint32_t import, SpvGlsl450Op op, uint32_t a,
                               uint32_t b, uint32_t c) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, OP_EXT_INST, 8);
    put(&e->functions, f32_type);
    put(&e->functions, id);
    put(&e->functions, import);
    put(&e->functions, (uint32_t)op);
    put(&e->functions, a);
    put(&e->functions, b);
    put(&e->functions, c);
    return id;
}

uint32_t spv_emit_vec4_extract(SpvEmitter* e, uint32_t component_type, uint32_t vector, uint32_t component) {
    assert(component < 4);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, OP_COMPOSITE_EXTRACT, 5);
    put(&e->functions, component_type);
    put(&e->functions, id);
    put(&e->functions, vector);
    put(&e->functions, component);
    return id;
}

uint32_t spv_emit_vec4_insert(SpvEmitter* e, uint32_t vec4_type, uint32_t vector, uint32_t component_value,
                              uint32_t component) {
    assert(component < 4);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, OP_COMPOSITE_INSERT, 6);
    put(&e->functions, vec4_type);
    put(&e->functions, id);
    put(&e->functions, component_value);
    put(&e->functions, vector);
    put(&e->functions, component);
    return id;
}

uint32_t spv_emit_vec4_construct(SpvEmitter* e, uint32_t vec4_type, const uint32_t components[4]) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, OP_COMPOSITE_CONSTRUCT, 7);
    put(&e->functions, vec4_type);
    put(&e->functions, id);
    for (size_t i = 0; i < 4; i++) put(&e->functions, components[i]);
    return id;
}

uint32_t spv_emit_vec2_construct(SpvEmitter* e, uint32_t vec2_type, uint32_t x, uint32_t y) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, OP_COMPOSITE_CONSTRUCT, 5);
    put(&e->functions, vec2_type);
    put(&e->functions, id);
    put(&e->functions, x);
    put(&e->functions, y);
    return id;
}

uint32_t spv_emit_sample2d(SpvEmitter* e, uint32_t result_type, uint32_t sampled_type, uint32_t sampler,
                           uint32_t coordinates, uint32_t depth_reference) {
    uint32_t image = spv_emit_load(e, sampled_type, sampler);
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, depth_reference ? OP_IMAGE_SAMPLE_DREF_IMPLICIT_LOD : OP_IMAGE_SAMPLE_IMPLICIT_LOD,
                depth_reference ? 6 : 5);
    put(&e->functions, result_type);
    put(&e->functions, id);
    put(&e->functions, image);
    put(&e->functions, coordinates);
    if (depth_reference) put(&e->functions, depth_reference);
    return id;
}

void spv_emit_no_contraction(SpvEmitter* e, uint32_t result) {
    instruction(&e->decorations, OP_DECORATE, 3);
    put(&e->decorations, result);
    put(&e->decorations, 42); /* NoContraction */
}

uint32_t spv_emit_storage_access_u32(SpvEmitter* e, uint32_t pointer_type, uint32_t buffer, uint32_t member_index,
                                     uint32_t element_index) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, OP_ACCESS_CHAIN, 6);
    put(&e->functions, pointer_type);
    put(&e->functions, id);
    put(&e->functions, buffer);
    put(&e->functions, member_index);
    put(&e->functions, element_index);
    return id;
}

uint32_t spv_emit_iadd_u32(SpvEmitter* e, uint32_t u32_type, uint32_t left, uint32_t right) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->functions, OP_IADD, 5);
    put(&e->functions, u32_type);
    put(&e->functions, id);
    put(&e->functions, left);
    put(&e->functions, right);
    return id;
}

void spv_emit_store(SpvEmitter* e, uint32_t pointer, uint32_t value) {
    instruction(&e->functions, OP_STORE, 3);
    put(&e->functions, pointer);
    put(&e->functions, value);
}

void spv_emit_store_u32(SpvEmitter* e, uint32_t pointer, uint32_t value) { spv_emit_store(e, pointer, value); }

void spv_emit_return(SpvEmitter* e) { instruction(&e->functions, OP_RETURN, 1); }
void spv_emit_kill(SpvEmitter* e) { instruction(&e->functions, OP_KILL, 1); }
void spv_emit_selection_merge(SpvEmitter* e, uint32_t merge_label) {
    instruction(&e->functions, OP_SELECTION_MERGE, 3);
    put(&e->functions, merge_label);
    put(&e->functions, 0); /* SelectionControlMaskNone */
}

void spv_emit_branch_conditional(SpvEmitter* e, uint32_t condition, uint32_t yes_label, uint32_t no_label) {
    instruction(&e->functions, OP_BRANCH_CONDITIONAL, 4);
    put(&e->functions, condition);
    put(&e->functions, yes_label);
    put(&e->functions, no_label);
}

void spv_emit_branch(SpvEmitter* e, uint32_t label) {
    instruction(&e->functions, OP_BRANCH, 2);
    put(&e->functions, label);
}

void spv_emit_loop_merge(SpvEmitter* e, uint32_t merge_label, uint32_t continue_label) {
    instruction(&e->functions, OP_LOOP_MERGE, 4);
    put(&e->functions, merge_label);
    put(&e->functions, continue_label);
    put(&e->functions, 0); /* LoopControlMaskNone */
}

void spv_emit_switch_u32(SpvEmitter* e, uint32_t selector, uint32_t default_label, const uint32_t* literals,
                         const uint32_t* labels, size_t count) {
    assert(count <= (UINT16_MAX - 3) / 2);
    instruction(&e->functions, OP_SWITCH, (uint32_t)(3 + count * 2));
    put(&e->functions, selector);
    put(&e->functions, default_label);
    for (size_t i = 0; i < count; i++) {
        put(&e->functions, literals[i]);
        put(&e->functions, labels[i]);
    }
}
void spv_emit_function_end(SpvEmitter* e) { instruction(&e->functions, OP_FUNCTION_END, 1); }

uint32_t* spv_emit_finish(const SpvEmitter* e, size_t* word_count) {
    const SpvWords* sections[] = {&e->capabilities, &e->extensions,  &e->imports, &e->entry_points, &e->execution_modes,
                                  &e->debug,        &e->decorations, &e->types,   &e->functions};
    size_t n = 8; /* header, memory model */
    for (size_t i = 0; i < sizeof sections / sizeof sections[0]; i++) {
        assert(sections[i]->count <= SIZE_MAX - n);
        n += sections[i]->count;
    }
    assert(n <= SIZE_MAX / sizeof(uint32_t));
    uint32_t* out = malloc(n * sizeof(uint32_t));
    if (!out) abort();
    size_t p = 0;
    out[p++] = 0x07230203; /* magic */
    out[p++] = 0x00010600; /* SPIR-V 1.6 */
    out[p++] = 0;          /* generator */
    out[p++] = e->next_id;
    out[p++] = 0; /* schema */
    if (e->capabilities.count) memcpy(out + p, e->capabilities.words, e->capabilities.count * sizeof(uint32_t));
    p += e->capabilities.count;
    if (e->extensions.count) memcpy(out + p, e->extensions.words, e->extensions.count * sizeof(uint32_t));
    p += e->extensions.count;
    if (e->imports.count) memcpy(out + p, e->imports.words, e->imports.count * sizeof(uint32_t));
    p += e->imports.count;
    out[p++] = (3u << 16) | OP_MEMORY_MODEL;
    out[p++] = 0; /* Logical */
    out[p++] = 1; /* GLSL450 */
    for (size_t i = 3; i < sizeof sections / sizeof sections[0]; i++) {
        if (sections[i]->count) memcpy(out + p, sections[i]->words, sections[i]->count * sizeof(uint32_t));
        p += sections[i]->count;
    }
    assert(p == n);
    *word_count = n;
    return out;
}
