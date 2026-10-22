/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The imported functions by NID: each import of the image (vita/loader/image.h) is looked up in the modules' tables
   at its first call. A NID names one function wherever it is imported from, so the tables are by NID alone */
#include "vita/hle/hle.h"
#include "snx_settings.h"
#include <inttypes.h>
#include <stdio.h>

uint32_t vita_arg(VitaProcess* p, VitaThread* t, unsigned n) {
    return n < 4 ? t->cpu.r[n] : vita_read32(p, t->cpu.r[13] + (n - 4) * 4);
}
static const VitaHle* find(uint32_t nid) {
    static const VitaHle* const tables[] = {vita_hle_kernel, vita_hle_io,    vita_hle_ctrl, vita_hle_display,
                                            vita_hle_audio,  vita_hle_misc, vita_hle_gxm};
    for (size_t i = 0; i < sizeof tables / sizeof *tables; ++i)
        for (const VitaHle* h = tables[i]; h->name; ++h)
            if (h->nid == nid) return h;
    return NULL;
}
void vita_hle_call(VitaProcess* p, VitaThread* t, uint32_t index) {
    const VitaImport* import = &p->image.imports[index];
    const VitaHle* h = p->hle[index];
    if (!h && !p->unknown_logged[index]) {
        h = find(import->nid);
        p->hle[index] = h;
        if (!h) p->unknown_logged[index] = 1;
    }
    ++p->hle_calls;
    uint32_t r[4] = {t->cpu.r[0], t->cpu.r[1], t->cpu.r[2], t->cpu.r[3]};
    if (!h) {
        /* the game goes on with 0, success for most of the system's functions, logged the first time */
        if (p->unknown_logged[index] == 1 || g_settings.vita_import_trace) {
            printf("vita: %s's 0x%08" PRIx32 " is not implemented (thread \"%s\", lr 0x%08" PRIx32 ")\n",
                   import->library, import->nid, t->name, t->cpu.r[14]);
            p->unknown_logged[index] = 2;
        }
        t->cpu.r[0] = 0;
        return;
    }
    uint32_t result = h->function(p, t);
    if (t->state == VITA_THREAD_READY && p->current == t) t->cpu.r[0] = result;
    if (g_settings.vita_import_trace > 1)
        printf("vita: \"%s\" %s(0x%" PRIx32 ", 0x%" PRIx32 ", 0x%" PRIx32 ", 0x%" PRIx32 ")%s 0x%08" PRIx32
               " (sp 0x%08" PRIx32 ", lr 0x%08" PRIx32 ")\n",
               t->name, h->name, r[0], r[1], r[2], r[3], t->state == VITA_THREAD_WAITING ? " waits," : " ->",
               t->cpu.r[0], t->cpu.r[13], t->cpu.r[14]);
}
