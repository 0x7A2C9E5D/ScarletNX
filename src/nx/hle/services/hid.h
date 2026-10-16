/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* input (hid.c) */
#ifndef SNX_HID_H
#define SNX_HID_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/hle/services/hid_touch.h"
#include "nx/runtime/platform/plat.h"

/* One sensor on a paired Joy-Con, or the primary sensor on a full-size pad.
   Values are in the Switch controller frame: acceleration in g and angular velocity in radians/second. */
typedef struct {
    float acceleration[3];
    float angular_velocity[3];
    int valid;
} PadMotion;

typedef enum { SNX_PAD_FULL_KEY = 0, SNX_PAD_JOY_DUAL, SNX_PAD_JOY_LEFT, SNX_PAD_JOY_RIGHT } PadKind;

typedef struct {
    u64 buttons;
    i32 lx, ly, rx, ry;
    int connected;       /* SDL pad present; No1 also stays connected for keyboard and scripted input */
    PadKind kind;        /* the physical controller, independent of styles the game supports */
    u32 source_id;       /* SDL instance ID; changes when a physical controller is replaced */
    PadMotion motion[2]; /* left/primary, right; the right sensor can be absent */
    /* the vibration the game last sent the left and the right device, amp_low | amp_high << 16 at 0 to 65535 each:
       written by its threads, played by the frontend's */
    u32 rumble[2];
} PadState;
extern PadState g_pad;
extern PadState g_pad_extra[7]; /* No2 through No8, in Npad ID order */
extern TouchState g_touch;      /* the frontend's fingers, which hid_update turns into the touch screen's samples */
extern PMutex g_pad_input_lock; /* guards input fields during frontend publication and HID sampling */
void hid_update(void);
int hid_connected_players(u32* first_id);

#endif
