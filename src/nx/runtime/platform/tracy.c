/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Tracy client lifetime and emission gate; the client is manual-lifetime. */
#include "nx/runtime/platform/tracy.h"
#include "common/utilities/build_info.h"
#include "common/utilities/common.h"
#include "nx/runtime/main.h"
#include "nx/runtime/startup.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "snx_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(SNX_TRACY)
#include <SDL3/SDL_process.h>
/* g_tracy_emit gates the emissions, g_tracy_pending counts them in flight, t_zones holds this thread's
   zones, t_attached its mirror of the connection, t_name the name it goes by. */
#define SNX_TRACY_ZONE_MAX 64
#define SNX_TRACY_POLL 4096
static int g_tracy_emit;
static int g_tracy_pending;
static int g_tracy_started;
static int g_tracy_stopped;
static __thread TracyCZoneCtx t_zones[SNX_TRACY_ZONE_MAX];
static __thread int t_zone_depth;
static __thread int t_attached;
static __thread unsigned t_zone_tick;
static __thread char t_name[64];

/* The launcher's tracy-capture process; it writes the trace when the client disconnects. */
static SDL_Process* g_tracy_capture;
static char g_tracy_capture_path[sizeof g_exedir + sizeof g_exename + 64];
static char g_tracy_title[256];
static char g_tracy_port[16]; /* the port the client listens on, and the capture connects to */
#define SNX_TRACY_RUN_SECONDS 3600
#define SNX_TRACY_NOW_SECONDS 60

/* One capture at a time; shutdown may lock from another thread. */
static PMutex g_tracy_capture_lock = PMUTEX_INIT;
static void tracy_capture_lock(void) { pm_lock(&g_tracy_capture_lock); }
static void tracy_capture_unlock(void) { pm_unlock(&g_tracy_capture_lock); }

/* the plots' formats, once a GUI attaches */
static void tracy_plots_send(void);

/* The sampler starts when the run asks for it, tracy_sampling, at the connect. Its samples land on the timeline as
   gray sample zones, which with JIT'd guest code and the drivers' own threads (the NVIDIA helpers', MangoHud's) reads
   as a wall of [unknown]; the instrumented zones alone don't, so the sampler is off unless the trace wants the flame
   graphs. */
static int g_tracy_sampling;
static void tracy_sampling_start(void) {
    if (!g_settings.tracy_sampling) return;
    if (__atomic_exchange_n(&g_tracy_sampling, 1, __ATOMIC_ACQ_REL)) return;
    if (TracyCBeginSamplingProfiling()) LOG("tracy: sampling the run's stacks\n");
}
static void tracy_sampling_stop(void) {
    if (!__atomic_exchange_n(&g_tracy_sampling, 0, __ATOMIC_ACQ_REL)) return;
    TracyCEndSamplingProfiling();
}

/* A tool of Tracy's: beside the runtime first (the build it was made with), then PATH. */
static char* tracy_tool(const char* name) {
    char* beside = snx_path_join(g_exedir[0] ? g_exedir : ".", name);
    char* tool = beside ? snx_find_tool(beside) : NULL;
    free(beside);
    return tool ? tool : snx_find_tool(name);
}

/* A thread that named itself before the client was up: its name went nowhere, so it goes out again on attaching. */
static void tracy_attach_thread(void) {
    if (t_name[0]) TracyCSetThreadName(t_name);
}

/* The client listens on the port it reads as it starts: the setting's, or, when a capture is asked for, one of the
   run's own so an earlier capture (or a GUI) on 8086 is not in the way. Without either the client keeps Tracy's 8086.
   Tracy's client still reads TRACY_PORT from the environment, so that one variable is set for it; nothing here reads
   the environment back. */
