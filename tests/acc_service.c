/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* acc (acc.c) through its generated dispatchers: the user's profile and its picture, Wimsy drawn as a 256x256 JPEG of
   the size GetImageSize tells, read back by plutovg's decoder. SNX_TEST_PICTURE names a file to keep the JPEG in */
#include "nx/hle/services/services_int.h"
#include <plutovg.h>
#include <stdarg.h>

#include "nx/hle/services/acc.c"

u8* g_mem;
u64 g_user_id[2] = {0x0123456789ABCDEFull, 0xFEDCBA9876543210ull};
int g_verbose;
static unsigned test_checks, test_objects;

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "acc_service.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

void logf_(int level, const char* format, ...) {
    (void)level;
    (void)format;
}
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), size_t size) {
    SvcObj* obj = calloc(1, sizeof *obj);
    CHECK(obj != NULL);
    obj->name = name;
    obj->dispatch = dispatch;
    obj->refs = 1;
    if (size) {
        obj->data = calloc(1, size);
        CHECK(obj->data != NULL);
    }
    test_objects++;
    return obj;
}
void svcobj_unref(SvcObj* obj) {
    if (!obj || --obj->refs) return;
    if (obj->destroy) obj->destroy(obj);
    free(obj->data);
    free(obj);
    test_objects--;
}
void out_obj(IpcCtx* c, SvcObj* obj) {
    CHECK(c->nout_objs < 8);
    c->out_objs[c->nout_objs++] = obj;
}
void out_copy_h(IpcCtx* c, u32 handle) {
    CHECK(c->nout_copy < 8);
    c->out_copy[c->nout_copy++] = handle;
}
void ipc_out_overflow(IpcCtx* c) {
    (void)c;
    test_fail("IPC output overflow", __LINE__);
}
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(11, 420);
}
int ipc_bad_objects(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(10, 235);
}
int stub_ok(SvcObj* o, IpcCtx* c) {
    fprintf(stderr, "unexpected stub: %s command %u\n", o->name, c->cmd);
    test_fail("a command without a handler", __LINE__);
    return -1;
}
int quiet_ok(SvcObj* o, IpcCtx* c) { return stub_ok(o, c); }
void svc_register(const char* name, SvcCtor ctor) {
    (void)name;
    (void)ctor;
}
int new_event(u32* event, const char* name, int signaled) {
    (void)name, (void)signaled;
    *event = 0x100;
    return 0;
}

typedef struct {
    IpcCtx c;
    u8 raw[32];
} Request;
static IpcCtx* request(Request* r, u32 cmd) {
    memset(r, 0, sizeof *r);
    r->c.cmd = cmd;
    r->c.raw = r->raw;
    r->c.raw_size = sizeof r->raw;
    return &r->c;
}
static u32 reply32(Request* r, u32 off) {
    u32 v;
    CHECK(off + 4 <= r->c.out_size);
    memcpy(&v, r->c.out + off, 4);
    return v;
}
static void call(SvcObj* o, Request* r) { CHECK(o->dispatch(o, &r->c) == 0); }

/* the frame of a JPEG: its first start of frame (C0 to C2) before the scan, and the components it has */
static int jpeg_frame(const u8* d, u32 size, u32* width, u32* height, u32* components) {
    u32 at = 2;
    while (at + 4 <= size && d[at] == 0xff) {
        u32 marker = d[at + 1], length = (u32)d[at + 2] << 8 | d[at + 3];
        if (marker >= 0xc0 && marker <= 0xc2 && at + 10 <= size) {
            *height = (u32)d[at + 5] << 8 | d[at + 6];
            *width = (u32)d[at + 7] << 8 | d[at + 8];
            *components = d[at + 9];
            return 1;
        }
        if (marker == 0xda || length < 2) return 0;
        at += 2 + length;
    }
    return 0;
}

/* acc:u0's GetProfile (5) of the user, then IProfile's GetImageSize (10) and LoadImage (11) into a buffer of
   0x20000, what a game reserves; and LoadImage into less, which writes what fits and tells the whole size */
static void test_picture(void) {
    SvcObj *acc = new_acc(), *profile;
    Request r;
    u32 size, loaded, width = 0, height = 0, components = 0;
    const u8* picture = g_mem + 0x1000;
    request(&r, 5);
    memcpy(r.raw, g_user_id, 16);
    call(acc, &r);
    CHECK(r.c.nout_objs == 1);
    profile = r.c.out_objs[0];

    request(&r, 10);
    call(profile, &r);
    size = reply32(&r, 0);
    request(&r, 11);
    r.c.nb = 1;
    r.c.b[0].addr = 0x1000;
    r.c.b[0].size = 0x20000;
    call(profile, &r);
    loaded = reply32(&r, 0);
    CHECK(size == loaded && size > 4 && size <= 0x20000);
    CHECK(picture[0] == 0xff && picture[1] == 0xd8 && picture[size - 2] == 0xff && picture[size - 1] == 0xd9);
    CHECK(jpeg_frame(picture, size, &width, &height, &components));
    CHECK(width == 256 && height == 256 && components == 3);
    if (getenv("SNX_TEST_PICTURE")) {
        FILE* f = fopen(getenv("SNX_TEST_PICTURE"), "wb");
        CHECK(f && fwrite(picture, 1, size, f) == size && !fclose(f));
    }
    /* it decodes: white in the corner, around the drawing, and Wimsy in the middle */
    plutovg_surface_t* decoded = plutovg_surface_load_from_image_data(picture, (int)size);
    CHECK(decoded && plutovg_surface_get_width(decoded) == 256 && plutovg_surface_get_height(decoded) == 256);
    const u8* corner = plutovg_surface_get_data(decoded);
    const u8* middle = corner + 128 * plutovg_surface_get_stride(decoded) + 128 * 4;
    CHECK(corner[0] > 240 && corner[1] > 240 && corner[2] > 240);
    CHECK(middle[0] < 200 || middle[1] < 200 || middle[2] < 200);
    plutovg_surface_destroy(decoded);

    memset(g_mem + 0x1000, 0xee, 0x20000);
    request(&r, 11);
    r.c.nb = 1;
    r.c.b[0].addr = 0x1000;
    r.c.b[0].size = 16;
    call(profile, &r);
    CHECK(reply32(&r, 0) == size && picture[0] == 0xff && picture[15] != 0xee && picture[16] == 0xee);

    svcobj_unref(profile);
    svcobj_unref(acc);
}

int main(void) {
    g_mem = calloc(1, 0x30000);
    CHECK(g_mem != NULL);
    test_picture();
    CHECK(test_objects == 0);
    free(g_mem);
    printf("acc_service: %u checks passed\n", test_checks);
    return 0;
}
