/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SceAudiodecUser: ATRAC9 and AAC decoded with FFmpeg's libavcodec into 16-bit interleaved PCM, a frame a call (n with
   DecodeNFrames), and SceCodecEngineUser's memory blocks, which the decoders' callers carve their memory from. As the
   vitasdk headers (MIT) declare them; the sizes a decoder reports are Vita3K's (GPL-2.0-or-later,
   modules/SceAudiodec/SceAudiodecUser.cpp).

   ATRAC9: each frame is the configuration's frame size (configData's average frame size, constant: the bit rate is),
   FFmpeg's decoder decoding as many as its packet holds, so a packet of one frame gives one. AAC: a raw frame, no
   ADTS, of the size SceMp4 gave the unit at that address (vita_mp4_unit_size: a raw frame says nothing of its length),
   or the most a decoder reads */
#include "vita/hle/hle.h"
#include <libavcodec/avcodec.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TYPE_AT9 0x1003u
#define TYPE_AAC 0x1005u
#define SCE_AUDIODEC_ERROR_API_FAIL           0x807f0000u
#define SCE_AUDIODEC_ERROR_INVALID_TYPE       0x807f0001u
#define SCE_AUDIODEC_ERROR_OUT_OF_MEMORY      0x807f0004u
#define SCE_AUDIODEC_ERROR_ALL_HANDLES_IN_USE 0x807f0007u
#define SCE_AUDIODEC_ERROR_INVALID_PTR        0x807f0008u
#define SCE_AUDIODEC_ERROR_INVALID_HANDLE     0x807f0009u
#define SCE_AUDIODEC_ERROR_INVALID_SIZE       0x807f000du
#define CTRL_SIZE  0x28u
#define AAC_MOST   1536u /* the most an AAC frame takes */
#define AT9_MOST   1024u
#define DECODERS   32u
#define BLOCKS     16u

typedef struct {
    bool used;
    uint32_t type, channels, frame_bytes, frame_samples;
    AVCodecContext* codec;
    AVPacket* packet;
    AVFrame* frame;
} Decoder;
struct VitaAudiodec {
    Decoder decoders[DECODERS];
    struct {
        uint32_t base, size, used;
    } blocks[BLOCKS]; /* the codec engine's: carved from their start */
};

static uint32_t arg(VitaProcess* p, VitaThread* t, unsigned n) { return vita_arg(p, t, n); }
static struct VitaAudiodec* state(VitaProcess* p) {
    if (!p->audiodec) p->audiodec = calloc(1, sizeof *p->audiodec);
    return p->audiodec;
}
static Decoder* decoder_of(VitaProcess* p, uint32_t ctrl) {
    uint32_t handle = ctrl ? vita_read32(p, ctrl + 4) : 0;
    return p->audiodec && handle >= 1 && handle <= DECODERS && p->audiodec->decoders[handle - 1].used
               ? &p->audiodec->decoders[handle - 1]
               : NULL;
}
static void release(Decoder* d) {
    avcodec_free_context(&d->codec);
    av_packet_free(&d->packet);
    av_frame_free(&d->frame);
    memset(d, 0, sizeof *d);
}
void vita_audiodec_free(VitaProcess* p) {
    if (!p->audiodec) return;
    for (unsigned i = 0; i < DECODERS; ++i)
        if (p->audiodec->decoders[i].used) release(&p->audiodec->decoders[i]);
    free(p->audiodec);
    p->audiodec = NULL;
}
static uint32_t ok(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}

/* ---- the decoders ---- */
/* sceAudiodecGetContextSize(ctrl, type): the decoder's memory, which its caller allocates */
static uint32_t get_context_size(VitaProcess* p, VitaThread* t) {
    uint32_t ctrl = arg(p, t, 0), type = arg(p, t, 1);
    if (!ctrl) return SCE_AUDIODEC_ERROR_INVALID_PTR;
    if (vita_read32(p, ctrl) != CTRL_SIZE) return SCE_AUDIODEC_ERROR_INVALID_SIZE;
    return type == TYPE_AAC ? 0x18000u : type == TYPE_AT9 ? 0xc00u : SCE_AUDIODEC_ERROR_INVALID_TYPE;
}
/* frames' samples by ATRAC9's sample rate index (FFmpeg's at9_tab_sri_frame_log2) */
static const uint8_t at9_frame_log2[16] = {6, 6, 7, 7, 7, 8, 8, 8, 6, 6, 7, 7, 7, 8, 8, 8};
/* A decoder of type for ctrl (its SceAudiodecInfo at pInfo, 0x24): FFmpeg's, its sizes written back (maxEsSize at
   0x10, maxPcmSize at 0x1c; an ATRAC9 one's channels, bit rate, sample rate, superframe size and frames from its
   configuration) */