static void tracy_port_setup(int capture) {
    const char* port = g_settings.tracy_port;
    if (g_tracy_port[0]) return;
    if (port[0]) {
        /* A port Tracy's client can bind: it takes the setting as it is, and a value that is not a number makes
           it exit and leave the capture connecting to nothing, so it is checked here rather than passed on. */
        char* end;
        unsigned long n = strtoul(port, &end, 10);
        if (*port && !*end && end != port && n > 0 && n < 65536)
            snprintf(g_tracy_port, sizeof g_tracy_port, "%s", port);
        else {
            WARN("tracy: Tracy port %s is not a port number\n", port);
            port = "";
        }
    }
    if (!g_tracy_port[0]) {
        /* none named, or an unusable one: only a capture needs a port of its own, and without one the client
           keeps Tracy's 8086 */
        if (!capture) return;
        snprintf(g_tracy_port, sizeof g_tracy_port, "%d", 18086 + (int)(plat_tid() % 1000));
    }
    if (plat_setenv("TRACY_PORT", g_tracy_port)) WARN("tracy: cannot set the client's port %s\n", g_tracy_port);
}

static int tracy_capture_start(int seconds, int launch) {
    char dir[sizeof g_exedir + 16], seconds_text[16], *tool;
    const char* named = g_settings.tracy_capture_tool;
    const char* argv[9];
    const char* suffix = launch ? "; the run was not started" : "";
    int n = 0;
    struct tm t;
    time_t now;
    snprintf(dir, sizeof dir, "%scaptures", g_exedir);
    tool = named[0] ? snx_find_tool(named) : tracy_tool("tracy-capture");
    if (!tool) {
        WARN("tracy: %s needs tracy-capture 0.14.1 beside the runtime or on PATH%s\n",
             launch ? "--tracy-capture" : "a capture", suffix);
        return 0;
    }
    if (snx_mkdirs(dir)) {
        WARN("tracy: cannot make the capture folder %s%s\n", dir, suffix);
        free(tool);
        return 0;
    }
    now = time(NULL);
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    snprintf(g_tracy_capture_path, sizeof g_tracy_capture_path, "%s/tracy_%s_%04d%02d%02d_%02d%02d%02d.tracy", dir,
             g_exename, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    snx_path_native(g_tracy_capture_path);
    argv[n++] = tool;
    argv[n++] = "-o";
    argv[n++] = g_tracy_capture_path;
    argv[n++] = "-f";
    argv[n++] = "-s";
    snprintf(seconds_text, sizeof seconds_text, "%d", seconds);
    argv[n++] = seconds_text;
    if (g_tracy_port[0]) {
        argv[n++] = "-p";
        argv[n++] = g_tracy_port;
    }
    argv[n] = NULL;
    g_tracy_capture = SDL_CreateProcess(argv, false);
    free(tool);
    if (!g_tracy_capture) {
        WARN("tracy: cannot start tracy-capture: %s%s\n", SDL_GetError(), suffix);
        g_tracy_capture_path[0] = 0;
        return 0;
    }
    LOG("tracy: recording to %s\n", g_tracy_capture_path);
    return 1;
}

/* the trace opens in the profiler by itself, so that a run needs nothing but starting it */
static void tracy_open_trace(const char* path) {
    char* tool;
    const char* argv[3];
    if (!path || !*path || !g_settings.tracy_open_trace) return;
    if (g_opt_headless) return; /* no screen to open it on */
    tool = tracy_tool("tracy-profiler");
    if (!tool) return;
    argv[0] = tool;
    argv[1] = path;
    argv[2] = NULL;
    SDL_CreateProcess(argv, false);
    free(tool);
    LOG("tracy: the trace opens in the profiler\n");
}

static void tracy_capture_finish_locked(int clean) {
    int status = 0;
    bool done;
    u64 until = plat_ns() + 5000000000ull;
    if (!g_tracy_capture) return;
    /* A clean exit waits for the trace to be written; a killed one only looks once. */
    while (!(done = SDL_WaitProcess(g_tracy_capture, false, &status)) && clean && plat_ns() < until)
        plat_sleep_ns(10000000);
    if (done) {
        if (status == 0) {
            LOG("tracy: capture saved to %s\n", g_tracy_capture_path);
            tracy_open_trace(g_tracy_capture_path);
        } else
            WARN("tracy: capture did not save: %s (status %d)\n", g_tracy_capture_path, status);
        SDL_DestroyProcess(g_tracy_capture);
    } else {
        /* tracy-capture writes after the connection closes; killing it would lose the trace. */
        LOG("tracy: capture still writing to %s; it will finish after the runtime exits\n", g_tracy_capture_path);
    }
    g_tracy_capture = NULL;
    g_tracy_capture_path[0] = 0;
}

static void tracy_capture_finish(int clean) {
    tracy_capture_lock();
    tracy_capture_finish_locked(clean);
    tracy_capture_unlock();
}

static void tracy_start_client(void) {
    char info[512];
    int n;
    if (__atomic_load_n(&g_tracy_started, __ATOMIC_ACQUIRE)) return;
    ___tracy_startup_profiler();
    __atomic_store_n(&g_tracy_started, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_tracy_emit, 1, __ATOMIC_RELEASE);
    /* The name and app info are deferred until a GUI connects, so they go out as they are. */
    n = snprintf(info, sizeof info, "%s\n%s", snx_build_info(), g_tracy_title);
    __atomic_add_fetch(&g_tracy_pending, 1, __ATOMIC_ACQ_REL);
    TracyCSetThreadName("main");
    if (n > 0) TracyCAppInfo(info, (size_t)(n < (int)sizeof info ? n : (int)sizeof info - 1));
    __atomic_sub_fetch(&g_tracy_pending, 1, __ATOMIC_ACQ_REL);
    if (g_opt_tracy_capture)
        LOG("tracy: profiling this run to its capture\n");
    else
        LOG("tracy: profiling this run; connect the GUI with: tracy-profiler -a 127.0.0.1 -p %s\n",
            g_tracy_port[0] ? g_tracy_port : "8086");
}

/* Starts a capture and waits for it to connect. The capture's handshake has a couple of seconds to be answered, and
   a machine busy with a recompile can miss it: the capture gives up then, and another one takes its place. */
static int tracy_capture_wait(void) {
    int i, j;
    for (i = 0; i < 3; i++) {
        int status = 0;
        int gave_up = 0;
        if (!tracy_capture_start(SNX_TRACY_RUN_SECONDS, 1)) return 0;
        for (j = 0; j < 200; j++) {
            if (TracyCIsConnected) return 1;
            if (SDL_WaitProcess(g_tracy_capture, false, &status)) {
                gave_up = 1; /* it gave up */
                break;
            }
            plat_sleep_ns(50000000);
        }
        if (TracyCIsConnected) return 1;
        if (!gave_up) {
            /* It still holds the port, so another capture could not bind it. This one may still connect. */
            WARN("tracy: the capture did not connect in 10 s and is still running; it may connect yet\n");
            return 0;
        }
        SDL_DestroyProcess(g_tracy_capture);
        g_tracy_capture = NULL;
        g_tracy_capture_path[0] = 0;
        LOG("tracy: the capture did not connect; another one takes its place\n");
    }
    return 0;
}

/* a capture that is already running: a moment for it to connect, and the game waits that long */
static void tracy_capture_connected(void) {
    int i;
    for (i = 0; i < 20 && !TracyCIsConnected; i++) plat_sleep_ns(50000000);
    if (!TracyCIsConnected) LOG("tracy: the capture did not connect\n");
}

void snx_tracy_capture_now(void) {
    int status = 0;
    tracy_capture_lock();
    if (__atomic_load_n(&g_tracy_started, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&g_tracy_emit, __ATOMIC_ACQUIRE))
        goto out;
    if (!__atomic_load_n(&g_tracy_started, __ATOMIC_ACQUIRE)) {
        tracy_port_setup(1); /* before the client reads it, which is when it starts */
        tracy_start_client();
        if (!tracy_capture_start(SNX_TRACY_NOW_SECONDS, 0)) goto out;
        tracy_capture_connected(); /* the game waits that long for it */
    } else {
        if (g_tracy_capture) {
            if (!SDL_WaitProcess(g_tracy_capture, false, &status)) {
                LOG("tracy: a capture is already recording to %s\n", g_tracy_capture_path);
                goto out;
            }
            tracy_capture_finish_locked(1);
        }
        if (TracyCIsConnected) {
            LOG("tracy: the Tracy GUI is connected; disconnect it before starting a capture\n");
            goto out;
        }
        if (!tracy_capture_start(SNX_TRACY_NOW_SECONDS, 0)) goto out;
    }
    LOG("tracy: capture started; it stops after %d seconds\n", SNX_TRACY_NOW_SECONDS);
out:
    tracy_capture_unlock();
}

int snx_tracy_enter(void) {
    /* The count comes before the second gate read, and both are sequenced with the store shutdown closes with, so a
       thread that is about to emit is either counted before shutdown looks or sees the gate closed. */
    if (!__atomic_load_n(&g_tracy_emit, __ATOMIC_ACQUIRE)) return 0;
    __atomic_add_fetch(&g_tracy_pending, 1, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&g_tracy_emit, __ATOMIC_SEQ_CST)) {
        __atomic_sub_fetch(&g_tracy_pending, 1, __ATOMIC_SEQ_CST);
        return 0;
    }
    return 1;
}

