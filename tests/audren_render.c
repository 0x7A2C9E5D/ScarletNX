/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The audio renderer's DSP offline: updates written field by field as edenprobe's audio family writes them
   (revision 4, 48 kHz, 240 samples a frame), frames rendered, and what the aux, the circular buffer sinks and the
   voices' counters show. Levels are one bin of a Fourier transform over a frame of whole periods */
#include "hle/services/audren_dsp.c"
#include <math.h>
#include <stdio.h>

u8* g_mem;
int g_verbose;
void logf_(int lvl, const char* fmt, ...) {
    (void)lvl;
    (void)fmt;
}

static unsigned checks, failures;
#define CHECK(expr, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(expr)) {                                                                                                 \
            failures++;                                                                                                \
            fprintf(stderr, "audren_render.c:%d: %s: ", __LINE__, #expr);                                              \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

enum {
    RATE = 48000,
    FRAME = 240,
    VOICES = 4,
    EFFECTS = 2,
    SINKS = 3,
    MIXES = 2,
    FINAL = 0,
    SUBMIX = 1,
    RING_FRAMES = 40,
    AUX_COUNT = RING_FRAMES * 2 * FRAME,
    /* guest addresses: offsets into the arena g_mem points at */
    G_ARENA = 8 << 20,
    G_PCM = 0x10000,    /* one second of a tone per voice, VOICES of them */
    G_ADPCM = 0x200000, /* an ADPCM buffer and its coefficients */
    G_AUX_SEND = 0x300000,
    G_AUX_RETURN = 0x340000,
    G_RING = 0x380000, /* the circular buffer sinks' rings, 0x20000 apiece */
    G_RING_BYTES = RING_FRAMES * 2 * FRAME * 2,
};

/* the update, laid out as edenprobe's source/audio.c lays it out */
static u8
    in[0x40 + 0x10 + 0x20 + 0x70 * VOICES + 0x170 * VOICES + 0xC0 * EFFECTS + 0x930 * MIXES + 0x140 * SINKS + 0x10];
static u8 out[0x1000];
static u8 *u_pool, *u_res, *u_voice, *u_eff, *u_mix, *u_sink;
static ARState* st;

static void w8(u8* p, u32 off, u8 v) { p[off] = v; }
static void w32(u8* p, u32 off, u32 v) { memcpy(p + off, &v, 4); }
static void w64(u8* p, u32 off, u64 v) { memcpy(p + off, &v, 8); }
static void wf(u8* p, u32 off, float v) { memcpy(p + off, &v, 4); }
static u8* voice(int i) { return u_voice + i * 0x170; }
static u8* res(int i) { return u_res + i * 0x70; }
static u8* eff(int i) { return u_eff + i * 0xC0; }
static u8* mix(int i) { return u_mix + i * 0x930; }
static u8* sink(int i) { return u_sink + i * 0x140; }

static void layout(void) {
    memset(in, 0, sizeof in);
    u32 rev = ('R' << 0) | ('E' << 8) | ('V' << 16) | ((u32)('0' + 4) << 24);
    u32 h[16] = {rev, 0x10, 0x20, 0x170 * VOICES, 0x70 * VOICES, 0xC0 * EFFECTS, 0x930 * MIXES, 0x140 * SINKS, 0x10};
    h[15] = sizeof in;
    memcpy(in, h, sizeof h);
    w32(in, 0x40, rev);
    u8* p = in + 0x50;
    u_pool = p;
    p += 0x20;
    u_res = p;
    p += 0x70 * VOICES;
    u_voice = p;
    p += 0x170 * VOICES;
    u_eff = p;
    p += 0xC0 * EFFECTS;
    u_mix = p;
    p += 0x930 * MIXES;
    u_sink = p;
    w64(u_pool, 0, 0);
    w64(u_pool, 8, G_ARENA);
    w32(u_pool, 0x10, 4); /* asked to attach */
    for (int i = 0; i < VOICES; i++) {
        w32(res(i), 0, (u32)i);
        w32(voice(i), 0, (u32)i);
    }
    for (int m = 0; m < MIXES; m++) {
        wf(mix(m), 0, 1.0f);
        w32(mix(m), 4, RATE);
        w32(mix(m), 8, m == FINAL ? 2u : 4u);
        w8(mix(m), 0xC, 1);
        w32(mix(m), 0x10, (u32)m);
        w32(mix(m), 0x924, m == FINAL ? 0x7FFFFFFFu : FINAL);
        w32(mix(m), 0x928, 0xFFFFFFFFu);
    }
    /* the limiter, off until a case turns it on, then the aux, both on the submix */
    w8(eff(0), 0, AR_EFFECT_LIMITER);
    w8(eff(0), 1, 1);
    w32(eff(0), 4, SUBMIX);
    w32(eff(0), 0x18, 0);
    u8* l = eff(0) + 0x20;
    l[0] = 0, l[1] = 1, l[6] = 0, l[7] = 1;
    l[0xC] = 2, l[0xE] = 2;
    w32(l, 0x10, RATE);
    w32(l, 0x38, 48);
    w32(l, 0x3C, 48);
    wf(l, 0x30, 1.0f);
    wf(l, 0x34, 1.0f);
    wf(l, 0x2C, 1.0f);
    wf(l, 0x24, 0.001f);
    wf(l, 0x28, 0.001f);
    w8(l, 0x40, 2);
    w8(eff(1), 0, AR_EFFECT_AUX);
    w8(eff(1), 1, 1);
    w32(eff(1), 4, SUBMIX);
    w32(eff(1), 0x18, 1);
    u8* a = eff(1) + 0x20;
    a[0] = 0, a[1] = 1, a[0x18] = 2, a[0x19] = 3;
    w32(a, 0x30, 2);
    w32(a, 0x34, RATE);
    w32(a, 0x38, AUX_COUNT);
    w32(a, 0x3C, 2);
    w64(a, 0x40, G_AUX_SEND);
    w64(a, 0x48, G_AUX_SEND + 0x80);
    w64(a, 0x50, G_AUX_RETURN);
    w64(a, 0x58, G_AUX_RETURN + 0x80);
    w32(a, 0x60, 4);
    w32(a, 0x64, FRAME);
    w32(a, 0x68, FRAME);
    /* the device sink on the final mix's two buffers, then two circular buffer sinks off until a case needs one */
    w8(sink(0), 0, AR_SINK_DEVICE);
    w8(sink(0), 1, 1);
    w32(sink(0), 0x20 + 0x100, 2);
    sink(0)[0x20 + 0x104] = 0, sink(0)[0x20 + 0x105] = 1;
    for (int k = 1; k < SINKS; k++) {
        u8* c = sink(k) + 0x20;
        w8(sink(k), 0, AR_SINK_CIRCULAR);
        w64(c, 0, G_RING + (u64)(k - 1) * 0x20000);
        w32(c, 8, G_RING_BYTES);
        w32(c, 0xC, 2);
        w32(c, 0x10, FRAME);
        w8(c, 0x18, 2);
        c[0x1C] = 0, c[0x1D] = 1;
    }
}

static void update(void) {
    int rc = audren_state_update(st, in, sizeof in, out, sizeof out);
    CHECK(rc == 0, "update returned %d", rc);
    for (int i = 0; i < VOICES; i++) {
        w8(voice(i), 8, 0);                                                  /* is_new once */
        for (int w = 0; w < 4; w++) w8(voice(i), 0x60 + w * 0x38 + 0x1A, 1); /* sent */
    }
    for (int i = 0; i < EFFECTS; i++) w8(eff(i), 1, 0);
}
static void frames(int n) {
    i16 stereo[FRAME * 2];
    for (int i = 0; i < n; i++) {
        update();
        audren_frame(st, stereo);
    }
}

/* a tone of whole periods in each frame, looping over one second of PCM16 at 48 kHz */
static void tone(int v, double hz, double amp, u8 quality, int channel, float gain) {
    i16* pcm = (i16*)(void*)(g_mem + G_PCM + (u64)v * RATE * 2);
    for (int i = 0; i < RATE; i++) pcm[i] = (i16)lrint(amp * 32767.0 * sin(2.0 * M_PI * hz * i / RATE));
    u8* vi = voice(v);
    memset(vi + 8, 0, 0x170 - 8);
    w8(vi, 8, 1);
    w8(vi, 9, 1);
    w8(vi, 0xA, 0); /* started */
    w8(vi, 0xB, AR_PCM16);
    w32(vi, 0xC, RATE);
    w32(vi, 0x18, 1);
    wf(vi, 0x1C, 1.0f);
    wf(vi, 0x20, 1.0f);
    w32(vi, 0x3C, 1);
    w32(vi, 0x58, SUBMIX);
    w32(vi, 0x5C, 0xFFFFFFFFu);
    w64(vi, 0x60, G_PCM + (u64)v * RATE * 2);
    w64(vi, 0x68, RATE * 2);
    w32(vi, 0x70, 0);
    w32(vi, 0x74, RATE);
    w8(vi, 0x78, 1);            /* loop */
    w32(vi, 0x7C, 0xFFFFFFFFu); /* forever */
    w32(vi, 0x140, (u32)v);
    w8(vi, 0x15E, quality);
    for (int w = 1; w < 4; w++)
        w8(vi, 0x60 + w * 0x38 + 0x1A, 1); /* the slots it does not use: sent, as clients mark them */
    memset(res(v) + 4, 0, 0x60);
    wf(res(v), 4 + channel * 4, gain);
    w8(res(v), 0x64, 1);
}
static void silence(int v) { w8(voice(v), 0xA, 1); /* stopped */ }

/* one Fourier bin over n samples, as a fraction of full scale */
static double level_i32(const i32* x, int n, double hz) {
    double re = 0, im = 0;
    for (int i = 0; i < n; i++) {
        re += x[i] * cos(2.0 * M_PI * hz * i / RATE);
        im -= x[i] * sin(2.0 * M_PI * hz * i / RATE);
    }
    return 2.0 * sqrt(re * re + im * im) / n / 32768.0;
}
static double level_i16(const i16* x, int n, double hz) {
    i32 tmp[FRAME];
    for (int i = 0; i < n; i++) tmp[i] = x[i];
    return level_i32(tmp, n, hz);
}
/* the aux ring's channel ch in its most recent full frame */
static double captured(int ch, double hz) {
    u32 woff;
    memcpy(&woff, g_mem + G_AUX_SEND + 0x40 + 4, 4);
    u32 frame_at = (woff + AUX_COUNT - 2 * FRAME) % AUX_COUNT;
    const i32* ring = (const i32*)(const void*)(g_mem + G_AUX_SEND + 0x80);
    return level_i32(ring + frame_at + ch * FRAME, FRAME, hz);
}

static void reset(void) {
    audren_state_free(st);
    u8 params[0x34] = {0};
    u32 p[13] = {RATE, FRAME, 6, MIXES - 1, VOICES, SINKS, EFFECTS};
    memcpy(params, p, sizeof p);
    st = audren_state_new(params);
    CHECK(st != NULL, "no state");
    memset(g_mem, 0, G_ARENA);
    layout();
}

int main(void) {
    g_mem = calloc(1, G_ARENA);
    if (!g_mem) return 1;

    /* the resampler at a ratio of one: an 11 kHz voice keeps its level at default quality as at high */
    for (u8 q = 0; q < 2; q++) {
        reset();
        w8(eff(1), 2, 1); /* aux on */
        tone(0, 11000, 0.5, q, 0, 1.0f);
        frames(100);
        double lv = captured(0, 11000);
        CHECK(fabs(lv - 0.5) < 0.005, "quality %u: 11 kHz at 0.5 came out at %.4f", q, lv);
    }

    /* a stereo circular buffer sink records the final mix, the left channel first and the right second, and its
       write offset moves */
    reset();
    tone(0, 1000, 0.25, 0, 0, 1.0f);
    tone(1, 1600, 0.25, 0, 1, 1.0f);
    wf(mix(SUBMIX), 0x24, 1.0f);                /* submix 0 -> final 0 */
    wf(mix(SUBMIX), 0x24 + (24 + 1) * 4, 1.0f); /* submix 1 -> final 1 */
    w8(sink(1), 1, 1);
    frames(50);
    const i16* ring = (const i16*)(const void*)(g_mem + G_RING);
    double l1k = level_i16(ring, FRAME, 1000), l16 = level_i16(ring, FRAME, 1600);
    double r1k = level_i16(ring + FRAME, FRAME, 1000), r16 = level_i16(ring + FRAME, FRAME, 1600);
    CHECK(fabs(l1k - 0.25) < 0.005 && l16 < 0.005, "left channel: 1 kHz %.4f, 1.6 kHz %.4f", l1k, l16);
    CHECK(fabs(r16 - 0.25) < 0.005 && r1k < 0.005, "right channel: 1.6 kHz %.4f, 1 kHz %.4f", r16, r1k);
    u32 woff;
    update();
    memcpy(&woff, out + 0x40 + 0x10 + VOICES * 0x10 + EFFECTS * 0x10 + 0x20, 4);
    CHECK(woff != 0 && woff % (2 * FRAME * 2) == 0, "write offset %u", woff);

    /* the limiter switched off keeps a tone's level; in mode 1 the tone comes out at threshold over the detector:
       1 kHz at 0.25, threshold 0.1, rates 0.01, gives 0.25 * 0.1 / (0.25 * 2 / pi) */
    for (int mode = -1; mode <= 1; mode++) {
        reset();
        w8(eff(1), 2, 1);
        tone(0, 1000, 0.25, 0, 0, 1.0f);
        u8* l = eff(0) + 0x20;
        w8(eff(0), 2, mode >= 0);
        w8(eff(0), 1, 1);
        w8(l, 0x43, (u8)(mode < 0 ? 0 : mode));
        wf(l, 0x2C, 0.1f);
        wf(l, 0x24, 0.01f);
        wf(l, 0x28, 0.01f);
        frames(200);
        double lv = captured(0, 1000), rule = 0.25 * 0.1 / (0.25 * 2.0 / M_PI);
        if (mode < 0)
            CHECK(fabs(lv - 0.25) < 0.003, "limiter off: %.4f", lv);
        else
            CHECK(fabs(lv - rule) / rule < 0.03, "limiter mode %d: %.4f, the rule gives %.4f", mode, lv, rule);
    }

    /* a voice's counters: 480 samples played as two frames consume their buffer, and the next update says so */
    reset();
    tone(0, 1000, 0.25, 0, 0, 1.0f);
    w32(voice(0), 0x74, 480);
    w8(voice(0), 0x78, 0); /* no loop */
    frames(3);
    update();
    u64 played;
    u32 consumed;
    memcpy(&played, out + 0x40 + 0x10, 8);
    memcpy(&consumed, out + 0x40 + 0x10 + 8, 4);
    CHECK(played == 480 && consumed == 1, "played %llu, consumed %u", (unsigned long long)played, consumed);
    /* a stop consumes what was queued and starts the count over */
    tone(1, 1000, 0.25, 0, 0, 1.0f);
    frames(1);
    silence(1);
    frames(2);
    update();
    memcpy(&played, out + 0x40 + 0x10 + 0x10, 8);
    memcpy(&consumed, out + 0x40 + 0x10 + 0x10 + 8, 4);
    CHECK(played == 0 && consumed == 1, "after a stop: played %llu, consumed %u", (unsigned long long)played, consumed);

    /* DSP-ADPCM: the high nibble first, each code times 2^scale plus the prediction from the last two samples */
    reset();
    ARVoice v = {0};
    ARChannel c = {0};
    ARWave w = {0};
    i16 coef[16] = {0};
    coef[2] = 2048; /* pair 1: the last sample, times 1.0 */
    memcpy(g_mem + G_ADPCM + 0x100, coef, sizeof coef);
    u8 frame0[8] = {0x02, 0x12, 0x34, 0x56, 0x7F, 0xED, 0xCB, 0xA9}; /* pair 0, scale 2 */
    u8 frame1[8] = {0x10, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11}; /* pair 1, scale 0: +1 each sample */
    memcpy(g_mem + G_ADPCM, frame0, 8);
    memcpy(g_mem + G_ADPCM + 8, frame1, 8);
    v.format = AR_ADPCM;
    v.channels = 1;
    v.data = G_ADPCM + 0x100;
    v.data_size = 32;
    w.addr = G_ADPCM;
    w.size = 16;
    w.end = 28;
    i16 got[28];
    u32 n = decode_adpcm(&v, &c, &w, 0, 28, 0, 28, got);
    static const i16 want0[14] = {4, 8, 12, 16, 20, 24, 28, -4, -8, -12, -16, -20, -24, -28};
    CHECK(n == 28, "decoded %u", n);
    CHECK(!memcmp(got, want0, sizeof want0), "first frame %d %d ... %d", got[0], got[1], got[13]);
    CHECK(got[14] == -27 && got[27] == -14, "second frame %d ... %d", got[14], got[27]);

    printf("%u checks, %u failed\n", checks, failures);
    audren_state_free(st);
    free(g_mem);
    return failures != 0;
}