static uint32_t create(VitaProcess* p, uint32_t ctrl, uint32_t type) {
    if (!ctrl) return SCE_AUDIODEC_ERROR_INVALID_PTR;
    if (vita_read32(p, ctrl) != CTRL_SIZE) return SCE_AUDIODEC_ERROR_INVALID_SIZE;
    uint32_t info = vita_read32(p, ctrl + 0x24);
    if (!info) return SCE_AUDIODEC_ERROR_INVALID_PTR;
    if (type != TYPE_AAC && type != TYPE_AT9) return SCE_AUDIODEC_ERROR_INVALID_TYPE;
    struct VitaAudiodec* s = state(p);
    Decoder* d = NULL;
    for (unsigned i = 0; s && i < DECODERS && !d; ++i)
        if (!s->decoders[i].used) d = &s->decoders[i];
    if (!d) return s ? SCE_AUDIODEC_ERROR_ALL_HANDLES_IN_USE : SCE_AUDIODEC_ERROR_OUT_OF_MEMORY;
    const AVCodec* codec = avcodec_find_decoder(type == TYPE_AAC ? AV_CODEC_ID_AAC : AV_CODEC_ID_ATRAC9);
    if (!codec) return SCE_AUDIODEC_ERROR_API_FAIL;
    d->used = true, d->type = type;
    d->codec = avcodec_alloc_context3(codec), d->packet = av_packet_alloc(), d->frame = av_frame_alloc();
    unsigned char* extra = av_mallocz(12 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!d->codec || !d->packet || !d->frame || !extra) {
        av_free(extra);
        release(d);
        return SCE_AUDIODEC_ERROR_OUT_OF_MEMORY;
    }
    uint32_t most_es, most_pcm;
    if (type == TYPE_AAC) { /* SceAudiodecInfoAac: size, isAdts, ch, samplingRate, isSbr */
        static const uint32_t rates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                           22050, 16000, 12000, 11025, 8000,  7350};
        uint32_t channels = vita_read32(p, info + 8), rate = vita_read32(p, info + 12), index = 0;
        while (index < 12 && rates[index] != rate) ++index;
        /* the AudioSpecificConfig a raw frame needs: AAC LC (2), the rate's index, the channels */
        extra[0] = (unsigned char)(2 << 3 | index >> 1), extra[1] = (unsigned char)((index & 1) << 7 | (channels & 15) << 3);
        d->codec->extradata = extra, d->codec->extradata_size = 2;
        d->channels = channels, d->frame_samples = 1024;
        most_es = AAC_MOST + (vita_read32(p, info + 4) ? 0x100 : 0);
        most_pcm = channels * 2048 * (vita_read32(p, info + 16) ? 2 : 1);
    } else { /* SceAudiodecInfoAt9: size, configData[4], ch, bitRate, samplingRate, superFrameSize, frames */
        const unsigned char* config = vita_ptr(p, info + 4);
        extra[0] = 2; /* FFmpeg's extradata: a version, then the configuration */
        memcpy(extra + 4, config, 4);
        uint32_t bits = (uint32_t)config[0] << 24 | (uint32_t)config[1] << 16 | (uint32_t)config[2] << 8 | config[3];
        /* the configuration word: 0xfe, the sample rate's index (4 bits), the channels' layout (3), a zero bit, the
           frame's bytes less one (11), the superframe's frames as a power of two (2) */
        d->frame_bytes = ((bits >> 5) & 0x7ff) + 1;
        uint32_t frames = 1u << ((bits >> 3) & 3);
        d->frame_samples = 1u << at9_frame_log2[(bits >> 20) & 15];
        d->codec->extradata = extra, d->codec->extradata_size = 12;
        d->codec->block_align = (int)(d->frame_bytes * frames);
        if (avcodec_open2(d->codec, codec, NULL) < 0) {
            release(d);
            return SCE_AUDIODEC_ERROR_API_FAIL;
        }
        d->channels = (uint32_t)d->codec->ch_layout.nb_channels;
        uint32_t rate = (uint32_t)d->codec->sample_rate, superframe = d->frame_bytes * frames;
        vita_write32(p, info + 8, d->channels);
        vita_write32(p, info + 12, (uint32_t)((uint64_t)superframe * 8 * rate / (d->frame_samples * frames)));
        vita_write32(p, info + 16, rate);
        vita_write32(p, info + 20, superframe);
        vita_write32(p, info + 24, frames);
        most_es = superframe < AT9_MOST ? superframe : AT9_MOST;
        most_pcm = d->frame_samples * d->channels * 2;
    }
    if (type == TYPE_AAC && avcodec_open2(d->codec, codec, NULL) < 0) {
        release(d);
        return SCE_AUDIODEC_ERROR_API_FAIL;
    }
    vita_write32(p, ctrl + 4, (uint32_t)(d - s->decoders) + 1);
    vita_write32(p, ctrl + 0x10, most_es), vita_write32(p, ctrl + 0x1c, most_pcm);
    return 0;
}
/* sceAudiodecCreateDecoder(ctrl, type), and its External form (ctrl, type, memory, size), whose memory is unused */
static uint32_t create_decoder(VitaProcess* p, VitaThread* t) { return create(p, arg(p, t, 0), arg(p, t, 1)); }
static uint32_t delete_decoder(VitaProcess* p, VitaThread* t) {
    Decoder* d = decoder_of(p, arg(p, t, 0));
    if (!d) return SCE_AUDIODEC_ERROR_INVALID_HANDLE;
    release(d);
    vita_write32(p, arg(p, t, 0) + 4, 0);
    return 0;
}
/* one frame from es into pcm: the bytes of each, false when it can't be decoded */
static bool decode_frame(VitaProcess* p, Decoder* d, uint32_t es, uint32_t pcm, uint32_t most_es, uint32_t* used,
                         uint32_t* given) {
    uint32_t size = d->type == TYPE_AT9 ? d->frame_bytes : vita_mp4_unit_size(p, es);
    if (!size || size > most_es) size = most_es;
    if (!vita_mapped(p, es, size) || av_new_packet(d->packet, (int)size) < 0) return false;
    memcpy(d->packet->data, vita_ptr(p, es), size);
    int r = avcodec_send_packet(d->codec, d->packet);
    av_packet_unref(d->packet);
    if (r < 0 || avcodec_receive_frame(d->codec, d->frame) < 0) return false;
    /* FFmpeg's planar or packed samples, float or 16-bit, as interleaved 16-bit ones */
    AVFrame* f = d->frame;
    uint32_t channels = (uint32_t)f->ch_layout.nb_channels, samples = (uint32_t)f->nb_samples;
    if (!vita_mapped(p, pcm, samples * channels * 2)) {
        av_frame_unref(f);
        return false;
    }
    unsigned char* out = vita_ptr(p, pcm);
    for (uint32_t i = 0; i < samples; ++i)
        for (uint32_t c = 0; c < channels; ++c) {
            int v;
            switch (f->format) {
                case AV_SAMPLE_FMT_FLTP: v = (int)lrintf(((const float*)f->extended_data[c])[i] * 32767.0f); break;
                case AV_SAMPLE_FMT_FLT: v = (int)lrintf(((const float*)f->data[0])[i * channels + c] * 32767.0f); break;
                case AV_SAMPLE_FMT_S16P: v = ((const int16_t*)f->extended_data[c])[i]; break;
                default: v = ((const int16_t*)f->data[0])[i * channels + c]; break;
            }
            v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
            out[(i * channels + c) * 2] = (unsigned char)v, out[(i * channels + c) * 2 + 1] = (unsigned char)(v >> 8);
        }
    *used = size, *given = samples * channels * 2;
    av_frame_unref(f);
    return true;
}
/* sceAudiodecDecodeNFrames(ctrl, n): from pEs (0x8) into pPcm (0x14), the bytes each took into inputEsSize (0xc) and
   outputPcmSize (0x18) */