void snx_tracy_leave(void) { __atomic_sub_fetch(&g_tracy_pending, 1, __ATOMIC_ACQ_REL); }

TracyCZoneCtx snx_tracy_zone_begin(const struct ___tracy_source_location_data* srcloc) {
    TracyCZoneCtx ctx = {0};
    if (!__atomic_load_n(&g_tracy_emit, __ATOMIC_ACQUIRE)) return ctx;
    /* No room to track a deeper zone; drop it rather than leak a begin. */
    if (t_zone_depth >= SNX_TRACY_ZONE_MAX) return ctx;
    /* A thread that is not mirroring a connection asks at every zone, so it is traced from its next one, however few
       it makes (the frontend's one a frame, the audio clock's one a tick, a thread that began before the client did).
       Attached, it looks again only every so many zones, for a disconnection. */
    if (!t_attached) {
        if (!snx_tracy_enter()) return ctx;
        t_attached = ___tracy_connected();
        if (t_attached) {
            tracy_attach_thread();
            tracy_sampling_start();
            tracy_plots_send();
        }
        snx_tracy_leave();
        if (!t_attached) return ctx;
    } else if (++t_zone_tick >= SNX_TRACY_POLL) {
        t_zone_tick = 0;
        if (!snx_tracy_enter()) return ctx;
        t_attached = ___tracy_connected();
        snx_tracy_leave();
        if (!t_attached) return ctx;
    }
    /* The count brackets the emit, so shutdown cannot end the client under it. */
    if (!snx_tracy_enter()) return ctx;
    /* the plain begin: a zone takes no callstack, the sampler's stacks are for that */
    ctx = ___tracy_emit_zone_begin(srcloc, 1);
    if (ctx.active)
        t_zones[t_zone_depth++] = ctx;
    else
        snx_tracy_leave();
    return ctx;
}

