/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "vita/gpu/present.h"
#include "vita/gpu/render.h"
#include "vita/gpu/vk.h"
#include "vita/platform/input.h"
#include <SDL3/SDL.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    char* shot;
    bool video;
} g_present;
static volatile sig_atomic_t g_interrupted;
static void on_signal(int signal) {
    (void)signal;
    g_interrupted = 1;
}
void vita_present_init(const char* title, const char* shot) {
    char error[256];
    if (!vita_vk_init(title, error, sizeof error)) printf("vita: no Vulkan device, nothing is drawn: %s\n", error);
    g_present.shot = shot && *shot ? strdup(shot) : NULL;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    vita_input_init();
}
bool vita_present_vblank(VitaProcess* p) {
    if (p->display.base) vita_render_show(p, p->display.base, p->display.pitch, p->display.width, p->display.height);
    SDL_Event event;
    while (SDL_WasInit(SDL_INIT_EVENTS) && SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return false;
        vita_input_event(&event);
    }
    return !g_interrupted;
}
/* SceDisplay's formats: A8B8G8R8 (0, the bytes R, G, B, A) and the others the runtime doesn't show yet */
void vita_present_shot(VitaProcess* p) {
    if (!g_present.shot || !p->display.base) return;
    vita_render_idle(p, true);
    char* path = malloc(strlen(g_present.shot) + 5);
    if (!path) return;
    sprintf(path, "%s.ppm", g_present.shot);
    FILE* f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", (unsigned)p->display.width, (unsigned)p->display.height);
        for (uint32_t y = 0; y < p->display.height; ++y)
            for (uint32_t x = 0; x < p->display.width; ++x) {
                const unsigned char* px = vita_ptr(p, p->display.base + (y * p->display.pitch + x) * 4);
                fwrite(px, 1, 3, f);
            }
        fclose(f);
        printf("Screenshot: %s\n", path);
    }
    free(path);
}
void vita_present_free(void) {
    vita_input_free();
    vita_vk_free();
    free(g_present.shot);
    memset(&g_present, 0, sizeof g_present);
}
