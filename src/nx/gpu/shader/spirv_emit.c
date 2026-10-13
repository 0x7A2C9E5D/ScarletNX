/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/shader/spirv_emit.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

enum {
    OP_NAME = 5,
    OP_MEMORY_MODEL = 14,
    OP_ENTRY_POINT = 15,
    OP_EXECUTION_MODE = 16,
    OP_CAPABILITY = 17,
    OP_TYPE_VOID = 19,
    OP_TYPE_BOOL = 20,
    OP_TYPE_INT = 21,
    OP_TYPE_FLOAT = 22,
    OP_TYPE_VECTOR = 23,
    OP_TYPE_RUNTIME_ARRAY = 29,
    OP_TYPE_STRUCT = 30,
    OP_TYPE_POINTER = 32,
    OP_TYPE_FUNCTION = 33,
    OP_CONSTANT = 43,
    OP_CONSTANT_COMPOSITE = 44,
    OP_FUNCTION = 54,
    OP_FUNCTION_END = 56,
    OP_VARIABLE = 59,
    OP_STORE = 62,
    OP_ACCESS_CHAIN = 65,
    OP_DECORATE = 71,
    OP_MEMBER_DECORATE = 72,
    OP_IADD = 128,
    OP_LABEL = 248,
    OP_RETURN = 253,
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
    SpvWords* sections[] = {&e->capabilities, &e->entry_points, &e->execution_modes, &e->debug,
                            &e->decorations,  &e->types,        &e->functions};
    for (size_t i = 0; i < sizeof sections / sizeof sections[0]; i++) free(sections[i]->words);
    memset(e, 0, sizeof *e);
}

uint32_t spv_emit_id(SpvEmitter* e) {
    assert(e->next_id && e->next_id < UINT32_MAX);
    return e->next_id++;
}

void spv_emit_shader_capability(SpvEmitter* e) {
    instruction(&e->capabilities, OP_CAPABILITY, 2);
    put(&e->capabilities, 1); /* Shader */
}

void spv_emit_compute_entry_str(SpvEmitter* e, uint32_t function, const char* name, const uint32_t* interfaces,
                                size_t interface_count) {
    size_t n = string_words(name);
    assert(n <= UINT16_MAX - 3 && interface_count <= UINT16_MAX - 3 - n);
    instruction(&e->entry_points, OP_ENTRY_POINT, (uint32_t)(3 + n + interface_count));
    put(&e->entry_points, 5); /* GLCompute */
    put(&e->entry_points, function);
    put_str(&e->entry_points, name);
    for (size_t i = 0; i < interface_count; i++) put(&e->entry_points, interfaces[i]);
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

uint32_t spv_emit_type_storage_pointer(SpvEmitter* e, uint32_t pointee_type) {
    uint32_t id = spv_emit_id(e);
    instruction(&e->types, OP_TYPE_POINTER, 4);
    put(&e->types, id);
    put(&e->types, 12); /* StorageBuffer */
    put(&e->types, pointee_type);
    return id;
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

void spv_emit_store_u32(SpvEmitter* e, uint32_t pointer, uint32_t value) {
    instruction(&e->functions, OP_STORE, 3);
    put(&e->functions, pointer);
    put(&e->functions, value);
}

void spv_emit_return(SpvEmitter* e) { instruction(&e->functions, OP_RETURN, 1); }
void spv_emit_function_end(SpvEmitter* e) { instruction(&e->functions, OP_FUNCTION_END, 1); }

uint32_t* spv_emit_finish(const SpvEmitter* e, size_t* word_count) {
    const SpvWords* sections[] = {&e->capabilities, &e->entry_points, &e->execution_modes, &e->debug,
                                  &e->decorations,  &e->types,        &e->functions};
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
    out[p++] = (3u << 16) | OP_MEMORY_MODEL;
    out[p++] = 0; /* Logical */
    out[p++] = 1; /* GLSL450 */
    for (size_t i = 1; i < sizeof sections / sizeof sections[0]; i++) {
        if (sections[i]->count) memcpy(out + p, sections[i]->words, sections[i]->count * sizeof(uint32_t));
        p += sections[i]->count;
    }
    assert(p == n);
    *word_count = n;
    return out;
}
