/*
 * Native XMV movie player (HLE of the XDK's XMVDecoder library).
 *
 * The title plays its FMVs (BootIntro.xmv, Attract.xmv, briefings, endings)
 * through the statically linked XDK XMV library at 0x004E1FED..0x004E2ECC:
 * WMV2 video and ADPCM audio decoded on the (recompiled) guest CPU, the
 * audio streamed through emulated DirectSound/APU. On a phone that decode
 * competes with the renderer and the APU, and the intro stuttered and its
 * sound dropped out.
 *
 * Here the library's entry points are replaced (stdcall, same signatures):
 *   0x004E221A XMVDecoder_CreateDecoderForPackets(flags, info, ctx, getpkt, relpkt, ppDec)
 *   0x004E1FED XMVDecoder_CreateDecoderForFile(flags, path, ppDec)
 *   0x004E2615 XMVDecoder_GetVideoDescriptor(dec, desc)
 *   0x004E2682 XMVDecoder_EnableAudioStream(dec, index, flags, mixbins, ppStream)
 *   0x004E2801 XMVDecoder_GetNextFrame(dec, surface, pResult, pTimestamp)
 *   0x004E2531 XMVDecoder_CloseDecoder(dec)
 *   0x004E2E7E XMVDecoder_GetSyncTime(dec)
 * The movie file is decoded natively by FFmpeg (xmv demuxer, wmv2,
 * adpcm_ima) on a host thread; the sound goes straight to its own AAudio
 * stream, which is also the playback clock; GetNextFrame hands the frame
 * due at that clock to the title's YUY2 surface, which the title draws as
 * before. CONKER_NO_NATIVE_XMV=1 restores the guest decoder.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#endif

#include "recomp_types.h"

extern ptrdiff_t g_xbox_mem_offset;
extern char g_xbox_movie_path[260];
extern void xbox_GuestRamPrepareWrite(uint32_t va, uint32_t len);
extern int xbox_translate_path(const char *xbox_path, char *host_path_buf, unsigned buf_size);

void sub_004E221A_gen(void);
void sub_004E1FED_gen(void);
void sub_004E2615_gen(void);
void sub_004E2682_gen(void);
void sub_004E2801_gen(void);
void sub_004E2531_gen(void);
void sub_004E2E7E_gen(void);

#define GM8(va)  (*(volatile uint8_t  *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define GM32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define ARG(i)   GM32(g_esp + 4 + 4 * (i))
/* stdcall return: pop the return address and n arguments */
#define RET(v, n) do { g_eax = (uint32_t)(v); g_esp += 4 + 4 * (n); } while (0)

#define XMV_MAGIC   0x58D00000u        /* fake decoder handles: XMV_MAGIC | slot */
#define VQ          48                 /* decoded video frames, hard cap */
#define VQ_SOFT     20                 /* decode ahead this far, then rest */
#define AR_FRAMES   (44100 * 4)        /* stereo ring: 4 s */
#define MAXA        6

static int native_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("CONKER_NO_NATIVE_XMV") == NULL;
    return on;
}

