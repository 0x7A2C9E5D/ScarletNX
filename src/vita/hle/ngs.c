/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SceNgs, the console's voice synthesizer, as a silent one: the system, its racks and their voices live in the memory
   the game gives them, a voice's handle the address of its part of its rack's buffer, its parameters the rest of
   that part. Nothing is mixed yet: a voice that plays has finished at once (available, no update passed). As the
   vitasdk headers (MIT) declare */
#include "vita/hle/hle.h"
#include <string.h>

#define SCE_NGS_ERROR_INVALID_PARAM 0x804a0002u
#define RACK_HEADER 0x40u  /* the rack's voice count, then its voices */
#define VOICE_SIZE  0x400u /* a voice's handle word, then its parameters */
#define VOICE_PARAMS 0x40u

static uint32_t arg(VitaProcess* p, VitaThread* t, unsigned n) { return vita_arg(p, t, n); }
static uint32_t ok(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}
/* sceNgsSystemGetRequiredMemorySize(params, size*) */
static uint32_t system_get_required_memory_size(VitaProcess* p, VitaThread* t) {
    if (!arg(p, t, 1)) return SCE_NGS_ERROR_INVALID_PARAM;
    vita_write32(p, arg(p, t, 1), 0x1000);
    return 0;
}
/* sceNgsSystemInit(memory, size, params, handle*): the memory is the handle */
static uint32_t system_init(VitaProcess* p, VitaThread* t) {
    if (!arg(p, t, 0) || !arg(p, t, 3)) return SCE_NGS_ERROR_INVALID_PARAM;
    vita_write32(p, arg(p, t, 3), arg(p, t, 0));
    return 0;
}
/* sceNgsRackGetRequiredMemorySize(system, description, size*): room for each voice the description names */
static uint32_t rack_get_required_memory_size(VitaProcess* p, VitaThread* t) {
    uint32_t description = arg(p, t, 1), out = arg(p, t, 2);
    if (!description || !out) return SCE_NGS_ERROR_INVALID_PARAM;
    int32_t voices = (int32_t)vita_read32(p, description + 4);
    if (voices <= 0 || voices > 0x10000) return SCE_NGS_ERROR_INVALID_PARAM;
    vita_write32(p, out, RACK_HEADER + (uint32_t)voices * VOICE_SIZE);
    return 0;
}
/* sceNgsRackInit(system, SceNgsBufferInfo*, description, rack*) */
static uint32_t rack_init(VitaProcess* p, VitaThread* t) {
    uint32_t buffer = arg(p, t, 1), description = arg(p, t, 2), out = arg(p, t, 3);
    if (!buffer || !description || !out) return SCE_NGS_ERROR_INVALID_PARAM;
    uint32_t rack = vita_read32(p, buffer), size = vita_read32(p, buffer + 4);
    uint32_t voices = vita_read32(p, description + 4);
    if (!rack || voices > 0x10000 || size < RACK_HEADER + voices * VOICE_SIZE || !vita_mapped(p, rack, size))
        return SCE_NGS_ERROR_INVALID_PARAM;
    memset(vita_ptr(p, rack), 0, RACK_HEADER + voices * VOICE_SIZE);
    vita_write32(p, rack, voices);
    vita_write32(p, out, rack);
    return 0;
}
/* sceNgsRackGetVoiceHandle(rack, index, voice*) */
static uint32_t rack_get_voice_handle(VitaProcess* p, VitaThread* t) {
    uint32_t rack = arg(p, t, 0), index = arg(p, t, 1), out = arg(p, t, 2);
    if (!rack || !out || index >= vita_read32(p, rack)) return SCE_NGS_ERROR_INVALID_PARAM;
    vita_write32(p, out, rack + RACK_HEADER + index * VOICE_SIZE);
    return 0;
}
/* the voice definitions sceNgsVoiceDefGet* return: immutable, which the game only hands back, at the runtime's page */
static uint32_t voice_definition(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return VITA_RUNTIME_PAGE + 0x800u;
}
/* sceNgsVoiceLockParams(voice, module, interface, SceNgsBufferInfo*): the voice's parameter room */
static uint32_t voice_lock_params(VitaProcess* p, VitaThread* t) {
    uint32_t voice = arg(p, t, 0), out = arg(p, t, 3);
    if (!voice || !out) return SCE_NGS_ERROR_INVALID_PARAM;
    vita_write32(p, out, voice + VOICE_PARAMS);
    vita_write32(p, out + 4, VOICE_SIZE - VOICE_PARAMS);
    return 0;
}
/* sceNgsVoiceGetStateData(voice, module, memory, size): a module that has done nothing */
static uint32_t voice_get_state_data(VitaProcess* p, VitaThread* t) {
    uint32_t memory = arg(p, t, 2), size = arg(p, t, 3);
    if (!memory || !vita_mapped(p, memory, size)) return SCE_NGS_ERROR_INVALID_PARAM;
    memset(vita_ptr(p, memory), 0, size);
    return 0;
}
/* sceNgsVoiceGetInfo(voice, SceNgsVoiceInfo*): available */
static uint32_t voice_get_info(VitaProcess* p, VitaThread* t) {
    uint32_t out = arg(p, t, 1);
    if (!arg(p, t, 0) || !out) return SCE_NGS_ERROR_INVALID_PARAM;
    memset(vita_ptr(p, out), 0, 0x18);
    return 0;
}
/* sceNgsPatchCreateRouting(SceNgsPatchSetupInfo*, patch*): a handle naming the source voice's output */
static uint32_t patch_create_routing(VitaProcess* p, VitaThread* t) {
    uint32_t info = arg(p, t, 0), out = arg(p, t, 1);
    if (!info || !out) return SCE_NGS_ERROR_INVALID_PARAM;
    vita_write32(p, out, vita_read32(p, info) + 4 + (vita_read32(p, info + 4) & 7) * 4);
    return 0;
}
/* sceNgsVoiceSetParamsBlock(voice, header, size, error count*) */
static uint32_t voice_set_params_block(VitaProcess* p, VitaThread* t) {
    if (arg(p, t, 3)) vita_write32(p, arg(p, t, 3), 0);
    return 0;
}

