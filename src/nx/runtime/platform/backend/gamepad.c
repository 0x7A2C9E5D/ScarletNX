/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
#include "runtime/platform/gamepad.h"
#include "runtime/options.h"
#include "runtime/platform/log.h"
#include "runtime/platform/ui.h"
#include "pad_sdl.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>

static SDL_Gamepad* g_pads[SNX_GAMEPADS];
static char g_names[SNX_GAMEPADS][64]; /* theirs, for the game's menu, which the renderer draws on its thread */
static int g_labels[SNX_GAMEPADS][4], g_symmetric[SNX_GAMEPADS]; /* and how its button map screen draws them */
static unsigned g_held;                                          /* the controls held, a bit each, as last polled */
#define UNSET SNX_PAD_UNSET
static SnxGamepadConfig g_cfg = {
    SNX_PAD_BY_LABEL,
    20,
    100,
    0,
    {{UNSET, UNSET, UNSET, UNSET, UNSET, UNSET, UNSET, UNSET, UNSET, UNSET, UNSET, UNSET, UNSET, UNSET, UNSET, UNSET}}};

/* SDL drops a gamepad's presses while no window of the program has the focus, unless told otherwise; set before it
   starts its gamepads (the .ini is read first), and at once on a change */
static void background_set(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, g_cfg.background ? "1" : "0");
}
/* the custom map: what it does not name goes as the labels say. One that cannot be read is said, and changes nothing */
static void map_set(const IniOpt* o, const char* v) {
    SnxPadMap map;
    char word[64];
    (void)o;
    snx_pad_map_unset(&map);
    if (snx_pad_map_read(&map, v, word, sizeof word))
        WARN("settings: gamepad_map's \"%s\" is not a control and the button it sends (south:B), so the map stays as "
             "it was\n",
             word);
    else
        g_cfg.own = map;
}
const IniOpt g_gamepad_ini[] = {
    {"gamepad_buttons", "1",
     "A controller's A, B, X and Y: 1 = as their labels say, 0 = by position, as on the console's, 2 = the map of\n"
     ";   gamepad_map",
     INI_INT, 0, &g_cfg.labels, 0, 2, NULL, "A B X Y", "By position|By label|Custom", 0, 0},
    {"gamepad_deadzone", "20", "The sticks' dead zone, in percent of their travel", INI_INT, 0, &g_cfg.deadzone, 0, 90,
     NULL, "Stick dead zone", "#%", 5, 0},
    {"gamepad_rumble", "100", "How strongly the controllers vibrate when the game asks, in percent: 0 = never", INI_INT,
     0, &g_cfg.rumble, 0, 100, NULL, "Vibration", "Off|#%", 10, 0},
    {"gamepad_background", "0", "1 = the controllers still play the game while its window is not focused", INI_INT, 0,
     &g_cfg.background, 0, 1, background_set, "Background input", "Off|On", 0, 0},
    {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0}};
const IniOpt g_gamepad_map_ini[] = {
    {"gamepad_map", NULL,
     "gamepad_buttons=2's map: a control by where it sits and the console's button it sends, as south:B east:A.\n"
     ";   Controls south east west north lb rb lt rt ls rs back start up down left right; buttons A B X Y L R ZL\n"
     ";   ZR L3 R3 Plus Minus Up Down Left Right None. One left out goes as its label says",
     INI_TEXT, 0, NULL, 0, 0, map_set, SNX_GAMEPAD_MAP, NULL, 0, 0},
    {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0}};
int gamepad_ini(const char* key, const char* value) {
    const IniOpt* o = ini_find(g_gamepad_ini, key);
    if (!o) o = ini_find(g_gamepad_map_ini, key);
    if (o) ini_read(o, value);
    return o != NULL;
}
SnxGamepadConfig* gamepad_config(void) { return &g_cfg; }
void gamepad_store(const int* setting) {
    const IniOpt* o;
    char v[16];
    for (o = g_gamepad_ini; o->key; o++)
        if (o->var == setting) {
            ini_format(o, v, sizeof v);
            ini_set(o->key, v);
            if (o->set) o->set(o, v);
        }
}
void gamepad_store_map(void) {
    char text[SNX_PAD_MAP_TEXT];
    snx_pad_map_write(&g_cfg.own, text, sizeof text);
    ini_set("gamepad_map", text);
}
int gamepad_layout(int i, int labels[4], int* symmetric) {
    int k, c;
    for (k = 0; k < SNX_GAMEPADS; k++)
        if (g_names[k][0] && !i--) {
            for (c = 0; c < 4; c++) labels[c] = g_labels[k][c];
            *symmetric = g_symmetric[k];
            return 1;
        }
    return 0;
}
unsigned gamepad_held(void) { return __atomic_load_n(&g_held, __ATOMIC_RELAXED); }
const char* gamepad_name(int i) {
    int k;
    for (k = 0; k < SNX_GAMEPADS; k++)
        if (g_names[k][0] && !i--) return g_names[k];
    return NULL;
}