static uint32_t decode_frames(VitaProcess* p, uint32_t ctrl, uint32_t frames) {
    if (!ctrl) return SCE_AUDIODEC_ERROR_INVALID_PTR;
    Decoder* d = decoder_of(p, ctrl);
    if (!d) return SCE_AUDIODEC_ERROR_INVALID_HANDLE;
    uint32_t es = vita_read32(p, ctrl + 8), pcm = vita_read32(p, ctrl + 0x14), most = vita_read32(p, ctrl + 0x10);
    uint32_t used = 0, given = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        uint32_t u, g;
        if (!decode_frame(p, d, es + used, pcm + given, most, &u, &g)) {
            vita_write32(p, ctrl + 0xc, used), vita_write32(p, ctrl + 0x18, given);
            return SCE_AUDIODEC_ERROR_API_FAIL;
        }
        used += u, given += g;
    }
    vita_write32(p, ctrl + 0xc, used), vita_write32(p, ctrl + 0x18, given);
    return 0;
}
static uint32_t decode(VitaProcess* p, VitaThread* t) { return decode_frames(p, arg(p, t, 0), 1); }
static uint32_t decode_n_frames(VitaProcess* p, VitaThread* t) {
    return decode_frames(p, arg(p, t, 0), arg(p, t, 1));
}
/* sceAudiodecClearContext(ctrl): the decoder as new, before a seek */
static uint32_t clear_context(VitaProcess* p, VitaThread* t) {
    Decoder* d = decoder_of(p, arg(p, t, 0));
    if (!d) return SCE_AUDIODEC_ERROR_INVALID_HANDLE;
    avcodec_flush_buffers(d->codec);
    return 0;
}

