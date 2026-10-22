/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The small system libraries: ScePower (a charged battery on mains power), SceAppUtil, SceSysmodule (every module is
   there: the runtime answers their functions), SceRtc, SceCommonDialog, SceAppMgr, and the network, which the runtime
   doesn't give a game yet (SceNet and SceNetCtl answer that there is none) */
#include "vita/hle/hle.h"
#include <string.h>
#include <time.h>

#define SCE_NET_ERROR_ENOTCONN    0x80410139u /* 0x80410100 | ENOTCONN, sceNet's errno form */
#define SCE_NET_CTL_ERROR_NOT_CONNECTED 0x80412108u
#define SCE_FONT_ERROR_HANDLER_OPEN_FAILED 0x80460005u
#define SCE_RTC_ERROR_INVALID_POINTER 0x80251001u

static uint32_t zero(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}
static uint32_t one(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 1;
}
static uint32_t battery_percent(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 100;
}
static uint32_t battery_life_time(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 300; /* minutes */
}
static uint32_t arm_clock(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 444;
}
/* sceAppUtilSystemParamGetInt(id, *value): the system's language (1, English), enter button (1, cross), date and
   time formats, the time zone */
static uint32_t system_param_int(VitaProcess* p, VitaThread* t) {
    uint32_t id = vita_arg(p, t, 0), out = vita_arg(p, t, 1), value = 0;
    switch (id) {
        case 1: value = 1; break; /* SCE_SYSTEM_PARAM_ID_LANG: English (United States) */
        case 2: value = 1; break; /* SCE_SYSTEM_PARAM_ID_ENTER_BUTTON: cross */
        default: value = 0; break;
    }
    if (out) vita_write32(p, out, value);
    return 0;
}
static uint32_t system_param_string(VitaProcess* p, VitaThread* t) {
    uint32_t out = vita_arg(p, t, 1), size = vita_arg(p, t, 2);
    static const char name[] = "Player";
    if (out && size) {
        size_t n = size < sizeof name ? size - 1 : sizeof name - 1;
        memcpy(vita_ptr(p, out), name, n);
        ((char*)vita_ptr(p, out))[n] = 0;
    }
    return 0;
}
/* sceRtcGetTime_t(const SceDateTime*, time_t*): the date (year, month, day, hour, minute, second as 16 bits, then
   microseconds) as the seconds since 1970, a 32-bit time_t as SceRtc has it */
static uint32_t rtc_get_time_t(VitaProcess* p, VitaThread* t) {
    uint32_t date = vita_arg(p, t, 0), out = vita_arg(p, t, 1);
    if (!date || !out) return SCE_RTC_ERROR_INVALID_POINTER;
    const unsigned char* b = vita_ptr(p, date);
    int64_t f[6];
    for (unsigned i = 0; i < 6; ++i) f[i] = b[i * 2] | b[i * 2 + 1] << 8;
    /* days from the civil date (Howard Hinnant's days_from_civil) */
    int64_t y = f[0] - (f[1] <= 2), era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    int64_t doy = (153 * (f[1] + (f[1] > 2 ? -3 : 9)) + 2) / 5 + f[2] - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    vita_write32(p, out, (uint32_t)(days * 86400 + f[3] * 3600 + f[4] * 60 + f[5]));
    return 0;
}
/* SceDateTime of now, local time */
static uint32_t rtc_current_local(VitaProcess* p, VitaThread* t) {
    uint32_t out = vita_arg(p, t, 0);
    if (!out) return SCE_RTC_ERROR_INVALID_POINTER;
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    time_t seconds = now.tv_sec;
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &seconds);
#else
    localtime_r(&seconds, &tm);
#endif
    uint16_t f[6] = {(uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_mday,
                     (uint16_t)tm.tm_hour,          (uint16_t)tm.tm_min,       (uint16_t)tm.tm_sec};
    unsigned char* b = vita_ptr(p, out);
    for (unsigned i = 0; i < 6; ++i) b[i * 2] = (unsigned char)f[i], b[i * 2 + 1] = (unsigned char)(f[i] >> 8);
    vita_write32(p, out + 12, (uint32_t)(now.tv_nsec / 1000));
    return 0;
}
/* sceAppMgrLoadExec: another program in the game's place, which the runtime can't start: the game ends */
static uint32_t load_exec(VitaProcess* p, VitaThread* t) {
    (void)t;
    p->exited = true;
    return 0;
}
static uint32_t net_not_connected(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return SCE_NET_ERROR_ENOTCONN;
}
static uint32_t net_ctl_not_connected(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return SCE_NET_CTL_ERROR_NOT_CONNECTED;
}
static uint32_t net_htonl(VitaProcess* p, VitaThread* t) {
    uint32_t v = vita_arg(p, t, 0);
    return (v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24);
}
static uint32_t net_htons(VitaProcess* p, VitaThread* t) {
    uint32_t v = vita_arg(p, t, 0) & 0xffffu;
    return ((v >> 8) | (v << 8)) & 0xffffu;
}
/* ScePgf: the console's system fonts, which the runtime doesn't have: sceFontNewLib answers no library and its error */
static uint32_t font_new_lib(VitaProcess* p, VitaThread* t) {
    uint32_t error = vita_arg(p, t, 1);
    if (error) vita_write32(p, error, SCE_FONT_ERROR_HANDLER_OPEN_FAILED);
    return 0;
}
static uint32_t font_fail(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return SCE_FONT_ERROR_HANDLER_OPEN_FAILED;
}