static int is_open(SDL_JoystickID id) {
    int i;
    for (i = 0; i < SNX_GAMEPADS; i++)
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == id) return 1;
    return 0;
}
void gamepad_open(u32 id) {
    int i;
    if (is_open(id)) return;
    for (i = 0; i < SNX_GAMEPADS; i++)
        if (!g_pads[i]) {
            g_pads[i] = SDL_OpenGamepad(id);
            if (!g_pads[i]) return;
            LOG("input: gamepad %s (%s)\n", SDL_GetGamepadName(g_pads[i]),
                SDL_GetGamepadStringForType(SDL_GetGamepadType(g_pads[i])));
            ui_lock();
            snprintf(g_names[i], sizeof g_names[i], "%s",
                     SDL_GetGamepadName(g_pads[i]) ? SDL_GetGamepadName(g_pads[i]) : "Gamepad");
            snx_pad_labels(g_pads[i], g_labels[i]);
            g_symmetric[i] = snx_pad_symmetric(g_pads[i]);
            ui_unlock();
            return;
        }
}
void gamepad_close(u32 id) {
    SDL_JoystickID* ids;
    int i, n = 0;
    for (i = 0; i < SNX_GAMEPADS; i++)
        if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == id) {
            SDL_CloseGamepad(g_pads[i]);
            g_pads[i] = NULL;
            ui_lock();
            g_names[i][0] = 0;
            ui_unlock();
        }
    /* a fifth pad connected while four were open takes the free place */
    ids = SDL_GetGamepads(&n);
    if (!ids) return;
    for (i = 0; i < n; i++) gamepad_open(ids[i]);
    SDL_free(ids);
}

/* the dead zone as a circle, so a stick pushed along one axis keeps the small part of the other, and the travel past
   it spread over the whole range, so the stick starts from nothing and still reaches the edge (a square gate's corner
   is past it) */
static void dz(i32* x, i32* y) {
    double r = sqrt((double)*x * *x + (double)*y * *y), z = g_cfg.deadzone * 32767.0 / 100, k;
    if (r <= z) {
        *x = *y = 0;
        return;
    }
    k = ((r < 32767 ? r : 32767) - z) / (32767 - z) * 32767 / r;
    *x = (i32)(*x * k);
    *y = (i32)(*y * k);
}
/* SDL's Y axes point down, the Switch's up */
static i32 up(i32 v) { return v == -32768 ? 32767 : -v; }

/* what each control of g sends, as gamepad_buttons says */
static void map_of(SDL_Gamepad* g, SnxPadMap* map) {
    int labels[4];
    SnxPadMap labeled;
    if (g_cfg.labels == SNX_PAD_BY_POSITION) {
        snx_pad_map_position(map);
        return;
    }
    snx_pad_labels(g, labels);
    snx_pad_map_labels(&labeled, labels);
    *map = labeled;
    if (g_cfg.labels == SNX_PAD_CUSTOM) {
        *map = g_cfg.own;
        snx_pad_map_fill(map, &labeled);
    }
}
u64 gamepad_button(u32 id, int button) {
    SDL_Gamepad* g = SDL_GetGamepadFromID(id);
    int control = snx_pad_of_button(button);
    SnxPadMap map;
    if (!g || control < 0) return 0;
    map_of(g, &map);
    return map.bit[control] < 0 ? 0 : 1ull << map.bit[control];
}

void gamepad_state(u64* buttons, i32 sticks[4]) {
    u64 b = 0;
    unsigned held = 0;
    int i;
    sticks[0] = sticks[1] = sticks[2] = sticks[3] = 0;
    for (i = 0; i < SNX_GAMEPADS; i++) {
        SDL_Gamepad* g = g_pads[i];
        SnxPadMap map;
        int c;
        if (!g) continue;
        map_of(g, &map);
        for (c = 0; c < SNX_PAD_CONTROLS; c++)
            if (snx_pad_held(g, c)) {
                held |= 1u << c;
                if (map.bit[c] >= 0) b |= 1ull << map.bit[c];
            }
        if (!sticks[0] && !sticks[1]) {
            sticks[0] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
            sticks[1] = up(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));
            dz(&sticks[0], &sticks[1]);
        }
        if (!sticks[2] && !sticks[3]) {
            sticks[2] = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX);
            sticks[3] = up(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY));
            dz(&sticks[2], &sticks[3]);
        }
    }
    *buttons = b;
    __atomic_store_n(&g_held, held, __ATOMIC_RELAXED); /* the renderer's thread draws them */
}

void gamepad_rumble(u32 left, u32 right) {
    /* each motor as the stronger of the two devices' bands: the heavy one low, the light one high */
    u32 lo = (left & 0xFFFF) > (right & 0xFFFF) ? left & 0xFFFF : right & 0xFFFF;
    u32 hi = left >> 16 > right >> 16 ? left >> 16 : right >> 16;
    int i;
    lo = lo * (u32)g_cfg.rumble / 100;
    hi = hi * (u32)g_cfg.rumble / 100;
    /* SDL sends a pad only what changed, and stops it when the time runs out: the game's thread may stop sending */
    for (i = 0; i < SNX_GAMEPADS; i++)
        if (g_pads[i]) SDL_RumbleGamepad(g_pads[i], (Uint16)lo, (Uint16)hi, 100);
}