static double now_s(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

typedef struct {
    int used;
    char path[512];
    AVFormatContext *fmt;
    AVCodecContext *vdec;
    int vidx;
    AVCodecContext *adec[MAXA];
    int aidx[MAXA], na, arate;
    AVRational vtb;
    int w, h;
    double fps;

    pthread_t thr;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    volatile int run, eof;

    AVFrame *vq[VQ]; double vpts[VQ]; int vq_r, vq_n;

    /* per-stream mono samples waiting to be mixed */
    int16_t *amono[MAXA]; int amono_n[MAXA], amono_cap[MAXA];
    /* mixed stereo ring, consumed by the audio callback */
    int16_t *ring; volatile int64_t ring_w, ring_r;
    volatile int64_t played;            /* frames the device has taken */
    volatile double cb_t, cb_base, cb_span; /* last callback: when, clock then, its length */
    volatile int started;               /* clock running */
    double wall0;                       /* clock origin without audio */
    double last_pts;
#ifdef __ANDROID__
    AAudioStream *as;
#endif
    struct SwsContext *sws; int sws_w, sws_h, sws_fmt;
    uint8_t *swz; size_t swz_cap;
    unsigned frames_out, frames_dropped;
} XmvPlayer;

static XmvPlayer s_pl[2];

static XmvPlayer *player_of(uint32_t h)
{
    if ((h & 0xFFFFFFF0u) != XMV_MAGIC) return NULL;
    uint32_t k = h & 0xF;
    return k < 2 && s_pl[k].used ? &s_pl[k] : NULL;
}

/* ── audio ─────────────────────────────────────────────────────────── */

static int64_t ring_fill(XmvPlayer *p) { return p->ring_w - p->ring_r; }

#ifdef __ANDROID__
static aaudio_data_callback_result_t audio_cb(AAudioStream *s, void *ud, void *data, int32_t n)
{
    (void)s;
    XmvPlayer *p = ud;
    int16_t *out = data;
    int64_t avail = ring_fill(p);
    int32_t k = 0;
    if (p->started) {
        for (; k < n && k < avail; k++) {
            int64_t i = (p->ring_r + k) % AR_FRAMES;
            out[2 * k] = p->ring[2 * i];
            out[2 * k + 1] = p->ring[2 * i + 1];
        }
        __atomic_store_n(&p->ring_r, p->ring_r + k, __ATOMIC_RELEASE);
        /* the clock moves smoothly through this burst, not in jumps of it */
        p->cb_base = (double)p->played / p->arate;
        p->cb_span = (double)k / p->arate;
        p->cb_t = now_s();
        __atomic_store_n(&p->played, p->played + k, __ATOMIC_RELEASE);
    }
    if (k < n) memset(out + 2 * k, 0, (size_t)(n - k) * 4);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
#endif

static void audio_open(XmvPlayer *p)
{
#ifdef __ANDROID__
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, p->arate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setDataCallback(b, audio_cb, p);
    if (AAudioStreamBuilder_openStream(b, &p->as) != AAUDIO_OK) p->as = NULL;
    AAudioStreamBuilder_delete(b);
    if (p->as && AAudioStream_requestStart(p->as) != AAUDIO_OK) {
        AAudioStream_close(p->as); p->as = NULL;
    }
    fprintf(stderr, "[XMV] audio %s (%d Hz, %d streams)\n", p->as ? "AAudio" : "none",
            p->arate, p->na);
#else
    (void)p;
#endif
}

static void audio_close(XmvPlayer *p)
{
#ifdef __ANDROID__
    if (p->as) { AAudioStream_requestStop(p->as); AAudioStream_close(p->as); p->as = NULL; }
#else
    (void)p;
#endif
}

/* the playback clock in seconds */
static double clock_s(XmvPlayer *p)
{
    if (!p->started) return -1.0;
#ifdef __ANDROID__
    if (p->as) {
        /* AAudio takes sound in bursts (tens of ms): counting frames taken
         * made the clock jump a burst at a time and frames come due two or
         * three at once (a third dropped). Interpolate within the burst. */
        double t = p->cb_t, base = p->cb_base, span = p->cb_span;
        if (t == 0) return (double)p->played / p->arate;
        double d = now_s() - t;
        if (d < 0) d = 0;
        if (d > span) d = span;
        return base + d;
    }
#endif
    return now_s() - p->wall0;
}

static void mono_push(XmvPlayer *p, int si, AVFrame *f)
{
    int n = f->nb_samples;
    if (p->amono_n[si] + n > p->amono_cap[si]) {
        p->amono_cap[si] = (p->amono_n[si] + n) * 2;
        p->amono[si] = realloc(p->amono[si], (size_t)p->amono_cap[si] * 2);
    }
    int16_t *d = p->amono[si] + p->amono_n[si];
    int ch = f->ch_layout.nb_channels;
    for (int i = 0; i < n; i++) {
        int v;
        switch (f->format) {
        case AV_SAMPLE_FMT_S16P: v = ((int16_t *)f->extended_data[0])[i]; break;
        case AV_SAMPLE_FMT_S16:  v = ((int16_t *)f->extended_data[0])[i * ch]; break;
        case AV_SAMPLE_FMT_FLTP: v = (int)(((float *)f->extended_data[0])[i] * 32767.f); break;
        case AV_SAMPLE_FMT_FLT:  v = (int)(((float *)f->extended_data[0])[i * ch] * 32767.f); break;
        case AV_SAMPLE_FMT_U8:   v = (((uint8_t *)f->extended_data[0])[i * ch] - 128) << 8; break;
        default: v = 0;
        }
        d[i] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    }
    p->amono_n[si] += n;
}

/* streams are FL, FR, SL, SR (the title's mixbins 0,1,4,5): fold to stereo */
static void mix_out(XmvPlayer *p, int flush)
{
    int n = 1 << 30;
    for (int s = 0; s < p->na; s++) if (p->amono_n[s] < n) n = p->amono_n[s];
    if (flush) { n = 0; for (int s = 0; s < p->na; s++) if (p->amono_n[s] > n) n = p->amono_n[s]; }
    if (n <= 0) return;
    int64_t space = AR_FRAMES - ring_fill(p);
    if (n > space) n = (int)space;
    for (int i = 0; i < n; i++) {
        int sm[MAXA];
        for (int s = 0; s < p->na; s++) sm[s] = i < p->amono_n[s] ? p->amono[s][i] : 0;
        int l, r;
        if (p->na == 1) l = r = sm[0];
        else if (p->na < 4) { l = sm[0]; r = sm[1]; }
        else { l = sm[0] + (sm[2] * 7) / 10; r = sm[1] + (sm[3] * 7) / 10;
               l = l * 4 / 5; r = r * 4 / 5; }
        int64_t w = (p->ring_w + i) % AR_FRAMES;
        p->ring[2 * w] = (int16_t)(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
        p->ring[2 * w + 1] = (int16_t)(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
    }
    __atomic_store_n(&p->ring_w, p->ring_w + n, __ATOMIC_RELEASE);
    for (int s = 0; s < p->na; s++) {
        int k = n < p->amono_n[s] ? n : p->amono_n[s];
        memmove(p->amono[s], p->amono[s] + k, (size_t)(p->amono_n[s] - k) * 2);
        p->amono_n[s] -= k;
    }
}

/* ── decode thread ─────────────────────────────────────────────────── */

static void *decode_thread(void *ud)
{
    XmvPlayer *p = ud;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    int at_eof = 0;
    while (p->run) {
        /* enough buffered? video queue full and >= 1 s of sound */
        pthread_mutex_lock(&p->mu);
        int vfull = p->vq_n >= VQ_SOFT;
        pthread_mutex_unlock(&p->mu);
        int afull = p->na == 0 || ring_fill(p) > p->arate / 2 || ring_fill(p) > AR_FRAMES - 8192;
        if (at_eof || (vfull && afull)) {
            if (at_eof) { p->eof = 1; }
            usleep(3000);
            continue;
        }
        int r = av_read_frame(p->fmt, pkt);
        if (r < 0) {
            /* drain */
            if (p->vdec) {
                avcodec_send_packet(p->vdec, NULL);
                while (avcodec_receive_frame(p->vdec, fr) == 0) {
                    pthread_mutex_lock(&p->mu);
                    while (p->vq_n >= VQ && p->run) {
                        pthread_mutex_unlock(&p->mu); usleep(3000); pthread_mutex_lock(&p->mu);
                    }
                    int k = (p->vq_r + p->vq_n) % VQ;
                    p->vq[k] = av_frame_clone(fr);
                    p->vpts[k] = (fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp : 0) * av_q2d(p->vtb);
                    p->vq_n++;
                    pthread_mutex_unlock(&p->mu);
                    av_frame_unref(fr);
                }
            }
            mix_out(p, 1);
            at_eof = 1;
            fprintf(stderr, "[XMV] end of file (%s)\n", r == AVERROR_EOF ? "eof" : "read error");
            continue;
        }
        if (pkt->stream_index == p->vidx && p->vdec) {
            if (avcodec_send_packet(p->vdec, pkt) == 0) {
                while (avcodec_receive_frame(p->vdec, fr) == 0) {
                    pthread_mutex_lock(&p->mu);
                    while (p->vq_n >= VQ && p->run) {
                        pthread_mutex_unlock(&p->mu); usleep(3000); pthread_mutex_lock(&p->mu);
                    }
                    if (!p->run) { pthread_mutex_unlock(&p->mu); av_frame_unref(fr); break; }
                    int k = (p->vq_r + p->vq_n) % VQ;
                    p->vq[k] = av_frame_clone(fr);
                    int64_t ts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp : fr->pts;
                    p->vpts[k] = ts != AV_NOPTS_VALUE ? ts * av_q2d(p->vtb) : p->last_pts + 1.0 / p->fps;
                    p->last_pts = p->vpts[k];
                    p->vq_n++;
                    pthread_mutex_unlock(&p->mu);
                    av_frame_unref(fr);
                }
            }
        } else {
            for (int s = 0; s < p->na; s++) {
                if (pkt->stream_index != p->aidx[s]) continue;
                if (avcodec_send_packet(p->adec[s], pkt) == 0)
                    while (avcodec_receive_frame(p->adec[s], fr) == 0) {
                        mono_push(p, s, fr);
                        av_frame_unref(fr);
                    }
                break;
            }
            mix_out(p, 0);
            /* a stream far ahead of the others (missing packets): don't hoard */
            for (int s = 0; s < p->na; s++)
                if (p->amono_n[s] > p->arate * 2) { mix_out(p, 1); break; }
        }
        av_packet_unref(pkt);
    }
    av_frame_free(&fr);
    av_packet_free(&pkt);
    return NULL;
}

/* ── open / close ──────────────────────────────────────────────────── */

extern volatile int64_t g_conker_movie_end_ms;
static void player_close(XmvPlayer *p)
{
    if (!p->used) return;
    g_conker_movie_end_ms = (int64_t)(now_s() * 1000.0);
    p->run = 0;
    pthread_join(p->thr, NULL);
    audio_close(p);
    for (int i = 0; i < p->vq_n; i++) av_frame_free(&p->vq[(p->vq_r + i) % VQ]);
    p->vq_n = 0;
    if (p->vdec) avcodec_free_context(&p->vdec);
    for (int s = 0; s < p->na; s++) { avcodec_free_context(&p->adec[s]); free(p->amono[s]); }
    if (p->fmt) avformat_close_input(&p->fmt);
    if (p->sws) sws_freeContext(p->sws);
    free(p->ring); free(p->swz);
    pthread_mutex_destroy(&p->mu);
    pthread_cond_destroy(&p->cv);
    fprintf(stderr, "[XMV] closed %s: %u frames shown, %u late frames dropped\n",
            p->path, p->frames_out, p->frames_dropped);
    memset(p, 0, sizeof *p);
}

static uint32_t player_open(const char *host_path)
{
    int slot = s_pl[0].used ? (s_pl[1].used ? -1 : 1) : 0;
    if (slot < 0) { player_close(&s_pl[1]); slot = 1; }
    XmvPlayer *p = &s_pl[slot];
    memset(p, 0, sizeof *p);
    snprintf(p->path, sizeof p->path, "%s", host_path);
    av_log_set_level(AV_LOG_ERROR);
    if (avformat_open_input(&p->fmt, host_path, NULL, NULL) < 0) {
        fprintf(stderr, "[XMV] cannot open %s\n", host_path);
        p->fmt = NULL;
        return 0;
    }
    avformat_find_stream_info(p->fmt, NULL);
    p->vidx = -1;
    p->arate = 44100;
    for (unsigned i = 0; i < p->fmt->nb_streams; i++) {
        AVStream *st = p->fmt->streams[i];
        const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
        if (!c) continue;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && p->vidx < 0) {
            p->vdec = avcodec_alloc_context3(c);
            avcodec_parameters_to_context(p->vdec, st->codecpar);
            p->vdec->thread_count = 2;
            if (avcodec_open2(p->vdec, c, NULL) < 0) { avcodec_free_context(&p->vdec); continue; }
            p->vidx = (int)i; p->vtb = st->time_base;
            p->w = st->codecpar->width; p->h = st->codecpar->height;
            p->fps = st->avg_frame_rate.num ? av_q2d(st->avg_frame_rate) : 30.0;
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && p->na < MAXA) {
            AVCodecContext *a = avcodec_alloc_context3(c);
            avcodec_parameters_to_context(a, st->codecpar);
            if (avcodec_open2(a, c, NULL) < 0) { avcodec_free_context(&a); continue; }
            if (p->na == 0) p->arate = st->codecpar->sample_rate;
            else if (st->codecpar->sample_rate != p->arate) { avcodec_free_context(&a); continue; }
            p->adec[p->na] = a; p->aidx[p->na] = (int)i; p->na++;
        } else st->discard = AVDISCARD_ALL;
    }
    if (p->vidx < 0) {
        fprintf(stderr, "[XMV] %s: no video stream\n", host_path);
        for (int s = 0; s < p->na; s++) avcodec_free_context(&p->adec[s]);
        avformat_close_input(&p->fmt);
        return 0;
    }
    p->ring = calloc(AR_FRAMES, 4);
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    p->used = 1; p->run = 1;
    if (p->na) audio_open(p);
    pthread_create(&p->thr, NULL, decode_thread, p);
    fprintf(stderr, "[XMV] native %s: %dx%d %.2f fps, %d audio streams @%d Hz\n",
            host_path, p->w, p->h, p->fps, p->na, p->arate);
    return XMV_MAGIC | (uint32_t)slot;
}

/* ── frame delivery ────────────────────────────────────────────────── */

static uint32_t swz_off(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t o = 0, b = 0;
    for (uint32_t m = 1; m < w || m < h; m <<= 1) {
        if (m < w) { if (x & m) o |= 1u << b; b++; }
        if (m < h) { if (y & m) o |= 1u << b; b++; }
    }
    return o;
}

static void write_surface(XmvPlayer *p, uint32_t surf, AVFrame *f)
{
    uint32_t data = GM32(surf + 4), fmtw = GM32(surf + 0xC), size = GM32(surf + 0x10);
    uint32_t fmt = (fmtw >> 8) & 0xFF;
    uint32_t w, h, pitch;
    int swizzled = 0, bpp;
    enum AVPixelFormat pf;
    switch (fmt) {
    case 0x24: pf = AV_PIX_FMT_YUYV422; bpp = 2; break;      /* YUY2 */
    case 0x25: pf = AV_PIX_FMT_UYVY422; bpp = 2; break;      /* UYVY */
    case 0x12: case 0x1E: pf = AV_PIX_FMT_BGRA; bpp = 4; break;
    case 0x06: case 0x07: pf = AV_PIX_FMT_BGRA; bpp = 4; swizzled = 1; break;
    case 0x11: pf = AV_PIX_FMT_RGB565LE; bpp = 2; break;
    case 0x05: pf = AV_PIX_FMT_RGB565LE; bpp = 2; swizzled = 1; break;
    default:
        { static int warned; if (!warned++) fprintf(stderr, "[XMV] surface format 0x%02X unsupported\n", fmt); }
        return;
    }
    if (size) {
        w = (size & 0xFFF) + 1; h = ((size >> 12) & 0xFFF) + 1;
        pitch = ((size >> 24) + 1) * 64;
    } else {
        w = 1u << ((fmtw >> 20) & 0xF); h = 1u << ((fmtw >> 24) & 0xF);
        pitch = w * bpp; swizzled = 1;
    }
    if (!data || w > 4096 || h > 4096) return;
    {
        static int logged;
        if (logged < 4) {
            logged++;
            fprintf(stderr, "[XMV] surface %08X: data %08X fmt 0x%02X %ux%u pitch %u%s (movie %dx%d)\n",
                    surf, data, fmt, w, h, pitch, swizzled ? " swizzled" : "", f->width, f->height);
        }
    }
    uint32_t va = 0x80000000u | (data & 0x03FFFFFFu);
    uint32_t bytes = pitch * h;
    if (!p->sws || p->sws_w != (int)w || p->sws_h != (int)h || p->sws_fmt != pf) {
        if (p->sws) sws_freeContext(p->sws);
        p->sws = sws_getContext(f->width, f->height, f->format, (int)w, (int)h, pf,
                                SWS_BILINEAR, NULL, NULL, NULL);
        p->sws_w = (int)w; p->sws_h = (int)h; p->sws_fmt = pf;
    }
    if (!p->sws) return;
    xbox_GuestRamPrepareWrite(va, bytes);
    uint8_t *dst = (uint8_t *)((uintptr_t)va + g_xbox_mem_offset);
    if (!swizzled) {
        uint8_t *d[4] = { dst, 0, 0, 0 };
        int ls[4] = { (int)pitch, 0, 0, 0 };
        sws_scale(p->sws, (const uint8_t * const *)f->data, f->linesize, 0, f->height, d, ls);
    } else {
        size_t need = (size_t)w * h * bpp;
        if (p->swz_cap < need) { free(p->swz); p->swz = malloc(need); p->swz_cap = need; }
        uint8_t *d[4] = { p->swz, 0, 0, 0 };
        int ls[4] = { (int)(w * bpp), 0, 0, 0 };
        sws_scale(p->sws, (const uint8_t * const *)f->data, f->linesize, 0, f->height, d, ls);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                memcpy(dst + (size_t)swz_off(x, y, w, h) * bpp, p->swz + ((size_t)y * w + x) * bpp, bpp);
    }
}

/* XMVRESULT: 0 no frame yet, 1 new frame, 2 end of file, 3 failure */
static uint32_t next_frame(XmvPlayer *p, uint32_t surf, double *pts_out)
{
    /* the first frame: wait for the decoder (the title spins on it anyway) */
    if (!p->started) {
        double t0 = now_s();
        for (;;) {
            pthread_mutex_lock(&p->mu);
            int n = p->vq_n;
            pthread_mutex_unlock(&p->mu);
            int ready = n > 0 && (p->na == 0 || ring_fill(p) >= p->arate / 4 || p->eof);
            if (ready || p->eof || now_s() - t0 > 2.0) break;
            usleep(2000);
        }
        if (!surf) return 0;
        pthread_mutex_lock(&p->mu);
        if (p->vq_n == 0) { pthread_mutex_unlock(&p->mu); return p->eof ? 2 : 0; }
        AVFrame *f = p->vq[p->vq_r];
        write_surface(p, surf, f);
        *pts_out = p->vpts[p->vq_r];
        av_frame_free(&p->vq[p->vq_r]);
        p->vq_r = (p->vq_r + 1) % VQ; p->vq_n--;
        pthread_mutex_unlock(&p->mu);
        p->wall0 = now_s();
        p->started = 1;                 /* sound and clock start with frame 0 */
        p->frames_out++;
        return 1;
    }
    double c = clock_s(p);
    pthread_mutex_lock(&p->mu);
    /* frames whose successor is already due were missed: skip them */
    while (p->vq_n >= 2 && p->vpts[(p->vq_r + 1) % VQ] <= c) {
        av_frame_free(&p->vq[p->vq_r]);
        p->vq_r = (p->vq_r + 1) % VQ; p->vq_n--;
        p->frames_dropped++;
    }
    if (p->vq_n > 0 && p->vpts[p->vq_r] <= c) {
        if (!surf) { pthread_mutex_unlock(&p->mu); return 0; }
        write_surface(p, surf, p->vq[p->vq_r]);
        *pts_out = p->vpts[p->vq_r];
        av_frame_free(&p->vq[p->vq_r]);
        p->vq_r = (p->vq_r + 1) % VQ; p->vq_n--;
        pthread_mutex_unlock(&p->mu);
        p->frames_out++;
        return 1;
    }
    int empty = p->vq_n == 0;
    pthread_mutex_unlock(&p->mu);
    /* the end: no frames left and the sound played out (or 1 s past it) */
    if (empty && p->eof) {
        static double t_end;
        int drained = 1;
#ifdef __ANDROID__
        drained = !p->as || ring_fill(p) < 1024;
#endif
        if (!drained && t_end == 0) t_end = now_s();
        if (drained || now_s() - t_end > 1.5) { t_end = 0; return 2; }
    }
    return 0;
}

/* ── the replaced XMV entry points ─────────────────────────────────── */

static uint32_t open_guest_path(const char *gpath)
{
    char host[1024];
    if (!xbox_translate_path(gpath, host, sizeof host)) {
        fprintf(stderr, "[XMV] cannot translate %s\n", gpath);
        return 0;
    }
    return player_open(host);
}

/* XMVDecoder_CreateDecoderForPackets(flags, info, ctx, getpkt, relpkt, ppDec): the
 * title feeds packets it reads itself from the .xmv it just opened; the
 * native player reads that file directly instead. */
void sub_004E221A(void)
{
    /* CreateDecoderForFile's own call (guest decoder fallback) stays guest */
    if (!native_on() || !g_xbox_movie_path[0] || GM32(g_esp) == 0x004E20A4u) {
        sub_004E221A_gen(); return;
    }
    uint32_t pp = ARG(5);
    uint32_t h = open_guest_path(g_xbox_movie_path);
    if (!h) { sub_004E221A_gen(); return; }     /* fall back to the guest decoder */
    if (pp) GM32(pp) = h;
    RET(0, 6);
}

/* XMVDecoder_CreateDecoderForFile(flags, path, ppDec) */
void sub_004E1FED(void)
{
    if (!native_on()) { sub_004E1FED_gen(); return; }
    uint32_t pathva = ARG(1), pp = ARG(2);
    char gp[260]; int k = 0;
    while (pathva && k < 259 && (gp[k] = (char)GM8(pathva + k))) k++;
    gp[k] = 0;
    uint32_t h = k ? open_guest_path(gp) : 0;
    if (!h) { sub_004E1FED_gen(); return; }
    if (pp) GM32(pp) = h;
    RET(0, 3);
}

/* XMVDecoder_GetVideoDescriptor(dec, desc): Width, Height, FramesPerSecond,
 * AudioStreamCount. No audio streams for the title: the sound is ours. */
void sub_004E2615(void)
{
    XmvPlayer *p = player_of(ARG(0));
    if (!p) { sub_004E2615_gen(); return; }
    uint32_t d = ARG(1);
    if (d) {
        GM32(d) = (uint32_t)p->w; GM32(d + 4) = (uint32_t)p->h;
        GM32(d + 8) = (uint32_t)(p->fps + 0.5); GM32(d + 12) = 0;
    }
    RET(0, 2);
}

/* XMVDecoder_EnableAudioStream(dec, index, flags, mixbins, ppStream) */
void sub_004E2682(void)
{
    if (!player_of(ARG(0))) { sub_004E2682_gen(); return; }
    uint32_t pp = ARG(4);
    if (pp) GM32(pp) = 0;
    RET(0, 5);
}

/* last time a movie asked for a frame (a movie is not a loading screen) */
volatile int64_t g_xmv_last_frame_ms;

/* XMVDecoder_GetNextFrame(dec, surface, pResult, pTimestamp) */
void sub_004E2801(void)
{
    XmvPlayer *p = player_of(ARG(0));
    if (!p) { sub_004E2801_gen(); return; }
    g_xmv_last_frame_ms = (int64_t)(now_s() * 1000.0);
    uint32_t surf = ARG(1), pr = ARG(2), pt = ARG(3);
    double pts = 0;
    uint32_t xr = next_frame(p, surf, &pts);
    if (pr) GM32(pr) = xr;
    if (pt && xr == 1) {
        uint64_t t = (uint64_t)(pts * 1e7);
        GM32(pt) = (uint32_t)t; GM32(pt + 4) = (uint32_t)(t >> 32);
    }
    if (xr == 2) fprintf(stderr, "[XMV] finished: %u frames shown, %u dropped\n",
                         p->frames_out, p->frames_dropped);
    RET(0, 4);
}

/* XMVDecoder_CloseDecoder(dec) */
void sub_004E2531(void)
{
    XmvPlayer *p = player_of(ARG(0));
    if (!p) { sub_004E2531_gen(); return; }
    player_close(p);
    RET(0, 1);
}

/* XMVDecoder_GetSyncTime(dec): milliseconds into the movie */
void sub_004E2E7E(void)
{
    XmvPlayer *p = player_of(ARG(0));
    if (!p) { sub_004E2E7E_gen(); return; }
    double c = clock_s(p);
    g_edx = 0;
    RET(c > 0 ? (uint32_t)(c * 1000.0) : 0, 1);
}

/* Movie manager per-frame update (sub_002527F0). It meters GetNextFrame by
 * its own clock (rdtsc into an accumulator at 0x849C6C, one call per 33.3
 * ms), which here runs slow: a third of the frames came due between its
 * calls and were dropped. With the native player the accumulator is set
 * from the player's clock instead: one call whenever a frame is due. */
void sub_002527F0_gen(void);
static void movie_loop_stats(void);
volatile int64_t g_conker_movie_end_ms;
void sub_002527F0(void)
{
    XmvPlayer *p = GM32(0x849C60) ? player_of(GM32(0x849C4C)) : NULL;
    volatile float *acc = (volatile float *)((uintptr_t)0x849C6Cu + g_xbox_mem_offset);
    if (p && *acc != -1.0f) {
        int due = 1;
        if (p->started) {
            double c = clock_s(p);
            pthread_mutex_lock(&p->mu);
            due = p->vq_n ? p->vpts[p->vq_r] <= c : p->eof;
            pthread_mutex_unlock(&p->mu);
        }
        *acc = due ? 33.34f : 0.0f;
        movie_loop_stats();
    }
    sub_002527F0_gen();
}

/* Movie loop timing (while a native movie plays): the interval between
 * the manager's updates and the time the frame's render + present
 * (sub_002AC490) takes, summed over 5 s. */
void sub_002AC490_gen(void);
static double s_ml_last, s_ml_t0, s_ml_int, s_ml_pres, s_ml_max;
static unsigned s_ml_n;
double g_xmv_bot_s;   /* time in D3D_BlockOnTime (recomp_manual.c) */
void sub_002AC490(void) { sub_002AC490_gen(); }
static void movie_loop_stats(void)
{
    double t1 = now_s();
    s_ml_pres = g_xmv_bot_s;
    if (s_ml_last) { double d = t1 - s_ml_last; s_ml_int += d; if (d > s_ml_max) s_ml_max = d; s_ml_n++; }
    s_ml_last = t1;
    if (!s_ml_t0) s_ml_t0 = t1;
    if (t1 - s_ml_t0 > 5.0 && s_ml_n) {
        fprintf(stderr, "[XMV] loop: %u frames/5s, avg %.1f ms (max %.1f), GPU waits %.1f ms\n",
                s_ml_n, s_ml_int / s_ml_n * 1e3, s_ml_max * 1e3, s_ml_pres / s_ml_n * 1e3);
        s_ml_t0 = t1; s_ml_n = 0; s_ml_int = s_ml_pres = s_ml_max = 0; g_xmv_bot_s = 0;
    }
}
