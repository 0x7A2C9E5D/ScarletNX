/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* sm: the service manager, which hands out sessions to the other services */
#include "hle/services/services_int.h"
#include "hle/services/sm.swipc.h"
/* ================================================================ sm */
static SvcObj* svc_generic(const char* name); /* an empty success reply for every command, logged */
int nn_sm_detail_IUserInterface_GetServiceHandle(SvcObj* self, const nn_sm_ServiceName* name, u32* session) {
    char n[9] = {0};
    SvcObj* s;
    memcpy(n, name->v, 8);
    s = svc_open(n);
    if (!s) {
        WARN("sm: service '%s' does not exist in ScarletNX: using a stub\n", n);
        s = svc_generic(n);
    }
    LOG("sm: GetServiceHandle(%s)\n", n);
    *session = session_new(s);
    return 0;
}
/* a game's access control lets it host no service */
int nn_sm_detail_IUserInterface_RegisterService(SvcObj* self, const nn_sm_ServiceName* name, u8 is_light,
                                                i32 max_sessions, u32* port) {
    return RC(21, 8); /* sm NotAllowed */
}
int nn_sm_detail_IUserInterface_UnregisterService(SvcObj* self, const nn_sm_ServiceName* name) { return RC(21, 8); }
SvcObj* new_sm(void) { return nn_sm_detail_IUserInterface_new(0); }

/* unknown service -> generic stub, everything succeeds */
typedef struct {
    char name[16];
} GenData;
static int generic_disp(SvcObj* o, IpcCtx* c) {
    if (ipc_warn_once(((GenData*)o->data)->name, c->cmd))
        WARN("  [not implemented] %s cmd %u -> ok\n", ((GenData*)o->data)->name, c->cmd);
    return 0;
}
static SvcObj* svc_generic(const char* n) {
    SvcObj* o = svcobj_new("generic", generic_disp, sizeof(GenData));
    strncpy(((GenData*)o->data)->name, n, 15);
    o->name = ((GenData*)o->data)->name;
    return o;
}