const VitaHle vita_hle_ngs[] = {
    {0x6ce8b36fu, "sceNgsSystemGetRequiredMemorySize", system_get_required_memory_size},
    {0xed14cf4au, "sceNgsSystemInit", system_init},
    {0x4a25bebcu, "sceNgsSystemRelease", ok},
    {0x64d80013u, "sceNgsSystemSetFlags", ok},
    {0x684f080cu, "sceNgsSystemUpdate", ok},
    {0x477318c0u, "sceNgsRackGetRequiredMemorySize", rack_get_required_memory_size},
    {0x0a92e4ecu, "sceNgsRackInit", rack_init},
    {0xfe1a98e9u, "sceNgsRackGetVoiceHandle", rack_get_voice_handle},
    {0xdd5ca10bu, "sceNgsRackRelease", ok},
    {0x0d5399cfu, "sceNgsVoiceDefGetSimpleVoice", voice_definition},
    {0x79a121d1u, "sceNgsVoiceDefGetMasterBuss", voice_definition},
    {0x9dcf50f5u, "sceNgsVoiceDefGetReverbBuss", voice_definition},
    {0xab6bef8fu, "sceNgsVoiceLockParams", voice_lock_params},
    {0x3d46d8a7u, "sceNgsVoiceUnlockParams", ok},
    {0xfb8174b1u, "sceNgsVoiceSetParamsBlock", voice_set_params_block},
    {0xc9b8c0b4u, "sceNgsVoiceGetStateData", voice_get_state_data},
    {0x5551410du, "sceNgsVoiceGetInfo", voice_get_info},
    {0xfa0a0f34u, "sceNgsVoicePlay", ok},
    {0xd7786e99u, "sceNgsVoicePause", ok},
    {0x54cfb981u, "sceNgsVoiceResume", ok},
    {0x0e291aadu, "sceNgsVoiceKill", ok},
    {0x9ab87e71u, "sceNgsVoiceBypassModule", ok},
    {0xd668b49cu, "sceNgsPatchCreateRouting", patch_create_routing},
    {0xd0c9ae5au, "sceNgsPatchRemoveRouting", ok},
    {0xa0f5402du, "sceNgsVoicePatchSetVolumesMatrix", ok},
    {0, NULL, NULL},
};
