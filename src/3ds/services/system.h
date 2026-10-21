/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_SYSTEM_H
#define SNX_CTR_SYSTEM_H
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/runtime/ctr_process.h"
#define SNX_CTR_SYSTEM_UNKNOWN 0xffffffffu
/* A session of the system service by that name, one the runtime has no model of: its result, or
   SNX_CTR_SYSTEM_UNKNOWN for a name that is no such service */
uint32_t snx_ctr_system_open(SnxCtrProcess* p, const uint8_t* name, uint32_t length, uint32_t* session);
const char* snx_ctr_system_name(uint32_t index);
void snx_ctr_system_dispatch(SnxCtrIpc* c);
#endif