void snx_tracy_zone_end(TracyCZoneCtx ctx) {
    int i;
    if (!ctx.active) return;
    /* Shutdown ends this thread's zones itself, so the scope end that follows finds none: ending it again would emit
       a second end for it. */
    for (i = t_zone_depth - 1; i >= 0 && t_zones[i].id != ctx.id; i--) {}
    if (i < 0) return;
    if (i + 1 < t_zone_depth)
        memmove(&t_zones[i], &t_zones[i + 1], sizeof t_zones[0] * (size_t)(t_zone_depth - i - 1));
    t_zone_depth--;
    ___tracy_emit_zone_end(ctx);
    __atomic_sub_fetch(&g_tracy_pending, 1, __ATOMIC_ACQ_REL);
}

/* Remembers the name for a later attach, and sends it now when a server can see it. */
void snx_tracy_thread_name(const char* name) {
    if (!name || !*name) return;
    snprintf(t_name, sizeof t_name, "%s", name);
    if (!snx_tracy_enter()) return;
    if (TracyCIsConnected) TracyCSetThreadName(t_name);
    snx_tracy_leave();
}

/* The plots' formats, sent when a GUI attaches: the byte counts as sizes, the idle mark as steps. A second
   connection gets them again, its server knowing none of it. */
static void tracy_plots_send(void) {
    static const struct {
        const char* name;
        int type, step, fill;
    } cfg[] = {
        {"gpu memory", 1, 0, 1},   {"gpu memory budget", 1, 0, 1}, {"texture memory", 1, 0, 1},
        {"guest memory", 1, 0, 1}, {"gpu idle", 0, 1, 0},
    };
    if (!snx_tracy_enter()) return;
    if (TracyCIsConnected)
        for (size_t i = 0; i < sizeof cfg / sizeof *cfg; i++)
            ___tracy_emit_plot_config(cfg[i].name, cfg[i].type, cfg[i].step, cfg[i].fill, 0);
    snx_tracy_leave();
}

