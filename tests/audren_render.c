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
    SNX_ART_RATE = 48000,
    SNX_ART_FRAME = 240,
    SNX_ART_VOICES = 4,
    SNX_ART_EFFECTS = 2,
    SNX_ART_SINKS = 3,
    SNX_ART_MIXES = 2,
    SNX_ART_FINAL = 0,
    SNX_ART_SUBMIX = 1,
    SNX_ART_RING_FRAMES = 40,
    SNX_ART_AUX_COUNT = SNX_ART_RING_FRAMES * 2 * SNX_ART_FRAME,
    /* guest addresses: offsets into the arena g_mem points at */
    SNX_ART_G_ARENA = 8 << 20,
    SNX_ART_G_PCM = 0x10000,    /* one second of a tone per voice, SNX_ART_VOICES of them */
    SNX_ART_G_ADPCM = 0x200000, /* an ADPCM buffer and its coefficients */
    SNX_ART_G_AUX_SEND = 0x300000,
    SNX_ART_G_AUX_RETURN = 0x340000,
    SNX_ART_G_RING = 0x380000, /* the circular buffer sinks' rings, 0x20000 apiece */
    SNX_ART_G_RING_BYTES = SNX_ART_RING_FRAMES * 2 * SNX_ART_FRAME * 2,
    /* the splitter section: room for a header, two infos of three destinations and six destinations */
    SNX_ART_SPLITTERS = 2,
    SNX_ART_DESTS = 6,
    SNX_ART_SPLIT_BYTES = 0x20 + SNX_ART_SPLITTERS * (0x1C + 3 * 4) + SNX_ART_DESTS * 0x70,
};

/* the update, laid out as edenprobe's source/audio.c lays it out */
static u8 in[0x40 + 0x10 + 0x20 + 0x70 * SNX_ART_VOICES + 0x170 * SNX_ART_VOICES + 0xC0 * SNX_ART_EFFECTS +
             SNX_ART_SPLIT_BYTES + 0x930 * SNX_ART_MIXES + 0x140 * SNX_ART_SINKS + 0x10];
static u8 out[0x1000];
static u8 *u_pool, *u_res, *u_voice, *u_eff, *u_split, *u_mix, *u_sink;
static u32 split_counts[2]; /* the splitters and destinations reset() opens the renderer with */
static u32 art_rev = 4;     /* the revision layout() writes */
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
    u32 rev = ('R' << 0) | ('E' << 8) | ('V' << 16) | ((u32)('0' + art_rev) << 24);
    u32 h[16] = {rev,
                 0x10,
                 0x20,
                 0x170 * SNX_ART_VOICES,
                 0x70 * SNX_ART_VOICES,
                 0xC0 * SNX_ART_EFFECTS,
                 0x930 * SNX_ART_MIXES,
                 0x140 * SNX_ART_SINKS,
                 0x10,
                 SNX_ART_SPLIT_BYTES};
    h[15] = sizeof in;
    memcpy(in, h, sizeof h);
    w32(in, 0x40, rev);
    u8* p = in + 0x50;
    u_pool = p;
    p += 0x20;
    u_res = p;
    p += 0x70 * SNX_ART_VOICES;
    u_voice = p;
    p += 0x170 * SNX_ART_VOICES;
    u_eff = p;
    p += 0xC0 * SNX_ART_EFFECTS;
    u_split = p; /* all zero, no splitter, until a case writes one */
    p += SNX_ART_SPLIT_BYTES;
    u_mix = p;
    p += 0x930 * SNX_ART_MIXES;
    u_sink = p;
    w64(u_pool, 0, 0);
    w64(u_pool, 8, SNX_ART_G_ARENA);
    w32(u_pool, 0x10, 4); /* asked to attach */
    for (int i = 0; i < SNX_ART_VOICES; i++) {
        w32(res(i), 0, (u32)i);
        w32(voice(i), 0, (u32)i);
    }
    for (int m = 0; m < SNX_ART_MIXES; m++) {
        wf(mix(m), 0, 1.0f);
        w32(mix(m), 4, SNX_ART_RATE);
        w32(mix(m), 8, m == SNX_ART_FINAL ? 2u : 4u);
        w8(mix(m), 0xC, 1);
        w32(mix(m), 0x10, (u32)m);
        w32(mix(m), 0x924, m == SNX_ART_FINAL ? 0x7FFFFFFFu : SNX_ART_FINAL);
        w32(mix(m), 0x928, 0xFFFFFFFFu);
    }
    /* the limiter, off until a case turns it on, then the aux, both on the submix */
    w8(eff(0), 0, SNX_AUDREN_EFFECT_LIMITER);
    w8(eff(0), 1, 1);
    w32(eff(0), 4, SNX_ART_SUBMIX);
    w32(eff(0), 0x18, 0);
    u8* l = eff(0) + 0x20;
    l[0] = 0, l[1] = 1, l[6] = 0, l[7] = 1;
    l[0xC] = 2, l[0xE] = 2;
    w32(l, 0x10, SNX_ART_RATE);
    w32(l, 0x38, 48);
    w32(l, 0x3C, 48);
    wf(l, 0x30, 1.0f);
    wf(l, 0x34, 1.0f);
    wf(l, 0x2C, 1.0f);
    wf(l, 0x24, 0.001f);
    wf(l, 0x28, 0.001f);
    w8(l, 0x40, 2);
    w8(eff(1), 0, SNX_AUDREN_EFFECT_AUX);
    w8(eff(1), 1, 1);
    w32(eff(1), 4, SNX_ART_SUBMIX);
    w32(eff(1), 0x18, 1);
    u8* a = eff(1) + 0x20;
    a[0] = 0, a[1] = 1, a[0x18] = 2, a[0x19] = 3;
    w32(a, 0x30, 2);
    w32(a, 0x34, SNX_ART_RATE);
    w32(a, 0x38, SNX_ART_AUX_COUNT);
    w32(a, 0x3C, 2);
    w64(a, 0x40, SNX_ART_G_AUX_SEND);
    w64(a, 0x48, SNX_ART_G_AUX_SEND + 0x80);
    w64(a, 0x50, SNX_ART_G_AUX_RETURN);
    w64(a, 0x58, SNX_ART_G_AUX_RETURN + 0x80);
    w32(a, 0x60, 4);
    w32(a, 0x64, SNX_ART_FRAME);
    w32(a, 0x68, SNX_ART_FRAME);
    /* the device sink on the final mix's two buffers, then two circular buffer sinks off until a case needs one */
    w8(sink(0), 0, SNX_AUDREN_SINK_DEVICE);
    w8(sink(0), 1, 1);
    w32(sink(0), 0x20 + 0x100, 2);
    sink(0)[0x20 + 0x104] = 0, sink(0)[0x20 + 0x105] = 1;
    for (int k = 1; k < SNX_ART_SINKS; k++) {
        u8* c = sink(k) + 0x20;
        w8(sink(k), 0, SNX_AUDREN_SINK_CIRCULAR);
        w64(c, 0, SNX_ART_G_RING + (u64)(k - 1) * 0x20000);
        w32(c, 8, SNX_ART_G_RING_BYTES);
        w32(c, 0xC, 2);
        w32(c, 0x10, SNX_ART_FRAME);
        w8(c, 0x18, 2);
        c[0x1C] = 0, c[0x1D] = 1;
    }
}

