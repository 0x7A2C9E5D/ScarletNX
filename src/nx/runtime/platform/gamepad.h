/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SDL gamepad discovery and per-player input (backend/gamepad.c). */
#ifndef SNX_GAMEPAD_H
#define SNX_GAMEPAD_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/runtime/ini.h"
#include "common/utilities/pad_map.h"
#include "nx/hle/services/hid.h"

#define SNX_GAMEPADS 8 /* one slot for each guest Npad player */
/* a gamepad the system added or removed (its SDL joystick id): opened, closed */
void gamepad_open(u32 id);
void gamepad_close(u32 id);
/* enumerate connected SDL joysticks at startup, or inspect one newly attached joystick */
void gamepad_scan(void);
void gamepad_detect(u32 id);
/* all open gamepads together, for the game's controller overlay and legacy callers */
void gamepad_state(u64* buttons, i32 sticks[4]);
void gamepad_state_slot(int slot, u64* buttons, i32 sticks[4]);
int gamepad_slot_type(int slot); /* SDL_GamepadType, or SDL_GAMEPAD_TYPE_UNKNOWN */
int gamepad_slot_connected(int slot);
u32 gamepad_slot_id(int slot); /* SDL joystick instance ID, 0 for an empty slot */
void gamepad_motion(int slot, PadMotion out[2]);
/* the HidNpadButton bit one of a gamepad's buttons (an SDL_GamepadButton) is, as the game reads it; 0 for none */
u64 gamepad_button(u32 id, int button);
/* legacy combined rumble, played on every open gamepad that can vibrate */
void gamepad_rumble(u32 left, u32 right);
/* one guest player's vibration (hid.h's PadState.rumble), played on that slot's gamepad each frame */
void gamepad_rumble_slot(int slot, u32 left, u32 right);
/* the settings, kept in the game's .ini; the game's menu changes them under ui_lock */
typedef struct {
    int swap_ab;     /* 1: A and B swapped, from the console's layout */
    int deadzone;    /* the sticks', in percent of their travel */
    int rumble;      /* the vibration's strength in percent, 0 for none */
    int background;  /* 1: they play while the game's window is not focused */
    int joycon_pair; /* 1: SDL combines a left and a right Joy-Con */
    SnxPadMap own;   /* the player's map, SNX_PAD_UNSET where a control sends the button in its place */
} SnxGamepadConfig;
SnxGamepadConfig* gamepad_config(void);
void gamepad_store(const int* setting); /* one of gamepad_config()'s, to the .ini, and applied */
void gamepad_store_map(void);           /* the custom map, to the .ini */
/* the i-th open gamepad's face labels and whether its sticks sit side by side: 1, 0 past the last. Under ui_lock */
int gamepad_layout(int i, int labels[4], int* symmetric);
unsigned gamepad_held(void); /* the controls (pad_map.h's) held at the last gamepad_state, a bit each */
/* the open gamepads' names, from 0 on: NULL past the last. Under ui_lock */
const char* gamepad_name(int i);
extern const IniOpt g_gamepad_ini[];                 /* the settings, keys gamepad_* */
#define SNX_GAMEPAD_PAGE "Controllers"               /* their page in the game's menu and the launcher's Settings */
#define SNX_GAMEPAD_MAP  "Button map"                /* and the screen of what each button sends */
extern const IniOpt g_gamepad_map_ini[];             /* gamepad_map, the player's map that screen sets */
int gamepad_ini(const char* key, const char* value); /* a key of the game's .ini: applied, 1; not one of these, 0 */

#endif