int snx_tracy_frame_start(const char* name) {
    int sent = 0;
    if (!name || !*name || !snx_tracy_enter()) return 0;
    if (TracyCIsConnected) {
        ___tracy_emit_frame_mark_start(name);
        sent = 1;
    }
    snx_tracy_leave();
    return sent;
}

void snx_tracy_frame_at_scope(SnxTracyFrame* f) {
    if (!f->started) return;
    f->started = 0;
    if (!snx_tracy_enter()) return;
    if (TracyCIsConnected) ___tracy_emit_frame_mark_end(f->name);
    snx_tracy_leave();
}

/* The GPU track's emissions, each gated on its own: the caller calls them as they come. A context is announced
   once per connection, a zone only after its context, so the server knows where the timestamps belong; a
   disconnection found on any of them re-arms the announcement. */
#define SNX_TRACY_GPU_CTX 8
static int g_gpu_sent[SNX_TRACY_GPU_CTX];
static uint16_t g_gpu_query[SNX_TRACY_GPU_CTX];

static int gpu_ctx_ok(int ctx) { return ctx >= 0 && ctx < SNX_TRACY_GPU_CTX; }

int snx_tracy_gpu_context(int ctx, int64_t gpu_time, float period, int calibrated, const char* name) {
    int announced = 0;
    if (!gpu_ctx_ok(ctx) || !snx_tracy_enter()) return 0;
    if (TracyCIsConnected) {
        if (!g_gpu_sent[ctx]) {
            struct ___tracy_gpu_new_context_data c = {gpu_time, period, (uint8_t)ctx, calibrated ? 1 : 0, 2};
            ___tracy_emit_gpu_new_context_serial(c);
            if (name && *name) {
                struct ___tracy_gpu_context_name_data n = {(uint8_t)ctx, name, (uint16_t)strlen(name)};
                ___tracy_emit_gpu_context_name_serial(n);
            }
            g_gpu_sent[ctx] = 1;
            announced = 1;
        }
    } else
        g_gpu_sent[ctx] = 0;
    snx_tracy_leave();
    return announced;
}

uint16_t snx_tracy_gpu_zone_begin(const char* name, uint32_t color, int ctx) {
    uint16_t q = 0;
    if (!gpu_ctx_ok(ctx) || !name || !*name || !snx_tracy_enter()) return 0;
    if (TracyCIsConnected && g_gpu_sent[ctx]) {
        /* one srcloc a zone: the name is the caller's, copied by the client at the allocation */
        u64 srcloc = ___tracy_alloc_srcloc_name(0, __FILE__, sizeof __FILE__ - 1, __func__, sizeof __func__ - 1, name,
                                                strlen(name), color);
        if (srcloc) {
            struct ___tracy_gpu_zone_begin_data d = {srcloc, ++g_gpu_query[ctx], (uint8_t)ctx};
            q = g_gpu_query[ctx];
            ___tracy_emit_gpu_zone_begin_alloc_serial(d);
        }
    }
    snx_tracy_leave();
    return q;
}

