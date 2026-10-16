/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/hle/services/hid_motion.h"
#include <cglm/cglm.h>
#include <math.h>
#include <string.h>

int hid_motion_at_rest(const PadMotion* motion) {
    float magnitude;
    float rate;
    if (!motion || !motion->valid) return 1;
    magnitude = motion->acceleration[0] * motion->acceleration[0] + motion->acceleration[1] * motion->acceleration[1] +
                motion->acceleration[2] * motion->acceleration[2];
    rate = motion->angular_velocity[0] * motion->angular_velocity[0] +
           motion->angular_velocity[1] * motion->angular_velocity[1] +
           motion->angular_velocity[2] * motion->angular_velocity[2];
    return magnitude > 0.85f && magnitude < 1.15f && rate < 0.0025f;
}

void hid_motion_sample(HidMotionState* state, const PadMotion* motion, u64 dt_ns, u64 sampling_number,
                       u8 out[HS_HidSixAxisSensorState]) {
    vec3 acc = {0.0f, 0.0f, -1.0f};
    vec3 gyro = {0.0f, 0.0f, 0.0f};
    vec3 corrected = {0.0f, 0.0f, 0.0f};
    versor q = {state->quaternion[1], state->quaternion[2], state->quaternion[3], state->quaternion[0]};
    mat3 matrix;
    float dir[9];
    float rate, gravity_norm;
    u32 attr = 1; /* HidSixAxisSensorAttribute_IsConnected */
    memset(out, 0, HS_HidSixAxisSensorState);
    if (glm_quat_norm(q) == 0.0f) glm_quat_identity(q);
    if (motion && motion->valid) {
        memcpy(acc, motion->acceleration, sizeof acc);
        memcpy(gyro, motion->angular_velocity, sizeof gyro);
        memcpy(corrected, gyro, sizeof corrected);
        gravity_norm = glm_vec3_norm(acc);
        /* Acceleration includes gravity. It sets the first pitch and roll, then corrects gyro drift when linear
           acceleration is small. Yaw has no gravity reference. */
        if (gravity_norm > 0.85f && gravity_norm < 1.15f) {
            vec3 measured, down = {0.0f, 0.0f, -1.0f};
            glm_vec3_scale(acc, 1.0f / gravity_norm, measured);
            if (!state->was_valid) {
                if (1.0f - measured[2] < 0.0001f)
                    glm_quat_init(q, 1.0f, 0.0f, 0.0f, 0.0f); /* keep the upside-down pose's X axis */
                else
                    glm_quat_from_vecs(measured, down, q);
            } else {
                versor inverse;
                vec3 predicted, error;
                glm_quat_conjugate(q, inverse);
                glm_quat_rotatev(inverse, down, predicted);
                glm_vec3_cross(measured, predicted, error);
                for (int i = 0; i < 3; i++) corrected[i] += 2.5f * error[i];
            }
        }
        state->was_valid = 1;
    } else if (state->was_valid) {
        /* A disconnected or disabled sensor starts from a neutral pose when it returns. */
        memset(state, 0, sizeof *state);
        glm_quat_identity(q);
    }
    /* Guard a suspended host or debugger pause from producing a huge jump. */
    if (dt_ns > 100000000ull) dt_ns = 100000000ull;
    rate = glm_vec3_norm(corrected);
    if (rate > 0.000001f && rate < 1000000.0f) {
        versor delta, next;
        glm_quatv(delta, rate * ((float)dt_ns * 0.000000001f), corrected);
        glm_quat_mul(q, delta, next);
        glm_quat_normalize_to(next, q);
    }
    for (int i = 0; i < 3; i++) state->angle[i] += gyro[i] * ((float)dt_ns * 0.000000001f);
    /* cglm stores [x,y,z,w] and column-major matrices. The HID state stores a scalar-first quaternion internally
       and serializes the direction matrix row by row. */
    state->quaternion[0] = q[3];
    for (int i = 0; i < 3; i++) state->quaternion[i + 1] = q[i];
    glm_quat_mat3(q, matrix);
    for (int row = 0; row < 3; row++)
        for (int column = 0; column < 3; column++) dir[row * 3 + column] = matrix[column][row];
    memcpy(out, &dt_ns, 8);
    memcpy(out + 8, &sampling_number, 8);
    memcpy(out + HO_HidSixAxisSensorState_acceleration, acc, sizeof acc);
    memcpy(out + HO_HidSixAxisSensorState_acceleration + 12, gyro, sizeof gyro);
    memcpy(out + HO_HidSixAxisSensorState_acceleration + 24, state->angle, sizeof state->angle);
    memcpy(out + HO_HidSixAxisSensorState_direction, dir, sizeof dir);
    memcpy(out + HO_HidSixAxisSensorState_attributes, &attr, 4);
}