const VitaHle vita_hle_misc[] = {
    {0x2085d15du, "scePowerGetBatteryLifePercent", battery_percent},
    {0x8efb3fa2u, "scePowerGetBatteryLifeTime", battery_life_time},
    {0x1e490401u, "scePowerIsBatteryCharging", one},
    {0x87440f5eu, "scePowerIsPowerOnline", one},
    {0x74db5ae5u, "scePowerSetArmClockFrequency", zero},
    {0xabc6f88fu, "scePowerGetArmClockFrequency", arm_clock},
    {0xdaffe671u, "sceAppUtilInit", zero},
    {0xb220b00bu, "sceAppUtilShutdown", zero},
    {0xee85804du, "sceAppUtilPhotoMount", zero},
    {0x9651b941u, "sceAppUtilPhotoUmount", zero},
    {0xb5edcbffu, "sceAppUtilMusicMount", zero},
    {0xa7fe1bf7u, "sceAppUtilMusicUmount", zero},
    {0x5dfb9ca0u, "sceAppUtilSystemParamGetInt", system_param_int},
    {0x6e6aa267u, "sceAppUtilSystemParamGetString", system_param_string},
    {0x79a0160au, "sceSysmoduleLoadModule", zero},
    {0x31d87805u, "sceSysmoduleUnloadModule", zero},
    {0x53099b7au, "sceSysmoduleIsLoaded", zero},
    {0x8de6feb7u, "sceRtcGetTime_t", rtc_get_time_t},
    {0x0572eddcu, "sceRtcGetCurrentClockLocalTime", rtc_current_local},
    {0x90530f2fu, "sceCommonDialogUpdate", zero},
    {0xe6774abcu, "sceAppMgrLoadExec", load_exec},
    {0xeb03e265u, "sceNetInit", zero},
    {0xea3cc286u, "sceNetTerm", zero},
    {0xf084fce3u, "sceNetSocket", net_not_connected},
    {0x29822b4du, "sceNetSocketClose", net_not_connected},
    {0x891c1b9bu, "sceNetSocketAbort", net_not_connected},
    {0x1296a94bu, "sceNetBind", net_not_connected},
    {0x7a8da094u, "sceNetListen", net_not_connected},
    {0x1adf9bb1u, "sceNetAccept", net_not_connected},
    {0x11e5b6f6u, "sceNetConnect", net_not_connected},
    {0xe3dd8cd9u, "sceNetSend", net_not_connected},
    {0x023643b7u, "sceNetRecv", net_not_connected},
    {0x1c66a6dbu, "sceNetGetsockname", net_not_connected},
    {0x338edc2eu, "sceNetShowNetstat", zero},
    {0x4c30b03cu, "sceNetHtonl", net_htonl},
    {0x9fa3207bu, "sceNetHtons", net_htons},
    {0xd5eeb048u, "sceNetInetPton", zero}, /* 0: not an address */
    {0x98839b74u, "sceNetInetNtop", zero}, /* NULL: no text */
    {0x495ca1dbu, "sceNetCtlInit", zero},
    {0xcd188648u, "sceNetCtlTerm", zero},
    {0xb26d07f3u, "sceNetCtlInetGetInfo", net_ctl_not_connected},
    {0x1055aba3u, "sceFontNewLib", font_new_lib},
    {0x07ee1733u, "sceFontDoneLib", zero},
    {0xbd2dfcffu, "sceFontOpen", font_new_lib},
    {0x4a7293e9u, "sceFontClose", zero},
    {0x6fd1ba65u, "sceFontGetCharInfo", font_fail},
    {0xab45aad3u, "sceFontGetCharGlyphImage", font_fail},
    {0, NULL, NULL},
};
