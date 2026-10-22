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
/* days since 1970 of a civil date (Howard Hinnant's days_from_civil) */
static int64_t days_from_civil(int64_t year, int64_t month, int64_t day) {
    int64_t y = year - (month <= 2), era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
/* a SceDateTime's seconds since 1970 */
static int64_t date_seconds(VitaProcess* p, uint32_t date) {
    const unsigned char* b = vita_ptr(p, date);
    int64_t f[6];
    for (unsigned i = 0; i < 6; ++i) f[i] = b[i * 2] | b[i * 2 + 1] << 8;
    return days_from_civil(f[0], f[1], f[2]) * 86400 + f[3] * 3600 + f[4] * 60 + f[5];
}
static uint32_t rtc_get_time_t(VitaProcess* p, VitaThread* t) {
    uint32_t date = vita_arg(p, t, 0), out = vita_arg(p, t, 1);
    if (!date || !out) return SCE_RTC_ERROR_INVALID_POINTER;
    vita_write32(p, out, (uint32_t)date_seconds(p, date));
    return 0;
}
static uint32_t rtc_get_time64_t(VitaProcess* p, VitaThread* t) {
    uint32_t date = vita_arg(p, t, 0), out = vita_arg(p, t, 1);
    if (!date || !out) return SCE_RTC_ERROR_INVALID_POINTER;
    vita_write64(p, out, (uint64_t)date_seconds(p, date));
    return 0;
}
/* SceRtcTick: microseconds since 0001-01-01 */
#define UNIX_EPOCH_TICK 62135596800000000ull
static uint32_t rtc_get_tick(VitaProcess* p, VitaThread* t) {
    uint32_t date = vita_arg(p, t, 0), out = vita_arg(p, t, 1);
    if (!date || !out) return SCE_RTC_ERROR_INVALID_POINTER;
    vita_write64(p, out, UNIX_EPOCH_TICK + (uint64_t)(date_seconds(p, date) * 1000000) + vita_read32(p, date + 12));
    return 0;
}
static uint32_t rtc_get_current_tick(VitaProcess* p, VitaThread* t) {
    uint32_t out = vita_arg(p, t, 0);
    if (!out) return SCE_RTC_ERROR_INVALID_POINTER;
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    vita_write64(p, out, UNIX_EPOCH_TICK + (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u);
    return 0;
}
static uint32_t rtc_get_tick_resolution(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 1000000;
}
/* sceRtcGetDayOfWeek(year, month, day): 0 for Sunday */
static uint32_t rtc_get_day_of_week(VitaProcess* p, VitaThread* t) {
    int64_t days = days_from_civil(vita_arg(p, t, 0), vita_arg(p, t, 1), vita_arg(p, t, 2));
    return (uint32_t)(((days + 4) % 7 + 7) % 7); /* 1970-01-01 was a Thursday */
}
/* sceRtcSetTime_t(SceDateTime*, time_t): the date of a 32-bit time_t, UTC */
static uint32_t rtc_set_time_t(VitaProcess* p, VitaThread* t) {
    uint32_t out = vita_arg(p, t, 0);
    if (!out) return SCE_RTC_ERROR_INVALID_POINTER;
    time_t seconds = (time_t)(int32_t)vita_arg(p, t, 1);
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &seconds);
#else
    gmtime_r(&seconds, &tm);
#endif
    uint16_t f[6] = {(uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_mday,
                     (uint16_t)tm.tm_hour,          (uint16_t)tm.tm_min,       (uint16_t)tm.tm_sec};
    unsigned char* b = vita_ptr(p, out);
    for (unsigned i = 0; i < 6; ++i) b[i * 2] = (unsigned char)f[i], b[i * 2 + 1] = (unsigned char)(f[i] >> 8);
    vita_write32(p, out + 12, 0);
    return 0;
}
/* sceRtcGetAccumulativeTime(): microseconds since the process started, 64 bits */
static uint32_t rtc_get_accumulative_time(VitaProcess* p, VitaThread* t) {
    return vita_return64(t, (vita_now_ns() - p->start_ns) / 1000u);
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
/* sceMotionGetState(SceMotionState*): a console at rest, level and face up: gravity alone on the accelerometer, the
   identity orientation */
static void put_float(VitaProcess* p, uint32_t address, float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    vita_write32(p, address, bits);
}
static uint32_t motion_get_state(VitaProcess* p, VitaThread* t) {
    uint32_t out = vita_arg(p, t, 0);
    if (!out) return 0x80360200u; /* SCE_MOTION_ERROR_NULL_PARAMETER */
    uint64_t us = (vita_now_ns() - p->start_ns) / 1000u;
    memset(vita_ptr(p, out), 0, 0xf8);
    vita_write32(p, out, (uint32_t)us);
    put_float(p, out + 12, -1.0f);         /* acceleration's z, in g */
    put_float(p, out + 52, 1.0f);          /* the quaternion's w */
    for (unsigned i = 0; i < 4; ++i) {     /* the rotation and NED matrices */
        put_float(p, out + 56 + i * 20, 1.0f);
        put_float(p, out + 120 + i * 20, 1.0f);
    }
    put_float(p, out + 196, 1.0f);         /* basic orientation: face up */
    vita_write64(p, out + 200, us);
    vita_write32(p, out + 244, 2);         /* SCE_MOTION_MAG_STABLE */
    return 0;
}
/* sceAppMgrReceiveSystemEvent and sceAppUtilReceiveAppEvent: no event comes */
static uint32_t receive_system_event(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0x80802013u; /* SCE_APPMGR_ERROR_STATE */
}
static uint32_t receive_app_event(VitaProcess* p, VitaThread* t) {
    uint32_t out = vita_arg(p, t, 0);
    if (!out) return 0x80100600u; /* SCE_APPUTIL_ERROR_PARAMETER */
    vita_write32(p, out, 0); /* no event */
    return 0;
}
/* sceAppUtilAppParamGetInt(id, *value): the game's full version (SCE_APPUTIL_APPPARAM_ID_SKU_FLAG 0, SKU 3) */
static uint32_t app_param_get_int(VitaProcess* p, VitaThread* t) {
    uint32_t out = vita_arg(p, t, 1);
    if (!out || vita_arg(p, t, 0)) return 0x80100600u;
    vita_write32(p, out, 3); /* SCE_APPUTIL_APPPARAM_SKU_FLAG_FULL */
    return 0;
}
/* the game's save data slots: none until it makes one */
static uint32_t save_data_slot_get_param(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0x80100641u; /* SCE_APPUTIL_ERROR_SAVEDATA_SLOT_NOT_FOUND */
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
    {0xc995de02u, "sceRtcGetTime64_t", rtc_get_time64_t},
    {0xf2b238e2u, "sceRtcGetTick", rtc_get_tick},
    {0x23f79274u, "sceRtcGetCurrentTick", rtc_get_current_tick},
    {0xcddd25feu, "sceRtcGetCurrentNetworkTick", rtc_get_current_tick},
    {0x811313b3u, "sceRtcGetTickResolution", rtc_get_tick_resolution},
    {0x2f3531ebu, "sceRtcGetDayOfWeek", rtc_get_day_of_week},
    {0x3a332f81u, "sceRtcSetTime_t", rtc_set_time_t},
    {0x258be8ecu, "sceRtcGetAccumulativeTime", rtc_get_accumulative_time},
    {0x3ce187b6u, "scePowerSetConfigurationMode", zero},
    {0x28034ac9u, "sceMotionStartSampling", zero},
    {0x0fd2cda2u, "sceMotionReset", zero},
    {0xbdb32767u, "sceMotionGetState", motion_get_state},
    {0x10b5765fu, "sceAppMgrReceiveSystemEvent", receive_system_event},
    {0xee0dbed9u, "sceAppUtilReceiveAppEvent", receive_app_event},
    {0xcd7fd67au, "sceAppUtilAppParamGetInt", app_param_get_int},
    {0x93f0d89fu, "sceAppUtilSaveDataSlotGetParam", save_data_slot_get_param},
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
