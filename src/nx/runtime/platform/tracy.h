/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Tracy zones, plots and memory events; compiled out without SNX_TRACY. */
#ifndef SNX_TRACY_H
#define SNX_TRACY_H

#if defined(SNX_TRACY)
#include <tracy/TracyC.h>

/* Every emission passes this gate; shutdown closes it. */
int snx_tracy_enter(void);
void snx_tracy_leave(void);
TracyCZoneCtx snx_tracy_zone_begin(const struct ___tracy_source_location_data* srcloc);
void snx_tracy_zone_end(TracyCZoneCtx ctx);
static inline void snx_tracy_zone_end_at_scope(TracyCZoneCtx* ctx) { snx_tracy_zone_end(*ctx); }

/* The zone ends at the end of the block it opens in, a return included; one that ends earlier calls SNX_ZONE_END. */
#define SNX_ZONE(ctx, name)                                                                                            \
    static const struct ___tracy_source_location_data TracyConcat(snx_tracy_source_, TracyLine) =                     \
        {name, __func__, TracyFile, (uint32_t)TracyLine, 0};                                                           \
    TracyCZoneCtx ctx __attribute__((cleanup(snx_tracy_zone_end_at_scope))) =                                          \
        snx_tracy_zone_begin(&TracyConcat(snx_tracy_source_, TracyLine))
/* a short emission: gated, and skipped when nothing is attached */
#define SNX_TRACY_EMIT(...)                                                                                            \
    do {                                                                                                               \
        if (snx_tracy_enter()) {                                                                                       \
            if (TracyCIsConnected) __VA_ARGS__;                                                                        \
            snx_tracy_leave();                                                                                         \
        }                                                                                                              \
    } while (0)
#define SNX_ZONE_TEXT(ctx, ...) SNX_TRACY_EMIT(TracyCZoneTextF(ctx, __VA_ARGS__))
#define SNX_ZONE_VALUE(ctx, value) SNX_TRACY_EMIT(TracyCZoneValue(ctx, (uint64_t)(value)))
/* a zone whose text or value is known as it opens */
#define SNX_ZONE_T(ctx, name, ...) SNX_ZONE(ctx, name); SNX_ZONE_TEXT(ctx, __VA_ARGS__)
#define SNX_ZONE_V(ctx, name, value) SNX_ZONE(ctx, name); SNX_ZONE_VALUE(ctx, value)
#define SNX_ZONE_TV(ctx, name, value, ...) SNX_ZONE_V(ctx, name, value); SNX_ZONE_TEXT(ctx, __VA_ARGS__)
#define SNX_ZONE_END(ctx) snx_tracy_zone_end(ctx)
#define SNX_MEM_ALLOC(ptr, size, pool) SNX_TRACY_EMIT(TracyCAllocN((const void*)(ptr), (size_t)(size), (pool)))
#define SNX_MEM_FREE(ptr, pool) SNX_TRACY_EMIT(TracyCFreeN((const void*)(ptr), (pool)))
#define SNX_FRAME_MARK() SNX_TRACY_EMIT(TracyCFrameMark)
#define SNX_PLOT(name, value) SNX_TRACY_EMIT(TracyCPlot(name, value))
/* the name goes into the client's table, which the server asks for when a GUI connects; kept per thread, so a
   thread that named itself before the client started is named again when it attaches */
#define SNX_THREAD_NAME(name) snx_tracy_thread_name(name)
#else
#define SNX_ZONE(ctx, name)      int ctx __attribute__((unused)) = 0
#define SNX_ZONE_TEXT(ctx, ...)  ((void)0)
#define SNX_ZONE_VALUE(ctx, value) ((void)0)
#define SNX_ZONE_T(ctx, name, ...) int ctx __attribute__((unused)) = 0
#define SNX_ZONE_V(ctx, name, value) int ctx __attribute__((unused)) = 0
#define SNX_ZONE_TV(ctx, name, value, ...) int ctx __attribute__((unused)) = 0
#define SNX_ZONE_END(ctx)        ((void)0)
#define SNX_MEM_ALLOC(ptr, size, pool) ((void)0)
#define SNX_MEM_FREE(ptr, pool) ((void)0)
#define SNX_FRAME_MARK()        ((void)0)
#define SNX_PLOT(name, value)   ((void)0)
#define SNX_THREAD_NAME(name)   ((void)0)
#endif

/* Starts the client when --tracy or --tracy-capture asks for it. Returns 1 when the capture it was asked for is not
   running; the run goes on without one. */
#if defined(SNX_TRACY)
int snx_tracy_init(const char* title);
void snx_tracy_thread_name(const char* name);
/* F7: starts the client if needed, then a 60-second capture. */
void snx_tracy_capture_now(void);
/* 0 warning, 1 log, 2 verbose. */
void snx_tracy_log(int lvl, const char* text);
/* Closes the client cleanly before plat_exit_now. */
void snx_tracy_shutdown(void);
#else
/* Stubs for tests that link plat.c without tracy.c. */
static inline int snx_tracy_init(const char* title) {
    (void)title;
    return 0;
}
static inline void snx_tracy_capture_now(void) {}
static inline void snx_tracy_log(int lvl, const char* text) {
    (void)lvl;
    (void)text;
}
static inline void snx_tracy_shutdown(void) {}
#endif

#endif