uint16_t snx_tracy_gpu_zone_end(uint16_t query, int ctx) {
    uint16_t q = 0;
    if (!query || !gpu_ctx_ok(ctx) || !snx_tracy_enter()) return 0;
    if (TracyCIsConnected) {
        struct ___tracy_gpu_zone_end_data d = {++g_gpu_query[ctx], (uint8_t)ctx};
        q = g_gpu_query[ctx];
        ___tracy_emit_gpu_zone_end_serial(d);
    }
    snx_tracy_leave();
    return q;
}

void snx_tracy_gpu_time(int64_t gpu_time, uint16_t query, int ctx) {
    if (!query || !gpu_ctx_ok(ctx) || !snx_tracy_enter()) return;
    if (TracyCIsConnected) {
        struct ___tracy_gpu_time_data t = {gpu_time, query, (uint8_t)ctx};
        ___tracy_emit_gpu_time_serial(t);
    }
    snx_tracy_leave();
}

void snx_tracy_gpu_sync(int64_t gpu_time, int ctx) {
    if (!gpu_ctx_ok(ctx) || !snx_tracy_enter()) return;
    if (TracyCIsConnected) {
        struct ___tracy_gpu_time_sync_data s = {gpu_time, (uint8_t)ctx};
        ___tracy_emit_gpu_time_sync_serial(s);
    } else
        g_gpu_sent[ctx] = 0;
    snx_tracy_leave();
}

void snx_tracy_gpu_calibrate(int64_t gpu_time, int64_t cpu_delta, int ctx) {
    if (!gpu_ctx_ok(ctx) || !snx_tracy_enter()) return;
    if (TracyCIsConnected) {
        struct ___tracy_gpu_calibration_data c = {gpu_time, cpu_delta, (uint8_t)ctx};
        ___tracy_emit_gpu_calibration_serial(c);
    } else
        g_gpu_sent[ctx] = 0;
    snx_tracy_leave();
}

int snx_tracy_init(const char* title) {
    snprintf(g_tracy_title, sizeof g_tracy_title, "%s", title && title[0] ? title : "ScarletNX");
    if (!g_opt_tracy_capture && !g_opt_tracy_client) return 0;
    /* The client before the capture: the capture connects once, so it has to find the client listening. The client
       reads its port as it starts, so it is set before that. */
    tracy_port_setup(g_opt_tracy_capture);
    tracy_start_client();
    if (g_opt_tracy_capture && !tracy_capture_wait()) return 1;
    return 0;
}

void snx_tracy_log(int lvl, const char* text) {
    static const int8_t severity[3] = {TracyMessageSeverityWarning, TracyMessageSeverityInfo,
                                       TracyMessageSeverityDebug};
    size_t n;
    if (!text || !snx_tracy_enter()) return;
    if (TracyCIsConnected) {
        n = strlen(text);
        while (n && (text[n - 1] == '\n' || text[n - 1] == '\r')) n--;
        if (n) ___tracy_emit_logString(severity[lvl < 0 ? 0 : lvl > 2 ? 2 : lvl], 0, 0, n, text);
    }
    snx_tracy_leave();
}

void snx_tracy_shutdown(void) {
    int i;
    if (!__atomic_load_n(&g_tracy_started, __ATOMIC_ACQUIRE)) return;
    if (__atomic_exchange_n(&g_tracy_stopped, 1, __ATOMIC_ACQ_REL)) return;
    /* Close the gate; the store is sequenced with the counts, so a thread that passed it is already counted. */
    __atomic_store_n(&g_tracy_emit, 0, __ATOMIC_SEQ_CST);
    tracy_sampling_stop();
    /* End this thread's nested zones, each releasing its own count. */
    while (t_zone_depth > 0) snx_tracy_zone_end(t_zones[t_zone_depth - 1]);
    /* Wait for other threads; if one stays in a zone, leave the client for _exit to close. */
    for (i = 0; i < 2000; i++) {
        if (!__atomic_load_n(&g_tracy_pending, __ATOMIC_ACQUIRE)) {
            ___tracy_shutdown_profiler();
            tracy_capture_finish(1);
            return;
        }
        plat_sleep_ns(1000000);
    }
    /* The socket closes at _exit; leave tracy-capture to write the trace. */
    tracy_capture_finish(0);
}
#endif