/* ---- SceCodecEngine: memory the decoders' callers hand them, carved from blocks the caller allocated ---- */
/* sceCodecEngineOpenUnmapMemBlock(base, size): the block's id */
static uint32_t open_block(VitaProcess* p, VitaThread* t) {
    struct VitaAudiodec* s = state(p);
    for (unsigned i = 0; s && i < BLOCKS; ++i)
        if (!s->blocks[i].size) {
            s->blocks[i].base = arg(p, t, 0), s->blocks[i].size = arg(p, t, 1), s->blocks[i].used = 0;
            return s->blocks[i].size ? i + 1 : 0x80000000u;
        }
    return 0x80000000u;
}
/* sceCodecEngineAllocMemoryFromUnmapMemBlock(id, size, alignment): its address, 0 without room */
static uint32_t alloc_from_block(VitaProcess* p, VitaThread* t) {
    uint32_t id = arg(p, t, 0), size = arg(p, t, 1), align = arg(p, t, 2);
    struct VitaAudiodec* s = p->audiodec;
    if (!s || id < 1 || id > BLOCKS || !s->blocks[id - 1].size || (align & (align - 1))) return 0;
    if (!align) align = 1;
    uint64_t at = ((uint64_t)s->blocks[id - 1].base + s->blocks[id - 1].used + align - 1) & ~(uint64_t)(align - 1);
    if (at + size > (uint64_t)s->blocks[id - 1].base + s->blocks[id - 1].size) return 0;
    s->blocks[id - 1].used = (uint32_t)(at + size - s->blocks[id - 1].base);
    return (uint32_t)at;
}
static uint32_t close_block(VitaProcess* p, VitaThread* t) {
    uint32_t id = arg(p, t, 0);
    if (!p->audiodec || id < 1 || id > BLOCKS) return 0x80000000u;
    memset(&p->audiodec->blocks[id - 1], 0, sizeof p->audiodec->blocks[id - 1]);
    return 0;
}

const VitaHle vita_hle_audiodec[] = {
    {0x445c2cefu, "sceAudiodecInitLibrary", ok},
    {0x45719b9du, "sceAudiodecTermLibrary", ok},
    {0xdb712abcu, "sceAudiodecGetContextSize", get_context_size},
    {0x4dfd3aaau, "sceAudiodecCreateDecoder", create_decoder},
    {0x56085dfbu, "sceAudiodecCreateDecoderExternal", create_decoder},
    {0xe7a24e16u, "sceAudiodecDeleteDecoder", delete_decoder},
    {0xe4ea05bbu, "sceAudiodecDeleteDecoderExternal", delete_decoder},
    {0xccdaba04u, "sceAudiodecDecode", decode},
    {0x8018aa9bu, "sceAudiodecDecodeNFrames", decode_n_frames},
    {0xf72f9b64u, "sceAudiodecClearContext", clear_context},
    {0x883b0cf5u, "sceAudiodecGetInternalError", ok},
    {0xf0b4c892u, "sceCodecEngineOpenUnmapMemBlock", open_block},
    {0x1a3a53e5u, "sceCodecEngineAllocMemoryFromUnmapMemBlock", alloc_from_block},
    {0x6a31831du, "sceCodecEngineFreeMemoryFromUnmapMemBlock", ok},
    {0x95ea3b3eu, "sceCodecEngineCloseUnmapMemBlock", close_block},
    {0, NULL, NULL},
};