static void update(void) {
    int rc = audren_state_update(st, in, sizeof in, out, sizeof out);
    CHECK(rc == 0, "update returned %d", rc);
    for (int i = 0; i < SNX_ART_VOICES; i++) {
        w8(voice(i), 8, 0);                                                  /* is_new once */
        for (int w = 0; w < 4; w++) w8(voice(i), 0x60 + w * 0x38 + 0x1A, 1); /* sent */
    }
    for (int i = 0; i < SNX_ART_EFFECTS; i++) w8(eff(i), 1, 0);
}
/* a frame with no update before it */
static void frame_only(void) {
    i16 stereo[SNX_ART_FRAME * 2];
    audren_frame(st, stereo);
}
static void frames(int n) {
    i16 stereo[SNX_ART_FRAME * 2];
    for (int i = 0; i < n; i++) {
        update();
        audren_frame(st, stereo);
    }
}

/* a tone of whole periods in each frame, looping over one second of PCM16 at 48 kHz */
static void tone(int v, double hz, double amp, u8 quality, int channel, float gain) {
    i16* pcm = (i16*)(void*)(g_mem + SNX_ART_G_PCM + (u64)v * SNX_ART_RATE * 2);
    for (int i = 0; i < SNX_ART_RATE; i++) pcm[i] = (i16)lrint(amp * 32767.0 * sin(2.0 * M_PI * hz * i / SNX_ART_RATE));
    u8* vi = voice(v);
    memset(vi + 8, 0, 0x170 - 8);
    w8(vi, 8, 1);
    w8(vi, 9, 1);
    w8(vi, 0xA, 0); /* started */
    w8(vi, 0xB, SNX_AUDREN_PCM16);
    w32(vi, 0xC, SNX_ART_RATE);
    w32(vi, 0x18, 1);
    wf(vi, 0x1C, 1.0f);
    wf(vi, 0x20, 1.0f);
    w32(vi, 0x3C, 1);
    w32(vi, 0x58, SNX_ART_SUBMIX);
    w32(vi, 0x5C, 0xFFFFFFFFu);
    w64(vi, 0x60, SNX_ART_G_PCM + (u64)v * SNX_ART_RATE * 2);
    w64(vi, 0x68, SNX_ART_RATE * 2);
    w32(vi, 0x70, 0);
    w32(vi, 0x74, SNX_ART_RATE);
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

/* voice v in stereo: hz0 on its left channel (resource v), hz1 on its right (resource v + 1), interleaved */
static void tone_stereo(int v, double hz0, double hz1, double amp) {
    tone(v, hz0, amp, 0, 0, 1.0f);
    i16* pcm = (i16*)(void*)(g_mem + SNX_ART_G_PCM + (u64)v * SNX_ART_RATE * 2);
    for (int i = 0; i < SNX_ART_RATE; i++)
        for (int c = 0; c < 2; c++)
            pcm[i * 2 + c] = (i16)lrint(amp * 32767.0 * sin(2.0 * M_PI * (c ? hz1 : hz0) * i / SNX_ART_RATE));
    w32(voice(v), 0x18, 2);
    w64(voice(v), 0x68, SNX_ART_RATE * 4);
    w32(voice(v), 0x144, (u32)v + 1);
    w8(res(v + 1), 0x64, 1);
}

/* a splitter section written in order: the header, then infos, then destinations */
static u32 split_at;
static void split_begin(u32 infos, u32 dests) {
    memset(u_split, 0, SNX_ART_SPLIT_BYTES);
    memcpy(u_split, "SNDH", 4);
    w32(u_split, 4, infos);
    w32(u_split, 8, dests);
    split_at = 0x20;
}
static void split_info(u32 id, u32 n, const u32* dests) {
    memcpy(u_split + split_at, "SNDI", 4);
    w32(u_split, split_at + 4, id);
    w32(u_split, split_at + 8, SNX_ART_RATE);
    w32(u_split, split_at + 0xC, n);
    for (u32 k = 0; k < n; k++) w32(u_split, split_at + 0x10 + k * 4, dests[k]);
    split_at += 0x1C + n * 4;
}
/* destination id to mix, volume v0 into its buffer 0 and v1 into its buffer 1 */
static u8* split_sent[SNX_ART_DESTS];
static void split_send(u32 id, u32 mix_id, u8 in_use, float v0, float v1) {
    u8* d = u_split + split_at;
    memcpy(d, "SNDD", 4);
    w32(d, 4, id);
    wf(d, 8, v0);
    wf(d, 0xC, v1);
    w32(d, 0x68, mix_id);
    w8(d, 0x6C, in_use);
    split_sent[id] = d;
    split_at += 0x70;
}

/* one Fourier bin over n samples, as a fraction of full scale */
static double level_i32(const i32* x, int n, double hz) {
    double re = 0, im = 0;
    for (int i = 0; i < n; i++) {
        re += x[i] * cos(2.0 * M_PI * hz * i / SNX_ART_RATE);
        im -= x[i] * sin(2.0 * M_PI * hz * i / SNX_ART_RATE);
    }
    return 2.0 * sqrt(re * re + im * im) / n / 32768.0;
}
static double level_i16(const i16* x, int n, double hz) {
    i32 tmp[SNX_ART_FRAME];
    for (int i = 0; i < n; i++) tmp[i] = x[i];
    return level_i32(tmp, n, hz);
}
/* the left (ch 0) or right channel the first circular buffer sink wrote in frame f since the reset */
static const i16* sink_frame_at(int f, int ch) {
    return (const i16*)(const void*)(g_mem + SNX_ART_G_RING) + (f % SNX_ART_RING_FRAMES) * 2 * SNX_ART_FRAME +
           ch * SNX_ART_FRAME;
}

/* a 1 kHz voice at 0.3 alone on the final mix's left, recorded by the first circular buffer sink */
static void bq_voice(void) {
    tone(0, 1000, 0.3, 0, 0, 1.0f);
    w32(voice(0), 0x58, SNX_ART_FINAL);
    w8(sink(1), 1, 1);
}
static void bq_set(int f, u8 on, const i16* b, const i16* a) {
    u8* bq = voice(0) + 0x24 + f * 0xC;
    w8(bq, 0, on);
    memcpy(bq + 2, b, 6);
    memcpy(bq + 8, a, 4);
}
/* Eden's ApplyBiquadFilterInt, or ApplyBiquadFilterFloat when dbl, over n samples from a clear state */
static void ref_biquad(const i16* in, i16* out, int n, const i16* b, const i16* a, int dbl) {
    i64 s0 = 0, s1 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (int i = 0; i < n; i++) {
        if (dbl) {
            double x = in[i], y = x * (b[0] / 16384.0) + x1 * (b[1] / 16384.0) + x2 * (b[2] / 16384.0) +
                                  y1 * (a[0] / 16384.0) + y2 * (a[1] / 16384.0);
            out[i] = (i16)(i32)y;
            x2 = x1, x1 = x, y2 = y1, y1 = y;
        } else {
            i64 x = in[i], y = (x * b[0] + s0 + (1 << 13)) >> 14;
            out[i] = (i16)y;
            s0 = s1 + b[1] * x + a[0] * y;
            s1 = b[2] * x + a[1] * y;
        }
    }
}

/* a 1 kHz voice on the submix, its buffers 0 and 1 to the final mix, effect 0 on the submix */
static u8* fx_voice(u8 type) {
    tone(0, 1000, 0.3, 0, 0, 1.0f);
    wf(mix(SNX_ART_SUBMIX), 0x24, 1.0f);
    wf(mix(SNX_ART_SUBMIX), 0x24 + (24 + 1) * 4, 1.0f);
    w8(sink(1), 1, 1);
    memset(eff(1), 0, 0xC0); /* no aux after it */
    u8* e = eff(0);
    memset(e, 0, 0xC0);
    w8(e, 0, type);
    w8(e, 1, 1);
    w32(e, 4, SNX_ART_SUBMIX);
    return e + 0x20;
}
/* frames from..to - 1 of the sink's left (ch 0) or right channel into x, from x[0] */
static void sink_copy(i16* x, int from, int to, int ch) {
    for (int f = from; f < to; f++) memcpy(x + (f - from) * SNX_ART_FRAME, sink_frame_at(f, ch), SNX_ART_FRAME * 2);
}
/* the submix's buffer from into the final mix's buffer to at gain g */
static void route(int from, int to, float g) { wf(mix(SNX_ART_SUBMIX), 0x24 + (from * 24 + to) * 4, g); }
/* a delay on effect 0: channels, time in ms, 48 kHz, then gains in Q14 */
static void delay_set(u8* p, u16 channels, u32 ms, i32 in_gain, i32 fb, i32 wet, i32 dry, i32 spread, i32 lp) {
    w32(p, 0xC, (u32)channels << 16 | channels);
    w32(p, 0x10, 2 * ms);
    w32(p, 0x14, ms);
    w32(p, 0x18, (u32)SNX_ART_RATE << 14);
    const i32 g[6] = {in_gain, fb, wet, dry, spread, lp};
    memcpy(p + 0x1C, g, sizeof g);
}
/* Eden's InitializeDelayEffect, SetDelayEffectParameter and ApplyDelay, one or two channels */
static void ref_delay(int nch, const i16* const* in, i16* const* out, int n, int len, i32 in_gain, i32 fb, i32 wet,
                      i32 dry, i32 spread, i32 lp) {
    static i64 line[2][4096];
    i64 z[2] = {0, 0};
    memset(line, 0, sizeof line);
    i32 fbs = (i32)(((i64)fb * 16056) >> 14), dfg = (i32)(((i64)fbs * (16384 - spread)) >> 14),
        cross = (i32)(((i64)spread * fbs) >> 14), lpf = (i32)(((i64)lp * 15564) >> 14), lpg = 16384 - lpf;
    i32 m[2][2] = {{nch == 1 ? fbs : dfg, cross}, {cross, dfg}};
    for (int i = 0, pos = 0; i < n; i++, pos = (pos + 1) % len) {
        i64 x[2], d[2];
        for (int c = 0; c < nch; c++) x[c] = (i64)in[c][i] * 64 * 16384, d[c] = line[c][pos];
        for (int c = 0; c < nch; c++) {
            i64 fed = 0;
            for (int j = 0; j < nch; j++) fed += (d[j] * m[j][c]) >> 14;
            z[c] = (((((x[c] * in_gain) >> 14) + fed) * lpg) >> 14) + ((z[c] * lpf) >> 14);
            line[c][pos] = z[c];
        }
        for (int c = 0; c < nch; c++)
            out[c][i] = (i16)((i32)((((x[c] * dry) >> 14) + ((d[c] * wet) >> 14)) >> 14) / 64);
    }
}

/* Eden's FixedPoint<50, 14>, as the reverb's expected values use it */
static i64 tq(float f) { return (i64)(f * 16384.0f); }
static i64 tmul(i64 a, i64 b) { return (i64)(((__int128)a * b) >> 14); }
static i64 tdiv(i64 a, i64 b) { return (i64)(((__int128)a * 16384) / b); }
static i32 tint(i64 q) { return (i32)((q + ((q & 0x3FFF) >> 1)) >> 14); }
static i16 tsat(i32 v) { return (i16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* a reverb on effect 0 at 48 kHz: channels, then v from 0x14 in Q14 */
static void reverb_set(u8* p, u16 ch, const i32* v) {
    w32(p, 0xC, (u32)ch << 16 | ch);
    w32(p, 0x10, 48 << 14);
    memcpy(p + 0x14, v, 11 * 4);
}

/* Eden's reverb tables (renderer/command/effect/reverb.cpp), mode 0 to 4 */
static const float rv_early_t[5][11] = {
    {0.0f, 3.5f, 2.799988f, 3.899963f, 2.699951f, 13.399963f, 7.899963f, 8.399963f, 9.899963f, 12.0f, 12.5f},
    {0.0f, 11.799988f, 5.5f, 11.199951f, 10.399963f, 38.099976f, 22.199951f, 29.599976f, 21.199951f, 24.799988f, 40.0f},
    {0.0f, 41.5f, 20.5f, 41.299988f, 0.0f, 29.5f, 33.799988f, 45.199951f, 46.799988f, 0.0f, 50.0f},
    {33.099976f, 43.299988f, 22.799988f, 37.899963f, 14.899963f, 35.299988f, 17.899963f, 34.199951f, 0.0f, 43.299988f,
     50.0f},
    {0}};
static const float rv_early_g[5][10] = {
    {0.699951f, 0.679993f, 0.699951f, 0.679993f, 0.699951f, 0.679993f, 0.699951f, 0.679993f, 0.679993f, 0.679993f},
    {0.699951f, 0.679993f, 0.699951f, 0.679993f, 0.699951f, 0.679993f, 0.679993f, 0.679993f, 0.679993f, 0.679993f},
    {0.5f, 0.699951f, 0.699951f, 0.679993f, 0.5f, 0.679993f, 0.679993f, 0.699951f, 0.679993f, 0.0f},
    {0.929993f, 0.919983f, 0.869995f, 0.859985f, 0.939941f, 0.809998f, 0.799988f, 0.769958f, 0.759949f, 0.649963f},
    {0}};
static const float rv_fdn_t[5][4] = {{53.953247f, 79.192566f, 116.238770f, 130.615295f},
                                     {53.953247f, 79.192566f, 116.238770f, 170.615295f},
                                     {5.0f, 10.0f, 5.0f, 10.0f},
                                     {47.029968f, 71.0f, 103.0f, 170.0f},
                                     {53.953247f, 79.192566f, 116.238770f, 170.615295f}};
static const float rv_decay_t[5][4] = {{7.0f, 9.0f, 13.0f, 17.0f},
                                       {7.0f, 9.0f, 13.0f, 17.0f},
                                       {1.0f, 1.0f, 1.0f, 1.0f},
                                       {7.0f, 7.0f, 13.0f, 9.0f},
                                       {7.0f, 9.0f, 13.0f, 17.0f}};
static const float rv_fdn_max[4] = {53.9532470703125f, 79.19256591796875f, 116.23876953125f, 170.61529541015625f},
                   rv_decay_max[4] = {7.0f, 9.0f, 13.0f, 17.0f};

/* the early taps right after InitializeReverbEffect: each update moves them a quarter of the way */
static void rv_first_taps(int mode, i32 early_gain, i32 pre, i32* t, i64* g) {
    for (int k = 0; k < 10; k++) {
        i32 target = tint(tmul(pre + tq(rv_early_t[mode][k]), 48 << 14));
        if (target > 16800) target = 16800;
        t[k] = (target + 1) / 4;
        g[k] = tdiv(tmul(early_gain, tq(rv_early_g[mode][k])), 4 * 16384);
    }
}
/* tap k of input x at sample i: x 1 + t[k] samples back, as the pre-delay line holds it (64 times, at base gain 1) */
static i64 rv_tap(const i32* x, int i, i32 t, i64 g) {
    int at = i - 1 - t;
    return at < 0 ? 0 : tmul((i64)x[at] * 16384 * 64, g);
}

/* Eden's reverb for one channel (renderer/command/effect/reverb.cpp), its lines as arrays */
typedef struct {
    i64* b;
    int end, in, out, count, max;
    i64 decay;
} RvLine;
typedef struct {
    RvLine pre, fdn[4], decay[4];
    i32 t[10], pre_t;
    i64 g[10], hf_gain[4], hf_prev[4], fb[4], cosine;
} RvRef;
static void rv_line(RvLine* l, int len, i64 decay) {
    l->b = calloc((size_t)len + 1, sizeof *l->b);
    l->end = l->count = l->max = len;
    l->in = l->out = 0;
    l->decay = decay;
}
static i64 rv_tick(RvLine* l, i64 v) {
    i64 r = l->b[l->out];
    if (++l->out >= l->end) l->out = 0;
    l->b[l->in] = v;
    if (++l->in >= l->end) l->in = 0;
    return r;
}
static i64 rv_tapout(const RvLine* l, int idx) {
    int at = l->in - (idx + 1);
    return l->b[at < 0 ? at + l->count : at];
}
static float rv_pow10(float v) { return v >= 0.0f ? 1.0f : v <= -5.3f ? 0.0f : powf(10.0f, v); }
/* UpdateReverbEffectParameter, v as reverb_set takes it, all in range (the clamps left out) */
static void rv_update(RvRef* r, const i32* v) {
    int em = v[0], lm = v[3];
    i64 rate = 48 << 14, pre = v[2];
    for (int k = 0; k < 10; k++) {
        i32 target = tint(tmul(pre + tq(rv_early_t[em][k]), rate)), old = r->t[k] - 1;
        if (target > r->pre.max) target = r->pre.max;
        r->t[k] = old + (target - old) / 4 + 1;
        i64 gain = tmul(v[1], tq(rv_early_g[em][k]));
        r->g[k] = r->g[k] + tdiv(gain - r->g[k], 4 * 16384);
    }
    i32 target = tint(tmul(pre + tq(rv_early_t[em][10]), rate));
    if (target > r->pre.max) target = r->pre.max;
    r->pre_t = r->pre_t + (target - r->pre_t) / 4;
    r->cosine = tq(cosf((float)tdiv(1280 * 16384, rate) / 16384.0f * 3.14159265358979f / 180.0f));
    for (int i = 0; i < 4; i++) {
        RvLine *f = &r->fdn[i], *d = &r->decay[i];
        i32 tf = tint(tmul(tq(rv_fdn_t[lm][i]), rate)), td = tint(tmul(tq(rv_decay_t[lm][i]), rate));
        f->count = f->count + ((tf > f->max ? f->max : tf) - f->count) / 4;
        f->end = f->count - 1;
        d->count = d->count + ((td > d->max ? d->max : td) - d->count) / 4;
        d->end = d->count - 1;
        d->decay = d->decay + tdiv(tmul(9830, 16384 - v[7]) - d->decay, 4 * 16384); /* 0.5999755859375 */
        i64 a = (i64)(f->max + d->max) * 16384 * -3, b = tdiv(a, tmul(v[5], rate)), c = 0, dd = 16384;
        float hf = (float)v[6] / 16384.0f;
        if (hf <= 0.9949f) {
            float e = rv_pow10((float)tmul(tq((1.0f / hf - 1.0f) * 2 / 100), tdiv(b, 10 * 16384)) / 16384.0f);
            if (e < 0.0001f) e = 0.0001f;
            float ff = 1.0f - e, gg = 2.0f - (float)r->cosine / 16384.0f * e * 2, h2 = gg * gg - ff * ff * 4;
            c = tq((gg - sqrtf(h2 < 0 ? 0 : h2)) / (ff * 2.0f));
            dd = 16384 - c;
        }
        r->hf_prev[i] = r->hf_prev[i] + tdiv(c - r->hf_prev[i], 4 * 16384);
        i64 gain = tmul(tmul(tq(rv_pow10((float)tdiv(b, 1000 * 16384) / 16384.0f)), dd), 11585); /* 0.70709228515625 */
        r->hf_gain[i] = r->hf_gain[i] + tdiv(gain - r->hf_gain[i], 4 * 16384);
        r->fb[i] = 0;
    }
}
/* InitializeReverbEffect, then ApplyReverbEffect<1> over x into y */
static void rv_ref(const i16* x, i16* y, int frames_n, const u8* state, const i32* v) {
    static RvRef r;
    for (int f = 0; f < frames_n; f++) {
        if (state[f] == SNX_AUDREN_PARAM_INITIALIZED) {
            free(r.pre.b);
            for (int i = 0; i < 4; i++) free(r.fdn[i].b), free(r.decay[i].b);
            memset(&r, 0, sizeof r);
            for (int i = 0; i < 4; i++) {
                rv_line(&r.fdn[i], (int)(tmul(tq(rv_fdn_max[i]), 48 << 14) >> 14), 16384);
                rv_line(&r.decay[i], (int)(tmul(tq(rv_decay_max[i]), 48 << 14) >> 14), 0);
            }
            rv_line(&r.pre, 16800, 16384);
            rv_update(&r, v);
        } else if (state[f] == SNX_AUDREN_PARAM_UPDATING)
            rv_update(&r, v);
        for (int i = f * SNX_ART_FRAME; i < (f + 1) * SNX_ART_FRAME; i++) {
            i64 o = 0, ap[4];
            for (int k = 0; k < 10; k++) o += tmul(rv_tapout(&r.pre, r.t[k]), r.g[k]);
            r.pre.b[r.pre.in] = tmul((i64)x[i] * 16384 * 64, v[8]);
            if (++r.pre.in >= r.pre.end) r.pre.in = 0;
            for (int k = 0; k < 4; k++)
                r.fb[k] = tmul(r.fb[k], r.hf_prev[k]) + tmul(r.fdn[k].b[r.fdn[k].out], r.hf_gain[k]);
            i64 pd = tmul(rv_tapout(&r.pre, r.pre_t), v[4]);
            i64 mix[4] = {r.fb[2] + r.fb[1] + pd, -r.fb[0] - r.fb[3] + pd, r.fb[0] - r.fb[3] + pd,
                          r.fb[1] - r.fb[2] + pd};
            for (int k = 0; k < 4; k++) {
                i64 mixed = mix[k] - tmul(r.decay[k].b[r.decay[k].out], r.decay[k].decay);
                ap[k] = rv_tick(&r.decay[k], mixed) + tmul(mixed, r.decay[k].decay);
                rv_tick(&r.fdn[k], ap[k]);
            }
            y[i] = tsat(tint(tmul((i64)x[i] * 16384, v[10]) + tdiv(tmul(o + ap[0], v[9]), 64 * 16384)));
        }
    }
}

/* the first sample where a and b differ, -1 if none */
static int first_diff(const i16* a, const i16* b, int n) {
    for (int i = 0; i < n; i++)
        if (a[i] != b[i]) return i;
    return -1;
}

/* the aux ring's channel ch in its most recent full frame */
static double captured(int ch, double hz) {
    u32 woff;
    memcpy(&woff, g_mem + SNX_ART_G_AUX_SEND + 0x40 + 4, 4);
    u32 frame_at = (woff + SNX_ART_AUX_COUNT - 2 * SNX_ART_FRAME) % SNX_ART_AUX_COUNT;
    const i32* ring = (const i32*)(const void*)(g_mem + SNX_ART_G_AUX_SEND + 0x80);
    return level_i32(ring + frame_at + ch * SNX_ART_FRAME, SNX_ART_FRAME, hz);
}

static void reset(void) {
    audren_state_free(st);
    u8 params[0x34] = {0};
    u32 p[13] = {SNX_ART_RATE, SNX_ART_FRAME, 8, SNX_ART_MIXES - 1, SNX_ART_VOICES, SNX_ART_SINKS, SNX_ART_EFFECTS};
    p[9] = split_counts[0], p[10] = split_counts[1]; /* splitter infos 0x24, destinations 0x28 */
    memcpy(params, p, sizeof p);
    st = audren_state_new(params);
    CHECK(st != NULL, "no state");
    memset(g_mem, 0, SNX_ART_G_ARENA);
    layout();
}

int main(void) {
    g_mem = calloc(1, SNX_ART_G_ARENA);
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
    wf(mix(SNX_ART_SUBMIX), 0x24, 1.0f);                /* submix 0 -> final 0 */
    wf(mix(SNX_ART_SUBMIX), 0x24 + (24 + 1) * 4, 1.0f); /* submix 1 -> final 1 */
    w8(sink(1), 1, 1);
    frames(50);
    const i16* ring = (const i16*)(const void*)(g_mem + SNX_ART_G_RING);
    double l1k = level_i16(ring, SNX_ART_FRAME, 1000), l16 = level_i16(ring, SNX_ART_FRAME, 1600);
    double r1k = level_i16(ring + SNX_ART_FRAME, SNX_ART_FRAME, 1000),
           r16 = level_i16(ring + SNX_ART_FRAME, SNX_ART_FRAME, 1600);
    CHECK(fabs(l1k - 0.25) < 0.005 && l16 < 0.005, "left channel: 1 kHz %.4f, 1.6 kHz %.4f", l1k, l16);
    CHECK(fabs(r16 - 0.25) < 0.005 && r1k < 0.005, "right channel: 1.6 kHz %.4f, 1 kHz %.4f", r16, r1k);
    u32 woff;
    update();
    memcpy(&woff, out + 0x40 + 0x10 + SNX_ART_VOICES * 0x10 + SNX_ART_EFFECTS * 0x10 + 0x20, 4);
    CHECK(woff != 0 && woff % (2 * SNX_ART_FRAME * 2) == 0, "write offset %u", woff);

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

    /* the resampler tables made from their windowed sincs, against the console's: phase 0 of default quality's
       pass-through table and of high quality's, and one phase in the middle */
    reset();
    static const int lut4_1[4] = {-68, 32639, 69, -5}, lut8_2[8] = {-12, 47, -134, 32767, 81, -16, 2, 0},
                     lut4_0_64[4] = {1221, 15064, 15185, 1266};
    for (int i = 0; i < 4; i++) {
        CHECK(fabs(st->lut4[1][i] * 32768.0 - lut4_1[i]) <= 1.0, "lut4[1] tap %d: %.1f", i, st->lut4[1][i] * 32768.0);
        CHECK(fabs(st->lut4[0][64 * 4 + i] * 32768.0 - lut4_0_64[i]) <= 1.0, "lut4[0] phase 64 tap %d", i);
    }
    for (int i = 0; i < 8; i++)
        CHECK(fabs(st->lut8[2][i] * 32768.0 - lut8_2[i]) <= 1.0, "lut8[2] tap %d: %.1f", i, st->lut8[2][i] * 32768.0);

    /* DSP-ADPCM: the high nibble first, each code times 2^scale plus the prediction from the last two samples */
    reset();
    ARVoice v = {0};
    ARChannel c = {0};
    ARWave w = {0};
    i16 coef[16] = {0};
    coef[2] = 2048; /* pair 1: the last sample, times 1.0 */
    memcpy(g_mem + SNX_ART_G_ADPCM + 0x100, coef, sizeof coef);
    u8 frame0[8] = {0x02, 0x12, 0x34, 0x56, 0x7F, 0xED, 0xCB, 0xA9}; /* pair 0, scale 2 */
    u8 frame1[8] = {0x10, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11}; /* pair 1, scale 0: +1 each sample */
    memcpy(g_mem + SNX_ART_G_ADPCM, frame0, 8);
    memcpy(g_mem + SNX_ART_G_ADPCM + 8, frame1, 8);
    v.format = SNX_AUDREN_ADPCM;
    v.channels = 1;
    v.data = SNX_ART_G_ADPCM + 0x100;
    v.data_size = 32;
    w.addr = SNX_ART_G_ADPCM;
    w.size = 16;
    w.end = 28;
    i16 got[28];
    u32 n = decode_adpcm(&v, &c, &w, 0, 28, 0, 28, got);
    static const i16 want0[14] = {4, 8, 12, 16, 20, 24, 28, -4, -8, -12, -16, -20, -24, -28};
    CHECK(n == 28, "decoded %u", n);
    CHECK(!memcmp(got, want0, sizeof want0), "first frame %d %d ... %d", got[0], got[1], got[13]);
    CHECK(got[14] == -27 && got[27] == -14, "second frame %d ... %d", got[14], got[27]);

    /* splitters (Eden's GenerateVoiceCommand): a stereo voice with no mix through destinations c, c + 2, ... */
    split_counts[0] = SNX_ART_SPLITTERS, split_counts[1] = SNX_ART_DESTS;
    reset();
    tone_stereo(0, 1000, 1600, 0.4);
    w32(voice(0), 0x58, SNX_AUDREN_UNUSED_MIX);
    w32(voice(0), 0x5C, 0);
    split_begin(1, 3);
    split_info(0, 3, (const u32[]){0, 1, 2});
    split_send(0, SNX_ART_FINAL, 1, 1.0f, 0.0f);
    split_send(1, SNX_ART_FINAL, 1, 0.0f, 0.5f);
    split_send(2, SNX_ART_FINAL, 0, 0.0f, 1.0f);
    w8(sink(1), 1, 1);
    frames(4);
    {
        double ll = level_i16(sink_frame_at(3, 0), SNX_ART_FRAME, 1000),
               lr = level_i16(sink_frame_at(3, 0), SNX_ART_FRAME, 1600),
               rr = level_i16(sink_frame_at(3, 1), SNX_ART_FRAME, 1600),
               rl = level_i16(sink_frame_at(3, 1), SNX_ART_FRAME, 1000);
        CHECK(fabs(ll - 0.4) < 0.005 && lr < 0.005, "splitter, left: 1 kHz %.4f, 1.6 kHz %.4f", ll, lr);
        CHECK(fabs(rr - 0.2) < 0.005 && rl < 0.005, "splitter, right: 1.6 kHz %.4f, 1 kHz %.4f", rr, rl);
    }

    /* a submix with no destination mix through splitter 1: destination k takes buffer k mod 2 */
    reset();
    tone(0, 1000, 0.4, 0, 0, 1.0f);
    tone(1, 1600, 0.4, 0, 1, 1.0f);
    w32(mix(SNX_ART_SUBMIX), 8, 2);
    wf(mix(SNX_ART_SUBMIX), 0, 0.5f);
    w32(mix(SNX_ART_SUBMIX), 0x924, SNX_AUDREN_UNUSED_MIX);
    w32(mix(SNX_ART_SUBMIX), 0x928, 1);
    split_begin(1, 3);
    split_info(1, 3, (const u32[]){3, 4, 5});
    split_send(3, SNX_ART_FINAL, 1, 1.0f, 0.0f);
    split_send(4, SNX_ART_FINAL, 1, 0.5f, 1.0f);
    split_send(5, SNX_ART_FINAL, 1, 0.0f, 0.25f);
    w8(sink(1), 1, 1);
    frames(4);
    {
        double ll = level_i16(sink_frame_at(3, 0), SNX_ART_FRAME, 1000),
               lr = level_i16(sink_frame_at(3, 0), SNX_ART_FRAME, 1600),
               rr = level_i16(sink_frame_at(3, 1), SNX_ART_FRAME, 1600),
               rl = level_i16(sink_frame_at(3, 1), SNX_ART_FRAME, 1000);
        CHECK(fabs(ll - 0.2) < 0.004 && fabs(lr - 0.1) < 0.004, "submix splitter, left: %.4f %.4f", ll, lr);
        CHECK(fabs(rr - 0.2) < 0.004 && fabs(rl - 0.05) < 0.004, "submix splitter, right: %.4f %.4f", rr, rl);
    }

    /* a destination's volume from 1 to 0 ramps over the next frame */
    reset();
    tone(0, 1000, 0.5, 0, 0, 1.0f);
    for (int i = 0; i < SNX_ART_RATE; i++) ((i16*)(void*)(g_mem + SNX_ART_G_PCM))[i] = 16384;
    w32(voice(0), 0x58, SNX_AUDREN_UNUSED_MIX);
    w32(voice(0), 0x5C, 0);
    split_begin(1, 1);
    split_info(0, 1, (const u32[]){0});
    split_send(0, SNX_ART_FINAL, 1, 1.0f, 0.0f);
    w8(sink(1), 1, 1);
    frames(3);
    wf(split_sent[0], 8, 0.0f);
    frames(2);
    {
        const i16 *steady = sink_frame_at(2, 0), *ramp = sink_frame_at(3, 0), *after = sink_frame_at(4, 0);
        double worst = 0;
        int loud = 0;
        for (int i = 0; i < SNX_ART_FRAME; i++) {
            double d = fabs(ramp[i] - steady[0] * (1.0 - i / (double)SNX_ART_FRAME));
            if (d > worst) worst = d;
            loud += after[i] != 0;
        }
        CHECK(steady[0] > 16000 && steady[SNX_ART_FRAME - 1] == steady[0], "steady %d %d", steady[0], steady[239]);
        CHECK(worst <= 1.0, "the ramp is off by %.1f", worst);
        CHECK(loud == 0, "%d samples after the ramp", loud);
    }

    /* a submix sending to itself is a cycle: the sort falls back, and the final mix still plays */
    reset();
    tone(0, 1000, 0.4, 0, 0, 1.0f);
    tone(1, 1600, 0.4, 0, 0, 1.0f);
    w32(voice(1), 0x58, SNX_ART_FINAL);
    w32(mix(SNX_ART_SUBMIX), 0x924, SNX_AUDREN_UNUSED_MIX);
    w32(mix(SNX_ART_SUBMIX), 0x928, 0);
    split_begin(1, 1);
    split_info(0, 1, (const u32[]){0});
    split_send(0, SNX_ART_SUBMIX, 1, 1.0f, 0.0f);
    w8(sink(1), 1, 1);
    frames(4);
    {
        double lr = level_i16(sink_frame_at(3, 0), SNX_ART_FRAME, 1600);
        CHECK(fabs(lr - 0.4) < 0.005 && (st->warned & SNX_AUDREN_WARN_CYCLE), "cycle: 1.6 kHz %.4f", lr);
    }
    split_counts[0] = split_counts[1] = 0;

    /* voice biquads against Eden's recursion, integers at revision 4 and doubles at 7, chained, then reset */
    static const i16 bq0b[3] = {4096, 8192, 4096}, bq0a[2] = {8192, -4096}, bq1b[3] = {8192, 0, 0}, bq1a[2] = {0, 0};
    for (art_rev = 4; art_rev <= 7; art_rev += 3) {
        static i16 plain[5 * SNX_ART_FRAME], got[5 * SNX_ART_FRAME], want[5 * SNX_ART_FRAME], mid[5 * SNX_ART_FRAME];
        int dbl = art_rev >= 7, nn = 5 * SNX_ART_FRAME;
        reset();
        bq_voice();
        frames(5);
        for (int f = 0; f < 5; f++) memcpy(plain + f * SNX_ART_FRAME, sink_frame_at(f, 0), SNX_ART_FRAME * 2);
        for (int mode = 0; mode < 3; mode++) {
            reset();
            bq_voice();
            bq_set(0, 1, bq0b, bq0a);
            if (mode == 1) bq_set(1, 1, bq1b, bq1a);
            for (int f = 0; f < 5; f++) {
                if (mode == 2) w8(voice(0), 0x24, f != 2);
                frames(1);
                memcpy(got + f * SNX_ART_FRAME, sink_frame_at(f, 0), SNX_ART_FRAME * 2);
            }
            if (mode == 2) {
                ref_biquad(plain, want, 2 * SNX_ART_FRAME, bq0b, bq0a, dbl);
                memcpy(want + 2 * SNX_ART_FRAME, plain + 2 * SNX_ART_FRAME, SNX_ART_FRAME * 2);
                ref_biquad(plain + 3 * SNX_ART_FRAME, want + 3 * SNX_ART_FRAME, 2 * SNX_ART_FRAME, bq0b, bq0a, dbl);
            } else {
                ref_biquad(plain, mode ? mid : want, nn, bq0b, bq0a, dbl);
                if (mode) ref_biquad(mid, want, nn, bq1b, bq1a, dbl);
            }
            int bad = 0, first = -1;
            for (int i = 0; i < nn; i++)
                if (got[i] != want[i]) bad++, first = first < 0 ? i : first;
            CHECK(bad == 0, "biquad rev %u mode %d: %d samples off, first %d: %d, want %d", art_rev, mode, bad, first,
                  first < 0 ? 0 : got[first], first < 0 ? 0 : want[first]);
        }
    }
    art_rev = 4;

    /* the biquad filter effect against Eden's recursion, at revisions 4 and 9, through updates and off */
    for (art_rev = 4; art_rev <= 9; art_rev += 5) {
        static i16 left[5 * SNX_ART_FRAME], right[5 * SNX_ART_FRAME], want[5 * SNX_ART_FRAME];
        i16 qb[3] = {4096, 8192, 4096}, qa[2] = {8192, -4096};
        if (art_rev >= 9) qb[1] = 6553, qa[1] = -4915;
        for (int mode = 0; mode < 3; mode++) {
            reset();
            u8* p = fx_voice(SNX_AUDREN_EFFECT_BIQUAD);
            w8(eff(0), 2, mode != 2);
            p[0] = 0, p[6] = 1;
            if (art_rev >= 9) {
                const float fb[3] = {0.25f, 0.4f, 0.25f}, fa[2] = {0.5f, -0.3f};
                memcpy(p + 0x10, fb, sizeof fb);
                memcpy(p + 0x1C, fa, sizeof fa);
                w8(p, 0x24, 1);
            } else {
                memcpy(p + 0xC, qb, sizeof qb);
                memcpy(p + 0x12, qa, sizeof qa);
                w8(p, 0x16, 1);
            }
            u32 st_at = art_rev >= 9 ? 0x25 : 0x17;
            for (int f = 0; f < 5; f++) {
                w8(p, st_at, f == 0 ? 0 : f == 2 && mode == 1 ? 1 : 2);
                if (f == 1 && mode == 0)
                    frame_only();
                else
                    frames(1);
            }
            sink_copy(left, 0, 5, 0);
            sink_copy(right, 0, 5, 1);
            int restart = mode == 1 && art_rev < 6 ? 2 * SNX_ART_FRAME : 5 * SNX_ART_FRAME;
            if (mode == 2)
                memcpy(want, left, sizeof want);
            else {
                ref_biquad(left, want, restart, qb, qa, art_rev >= 7);
                ref_biquad(left + restart, want + restart, 5 * SNX_ART_FRAME - restart, qb, qa, art_rev >= 7);
            }
            int at = first_diff(right, want, 5 * SNX_ART_FRAME);
            CHECK(at < 0, "biquad effect rev %u mode %d: sample %d is %d, want %d", art_rev, mode, at,
                  at < 0 ? 0 : right[at], at < 0 ? 0 : want[at]);
        }
    }
    art_rev = 4;

    /* the delay: a mono 10 ms line, an invalid count until frame 5, restarts on initialized and is_new */
    {
        static i16 x[12 * SNX_ART_FRAME], y[12 * SNX_ART_FRAME];
        reset();
        u8* p = fx_voice(SNX_AUDREN_EFFECT_DELAY);
        w8(eff(0), 2, 1);
        p[0] = 0, p[6] = 1;
        delay_set(p, 1, 10, 16384, 0, 16384, 0, 0, 0);
        for (int f = 0; f < 12; f++) {
            w8(p, 0xE, f < 5 ? 3 : 1);
            w8(p, 0x34, f == 2 || f == 5 ? SNX_AUDREN_PARAM_INITIALIZED : SNX_AUDREN_PARAM_UPDATED);
            if (f == 8) w8(eff(0), 1, 1);
            frames(1);
        }
        sink_copy(x, 0, 12, 0);
        sink_copy(y, 0, 12, 1);
        int bad = -1;
        for (int i = 0; i < 12 * SNX_ART_FRAME && bad < 0; i++) {
            int fresh = i < 480 || (i >= 5 * SNX_ART_FRAME && i < 7 * SNX_ART_FRAME) ||
                        (i >= 8 * SNX_ART_FRAME && i < 10 * SNX_ART_FRAME);
            if (y[i] != (fresh ? 0 : x[i - 480])) bad = i;
        }
        CHECK(bad < 0, "delay of 480: sample %d is %d", bad, bad < 0 ? 0 : y[bad]);
    }

    /* a stereo delay with every parameter, against Eden's recursion over the same input */
    {
        static i16 x0[10 * SNX_ART_FRAME], x1[10 * SNX_ART_FRAME], y0[10 * SNX_ART_FRAME], y1[10 * SNX_ART_FRAME],
            w0[10 * SNX_ART_FRAME], w1[10 * SNX_ART_FRAME];
        for (int on = 0; on < 2; on++) {
            reset();
            u8* p = fx_voice(SNX_AUDREN_EFFECT_DELAY);
            tone(1, 1600, 0.3, 0, 1, 1.0f);
            route(0, 0, 0.0f), route(1, 1, 0.0f), route(2, 0, 1.0f), route(3, 1, 1.0f);
            w8(eff(0), 2, (u8)on);
            p[0] = 0, p[1] = 1, p[6] = 2, p[7] = 3;
            delay_set(p, 2, 5, 12288, 8192, 9830, 6553, 4915, 4096);
            for (int f = 0; f < 10; f++) {
                frames(1);
                w8(p, 0x34, SNX_AUDREN_PARAM_UPDATED);
            }
            sink_copy(on ? y0 : x0, 0, 10, 0);
            sink_copy(on ? y1 : x1, 0, 10, 1);
        }
        double lx = level_i16(x0 + 9 * SNX_ART_FRAME, SNX_ART_FRAME, 1000);
        CHECK(fabs(lx - 0.3) < 0.005, "delay off copies its input: 1 kHz at %.4f", lx);
        const i16* xs[2] = {x0, x1};
        i16* ws[2] = {w0, w1};
        ref_delay(2, xs, ws, 10 * SNX_ART_FRAME, 240, 12288, 8192, 9830, 6553, 4915, 4096);
        int a = first_diff(y0, w0, 10 * SNX_ART_FRAME), b = first_diff(y1, w1, 10 * SNX_ART_FRAME);
        CHECK(a < 0 && b < 0, "stereo delay: left off at %d (%d, want %d), right at %d (%d, want %d)", a,
              a < 0 ? 0 : y0[a], a < 0 ? 0 : w0[a], b, b < 0 ? 0 : y1[b], b < 0 ? 0 : w1[b]);
    }

    /* six channels: buffer 5 is the back right's line from revision 11, the LFE's before it */
    for (art_rev = 4; art_rev <= 11; art_rev += 7) {
        reset();
        u8* p = fx_voice(SNX_AUDREN_EFFECT_DELAY);
        tone(0, 1000, 0.3, 0, 5, 1.0f);
        w32(mix(SNX_ART_SUBMIX), 8, 6);
        route(0, 0, 0.0f), route(1, 1, 0.0f), route(5, 0, 1.0f);
        w8(eff(0), 2, 1);
        for (int c = 0; c < 6; c++) p[c] = p[6 + c] = (u8)c;
        delay_set(p, 6, 5, 16384, 8192, 16384, 0, 0, 0);
        for (int f = 0; f < 40; f++) {
            frames(1);
            w8(p, 0x34, SNX_AUDREN_PARAM_UPDATED);
        }
        double lv = level_i16(sink_frame_at(39, 0), SNX_ART_FRAME, 1000),
               want = 0.3 / (1.0 - (art_rev >= 11 ? 8028.0 / 16384.0 : 0.5));
        CHECK(fabs(lv - want) < 0.004, "six-channel delay rev %u: %.4f, want %.4f", art_rev, lv, want);
    }
    art_rev = 4;

    /* the reverb's dry path alone: to_int(x * 0.5) rounds up from two thirds */
    {
        static i16 x[5 * SNX_ART_FRAME], y[5 * SNX_ART_FRAME];
        static const i32 v[11] = {0, 0, 0, 0, 0, 24576, 8192, 0, 16384, 0, 8192};
        reset();
        u8* p = fx_voice(SNX_AUDREN_EFFECT_REVERB);
        w8(eff(0), 2, 1);
        p[0] = 0, p[6] = 1;
        reverb_set(p, 1, v);
        for (int f = 0; f < 5; f++) {
            frames(1);
            w8(p, 0x40, SNX_AUDREN_PARAM_UPDATED);
        }
        sink_copy(x, 0, 5, 0);
        sink_copy(y, 0, 5, 1);
        int bad = -1;
        for (int i = 0; i < 5 * SNX_ART_FRAME && bad < 0; i++)
            if (y[i] != tint((i64)x[i] * 8192)) bad = i;
        CHECK(bad < 0, "reverb dry: sample %d is %d for %d", bad, bad < 0 ? 0 : y[bad], bad < 0 ? 0 : x[bad]);
    }

    /* a mono reverb with every parameter, a hundred frames, against Eden's run here */
    {
        static i16 x[100 * SNX_ART_FRAME], y[100 * SNX_ART_FRAME], want[100 * SNX_ART_FRAME];
        static const i32 v[11] = {1, 13107, 81920, 0, 11469, 24576, 8192, 4915, 14746, 13107, 9830};
        u8 state[100];
        reset();
        u8* p = fx_voice(SNX_AUDREN_EFFECT_REVERB);
        tone(0, 1000, 0.2, 0, 0, 1.0f);
        w8(eff(0), 2, 1);
        p[0] = 0, p[6] = 1;
        reverb_set(p, 1, v);
        for (int f = 0; f < 100; f++) {
            state[f] = f == 0 ? 0 : f == 2 || f == 3 ? 1 : 2;
            w8(p, 0x40, state[f]);
            frames(1);
            if (f % SNX_ART_RING_FRAMES == SNX_ART_RING_FRAMES - 1 || f == 99) { /* before the sink's ring laps */
                int from = f - f % SNX_ART_RING_FRAMES;
                sink_copy(x + from * SNX_ART_FRAME, from, f + 1, 0);
                sink_copy(y + from * SNX_ART_FRAME, from, f + 1, 1);
            }
        }
        rv_ref(x, want, 100, state, v);
        int at = first_diff(y, want, 100 * SNX_ART_FRAME);
        CHECK(at < 0, "mono reverb: sample %d is %d, want %d", at, at < 0 ? 0 : y[at], at < 0 ? 0 : want[at]);
    }

    /* early reflections only, right after it starts: each tap a quarter of the way to Eden's */
    {
        static i16 x0[5 * SNX_ART_FRAME], x1[5 * SNX_ART_FRAME], y0[5 * SNX_ART_FRAME], y1[5 * SNX_ART_FRAME],
            w0[5 * SNX_ART_FRAME], w1[5 * SNX_ART_FRAME];
        static i32 xs[5 * SNX_ART_FRAME];
        static const i32 v[11] = {0, 16384, 0, 0, 0, 24576, 8192, 0, 16384, 16384, 0};
        static const u8 side[10] = {0, 0, 1, 1, 0, 1, 0, 0, 1, 1};
        for (int run = 0; run < 2; run++) {
            reset();
            u8* p = fx_voice(SNX_AUDREN_EFFECT_REVERB);
            tone(1, 1600, 0.3, 0, 1, 1.0f);
            if (run) route(0, 0, 0.0f), route(1, 1, 0.0f), route(2, 0, 1.0f), route(3, 1, 1.0f);
            w8(eff(0), 2, 1);
            p[0] = 0, p[1] = 1, p[6] = 2, p[7] = 3;
            reverb_set(p, 2, v);
            for (int f = 0; f < 5; f++) {
                frames(1);
                w8(p, 0x40, SNX_AUDREN_PARAM_UPDATED);
            }
            sink_copy(run ? y0 : x0, 0, 5, 0);
            sink_copy(run ? y1 : x1, 0, 5, 1);
        }
        i32 t[10];
        i64 g[10];
        rv_first_taps(0, 16384, 0, t, g);
        g[4] = tmul(g[4], 8192), g[5] = tmul(g[5], 8192);
        for (int i = 0; i < 5 * SNX_ART_FRAME; i++) xs[i] = x0[i] + x1[i];
        for (int i = 0; i < 5 * SNX_ART_FRAME; i++) {
            i64 o[2] = {0, 0};
            for (int k = 0; k < 10; k++) o[side[k]] += rv_tap(xs, i, t[k], g[k]);
            w0[i] = tsat(tint(tdiv(o[0], 64 * 16384)));
            w1[i] = tsat(tint(tdiv(o[1], 64 * 16384)));
        }
        int a = first_diff(y0, w0, 5 * SNX_ART_FRAME), b = first_diff(y1, w1, 5 * SNX_ART_FRAME);
        CHECK(a < 0 && b < 0, "stereo early reflections: left at %d (%d, want %d), right at %d (%d, want %d)", a,
              a < 0 ? 0 : y0[a], a < 0 ? 0 : w0[a], b, b < 0 ? 0 : y1[b], b < 0 ? 0 : w1[b]);
    }

    /* six channels: the LFE takes every tap at 0.2; the old order before revision 11 swaps the back right */
    for (art_rev = 4; art_rev <= 11; art_rev += 7) {
        static i16 x[5 * SNX_ART_FRAME], b3[5 * SNX_ART_FRAME], b5[5 * SNX_ART_FRAME];
        static i32 xs[5 * SNX_ART_FRAME];
        static const i32 v[11] = {0, 16384, 0, 0, 0, 24576, 8192, 0, 16384, 16384, 0};
        for (int run = 0; run < 2; run++) {
            reset();
            u8* p = fx_voice(SNX_AUDREN_EFFECT_REVERB);
            w32(mix(SNX_ART_SUBMIX), 8, 6);
            if (run) route(0, 0, 0.0f), route(1, 1, 0.0f), route(3, 0, 1.0f), route(5, 1, 1.0f);
            w8(eff(0), 2, (u8)run);
            for (int c = 0; c < 6; c++) p[c] = p[6 + c] = (u8)c;
            reverb_set(p, 6, v);
            for (int f = 0; f < 5; f++) {
                frames(1);
                w8(p, 0x40, SNX_AUDREN_PARAM_UPDATED);
            }
            if (run) {
                sink_copy(b3, 0, 5, 0);
                sink_copy(b5, 0, 5, 1);
            } else
                sink_copy(x, 0, 5, 0);
        }
        i32 t[10];
        i64 g[10];
        rv_first_taps(0, 16384, 0, t, g);
        for (int i = 0; i < 5 * SNX_ART_FRAME; i++) xs[i] = x[i];
        int bad = -1;
        for (int i = 0; i < 5 * SNX_ART_FRAME && bad < 0; i++) {
            i64 all = 0;
            for (int k = 0; k < 10; k++) all += rv_tap(xs, i, t[k], g[k]);
            i16 lfe = tsat(tint(tdiv(tmul(all, 3276), 64 * 16384))),
                back = tsat(tint(tdiv(rv_tap(xs, i, t[8], g[8]) + rv_tap(xs, i, t[9], g[9]), 64 * 16384)));
            if (b3[i] != (art_rev >= 11 ? lfe : back) || b5[i] != (art_rev >= 11 ? back : lfe)) bad = i;
        }
        CHECK(bad < 0, "six-channel reverb rev %u: sample %d is %d and %d", art_rev, bad, bad < 0 ? 0 : b3[bad],
              bad < 0 ? 0 : b5[bad]);
    }
    art_rev = 4;

    printf("%u checks, %u failed\n", checks, failures);
    audren_state_free(st);
    free(g_mem);
    return failures != 0;
}
