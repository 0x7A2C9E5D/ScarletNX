/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/hle/services/hid_motion.h"
#include <assert.h>
#include <math.h>
#include <string.h>

static float f32_at(const u8* state, int offset) {
    float value;
    memcpy(&value, state + offset, 4);
    return value;
}
static u64 u64_at(const u8* state, int offset) {
    u64 value;
    memcpy(&value, state + offset, 8);
    return value;
}
static u32 u32_at(const u8* state, int offset) {
    u32 value;
    memcpy(&value, state + offset, 4);
    return value;
}
static int near(float a, float b) { return fabsf(a - b) < 0.0005f; }

int main(void) {
    HidMotionState state;
    PadMotion motion;
    u8 out[HS_HidSixAxisSensorState];
    memset(&state, 0, sizeof state);
    memset(&motion, 0, sizeof motion);
    hid_motion_sample(&state, NULL, 0, 1, out);
    assert(u64_at(out, 8) == 1 && near(f32_at(out, HO_HidSixAxisSensorState_acceleration + 8), -1.0f));
    assert(near(f32_at(out, HO_HidSixAxisSensorState_direction), 1.0f));
    assert(u32_at(out, HO_HidSixAxisSensorState_attributes) == 1);
    assert(hid_motion_at_rest(NULL));

    motion.valid = 1;
    motion.acceleration[2] = -1.0f;
    motion.angular_velocity[2] = 1.57079632679f;
    hid_motion_sample(&state, &motion, 1000000000ull, 2, out);
    /* A delayed frame integrates only 100 ms, preventing a one-second jump. */
    assert(u64_at(out, 0) == 100000000ull && u64_at(out, 8) == 2);
    assert(near(f32_at(out, HO_HidSixAxisSensorState_acceleration + 8), -1.0f));
    assert(near(f32_at(out, HO_HidSixAxisSensorState_acceleration + 20), 1.57079632679f));
    assert(near(f32_at(out, HO_HidSixAxisSensorState_acceleration + 32), 0.15707963f));
    assert(f32_at(out, HO_HidSixAxisSensorState_direction + 4) < -0.15f);
    assert(!hid_motion_at_rest(&motion));

    motion.angular_velocity[2] = 0.0f;
    motion.acceleration[0] = 0.0f;
    motion.acceleration[1] = 0.0f;
    motion.acceleration[2] = -1.0f;
    assert(hid_motion_at_rest(&motion));
    hid_motion_sample(&state, NULL, 16000000, 3, out);
    assert(near(f32_at(out, HO_HidSixAxisSensorState_direction), 1.0f));
    assert(near(f32_at(out, HO_HidSixAxisSensorState_acceleration + 32), 0.0f));
    /* The first accelerometer sample determines pitch and roll even without a prior gyro movement. */
    memset(&state, 0, sizeof state);
    memset(&motion, 0, sizeof motion);
    motion.valid = 1;
    motion.acceleration[1] = 1.0f;
    hid_motion_sample(&state, &motion, 0, 4, out);
    assert(near(f32_at(out, HO_HidSixAxisSensorState_direction + 28), -1.0f));
    /* A short gyro impulse disturbs that tilt. Gravity brings it back when the pad rests. */
    motion.angular_velocity[0] = 1.0f;
    hid_motion_sample(&state, &motion, 100000000, 5, out);
    motion.angular_velocity[0] = 0.0f;
    {
        int i;
        for (i = 0; i < 20; i++) {
            hid_motion_sample(&state, &motion, 100000000, (u64)(6 + i), out);
            assert(near(f32_at(out, HO_HidSixAxisSensorState_acceleration + 12), 0.0f));
            assert(near(f32_at(out, HO_HidSixAxisSensorState_acceleration + 24), 0.1f));
        }
    }
    assert(f32_at(out, HO_HidSixAxisSensorState_direction + 28) < -0.999f);
    return 0;
}
