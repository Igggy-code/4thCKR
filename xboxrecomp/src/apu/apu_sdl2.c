/**
 * SDL2 Audio Output Backend (POSIX hosts)
 *
 * The xa2_* contract of apu_xaudio2.h on macOS and Linux: the APU frame
 * thread hands over one EP period (256 stereo frames) at a time, and the
 * device queue paces it (mcpx_apu_pace_step reads xa2_queued_samples).
 * SDL's push API (SDL_QueueAudio) is the queue; its device thread pulls one
 * device buffer per callback and pads a short pull with silence, which is
 * why the pacing marks keep at least one device buffer queued.
 *
 * Headless runs (RECOMP_HEADLESS=1) use SDL's "dummy" driver unless
 * SDL_AUDIODRIVER is already set: it drains the queue in real time without
 * a device, so pacing behaves as on a real device and nothing reaches the
 * speakers. SDL_AUDIODRIVER=coreaudio (or pulseaudio, pipewire)
 * with RECOMP_HEADLESS=1 is the way to hear a headless run.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_xaudio2.h"

#if !defined(_WIN32)

#include <SDL.h>
#include <time.h>

#define SDLA_SAMPLE_RATE   48000
#define SDLA_CHANNELS      2
/* Device buffer size and count: the same defaults and knobs as the XAudio2
 * backend (512 x 4: 10.7 ms per buffer, 43 ms queued at the high mark). The
 * count is a pacing mark here, not a slot limit: SDL's queue is unbounded. */
#define SDLA_BUF_SAMPLES_DEFAULT 512
#define SDLA_NUM_BUFS_DEFAULT    4
#define SDLA_BUF_SAMPLES_MIN     128
#define SDLA_BUF_SAMPLES_MAX     4096
#define SDLA_NUM_BUFS_MIN        2
#define SDLA_NUM_BUFS_MAX        32

static SDL_AudioDeviceID g_dev;
static int       g_buf_samples = SDLA_BUF_SAMPLES_DEFAULT;
static int       g_num_bufs    = SDLA_NUM_BUFS_DEFAULT;
static int       g_initialized;
static int       g_audio_opened;    /* this backend brought SDL audio up */
static int       g_frames_written;  /* periods handed to the queue */
static unsigned  g_underruns;
static uint64_t  g_samples_queued;  /* samples handed to the queue, ever */
static int       g_dry;
static int64_t   g_underrun_logged_ms;

static int env_int(const char *name, int def, int lo, int hi)
{
    const char *e = getenv(name);
    int v;
    if (!e || !*e) return def;
    v = atoi(e);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int xa2_init(void)
{
    SDL_AudioSpec want, have;
    const char *e, *driver;

    if (g_initialized) return 1;

    g_buf_samples = env_int("RECOMP_AUDIO_BUF_SAMPLES", SDLA_BUF_SAMPLES_DEFAULT,
                            SDLA_BUF_SAMPLES_MIN, SDLA_BUF_SAMPLES_MAX);
    g_num_bufs = env_int("RECOMP_AUDIO_BUF_COUNT", SDLA_NUM_BUFS_DEFAULT,
                         SDLA_NUM_BUFS_MIN, SDLA_NUM_BUFS_MAX);

    /* SDL installs SIGINT/SIGTERM handlers that only post SDL_QUIT, which
     * nothing reads in a headless run; keep the default handlers so Ctrl-C
     * still ends the process. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

    /* Headless: no speakers unless the caller names a driver. Must be set
     * before the audio subsystem starts; 0 keeps an existing value. */
    e = getenv("RECOMP_HEADLESS");
    if (e && *e && *e != '0')
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 0);

#ifdef __ANDROID__
    /* nv2a-port: AAudio first. SDL 2's default on Android is OpenSL ES,
     * whose buffers are refilled by an ordinary SDL thread: a late refill
     * under load (the emulator keeps several cores busy) played a
     * crackle/hiss. AAudio's data path runs on the audio HAL's own
     * high-priority thread. OpenSL ES stays as the fallback; the
     * SDL_AUDIODRIVER environment variable still overrides both. */
    if (!getenv("SDL_AUDIODRIVER") && !SDL_WasInit(SDL_INIT_AUDIO))
        SDL_SetHint(SDL_HINT_AUDIODRIVER,
                    getenv("RECOMP_AUDIO_OPENSL") ? "openslES" : "aaudio,openslES");
#endif

    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "[SDL2] audio init failed: %s\n", SDL_GetError());
            return 0;
        }
        g_audio_opened = 1;
    }

    memset(&want, 0, sizeof(want));
    want.freq     = SDLA_SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = SDLA_CHANNELS;
    want.samples  = (Uint16)g_buf_samples;
    want.callback = NULL;                       /* push: SDL_QueueAudio */

    /* No format changes allowed: the mixer's 48 kHz s16 stereo is what the
     * queue takes, and SDL converts for the device if it has to. */
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
#ifdef __ANDROID__
    if (g_dev == 0 && g_audio_opened && !getenv("SDL_AUDIODRIVER")) {
        const char *drv = SDL_GetCurrentAudioDriver();
        if (drv && !strcmp(drv, "aaudio")) {
            fprintf(stderr, "[SDL2] AAudio device failed (%s), trying OpenSL ES\n", SDL_GetError());
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            SDL_SetHint(SDL_HINT_AUDIODRIVER, "openslES");
            if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0)
                g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        }
    }
#endif
    if (g_dev == 0) {
        fprintf(stderr, "[SDL2] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        if (g_audio_opened) { SDL_QuitSubSystem(SDL_INIT_AUDIO); g_audio_opened = 0; }
        return 0;
    }
    if (have.samples > 0 && have.samples != g_buf_samples) {
        fprintf(stderr, "[SDL2] device buffer is %d samples (asked %d)\n",
                have.samples, g_buf_samples);
        g_buf_samples = have.samples;
        /* The queue marks and the pacer's stall window assume buffers no
         * larger than the env limit. A driver that grants more is still
         * fed, against marks sized for the limit, and may underrun. */
        if (g_buf_samples > SDLA_BUF_SAMPLES_MAX)
            g_buf_samples = SDLA_BUF_SAMPLES_MAX;
    }

    /* nv2a-port: the queue's lead is counted in device buffers. AAudio's
     * are small (a few ms), and DirectSound stops the sequencer for up to
     * ~10 ms dozens of times a second, so keep at least ~85 ms (4096
     * samples) of lead whatever the buffer size, as with OpenSL ES' four
     * 1024-sample buffers. */
    if (g_buf_samples > 0 && g_num_bufs * g_buf_samples < 4096) {
        int need = (4096 + g_buf_samples - 1) / g_buf_samples;
        g_num_bufs = need > SDLA_NUM_BUFS_MAX ? SDLA_NUM_BUFS_MAX : need;
    }

    g_frames_written = 0;
    g_samples_queued = 0;
    g_dry = 0;
    g_initialized = 1;
    SDL_PauseAudioDevice(g_dev, 0);

    driver = SDL_GetCurrentAudioDriver();
    fprintf(stderr, "[SDL2] audio initialized: driver %s (%d Hz stereo 16-bit, %d x %d-sample buffers, %.1f ms queued max)%s\n",
            driver ? driver : "?", SDLA_SAMPLE_RATE, g_num_bufs, g_buf_samples,
            1000.0 * g_num_bufs * g_buf_samples / SDLA_SAMPLE_RATE,
            driver && !strcmp(driver, "dummy") ? " [silent: no device output]" : "");
    return 1;
}

void xa2_shutdown(void)
{
    if (g_dev) {
        SDL_PauseAudioDevice(g_dev, 1);
        SDL_ClearQueuedAudio(g_dev);
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
    }
    if (g_audio_opened) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        g_audio_opened = 0;
    }
    if (g_initialized)
        fprintf(stderr, "[SDL2] shut down (%d periods written, %u underruns)\n",
                g_frames_written, g_underruns);
    g_initialized = 0;
}

int xa2_is_active(void)
{
    return g_initialized;
}

/* Hand one EP period to the queue. SDL copies the samples, so the caller's
 * buffer is free on return. Nothing is ever refused by depth: the queue is
 * unbounded and pacing by queue depth holds it at the marks. */
/* REC: the last 12 s of what went to the speaker, written as a WAV next to
 * the capture (noise, crackle or silence can then be looked at). */
#define ARING (48000 * 12)
static int16_t s_aring[ARING * 2];
static unsigned s_aring_pos;
volatile int g_apu_wav_request;
static void aring_dump(void)
{
    const char *dir = getenv("NV2A_DUMP_DIR");
    if (!dir) return;
    static int n;
    char path[600];
    snprintf(path, sizeof path, "%s/audio_%02d.wav", dir, ++n);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t data = ARING * 4, riff = 36 + data, sr = 48000, br = 48000 * 4, fmt = 16;
    uint16_t pcm = 1, ch = 2, ba = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    unsigned p0 = s_aring_pos % ARING;
    fwrite(s_aring + p0 * 2, 4, ARING - p0, f);
    fwrite(s_aring, 4, p0, f);
    fclose(f);
    fprintf(stderr, "[APU] last 12 s of output -> %s\n", path);
}

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    for (int i = 0; i < num_samples; i++) {
        unsigned q = (s_aring_pos++) % ARING;
        s_aring[q * 2] = samples[i * 2]; s_aring[q * 2 + 1] = samples[i * 2 + 1];
    }
    if (g_apu_wav_request == 1) { g_apu_wav_request = 2; aring_dump(); }
    if (!g_initialized || !g_dev || num_samples <= 0) return 0;
    {   extern float g_apu_out_peak;
        for (int i = 0; i < num_samples * SDLA_CHANNELS; i += 8) {
            float a = samples[i] < 0 ? -samples[i] / 32768.0f : samples[i] / 32768.0f;
            if (a > g_apu_out_peak) g_apu_out_peak = a;
        } }
    if (SDL_QueueAudio(g_dev, samples,
                       (Uint32)num_samples * SDLA_CHANNELS * sizeof(int16_t)) != 0) {
        fprintf(stderr, "[SDL2] SDL_QueueAudio failed: %s\n", SDL_GetError());
        return 0;
    }
    g_frames_written++;
    g_samples_queued += (uint64_t)num_samples;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return g_buf_samples;
}

int xa2_get_buffer_count(void)
{
    return g_num_bufs;
}

/* Samples queued and not yet pulled by the device thread. The device holds
 * up to one more buffer in flight, which this does not count, the same way
 * XAudio2's BuffersQueued does not count the one playing. Also the underrun
 * detector: an empty queue after audio has started means the device's last
 * pull was short and padded with silence; counted once per dry spell and
 * reported at most once a second. A drain announced by xa2_expect_drain
 * (the frame thread pausing) is not one. */
int xa2_queued_samples(void)
{
    Uint32 bytes;

    if (!g_initialized || !g_dev) return 0;
    bytes = SDL_GetQueuedAudioSize(g_dev);
    if (bytes == 0 && g_frames_written > 0) {
        if (!g_dry) {
            int64_t now = now_ms();
            g_dry = 1;
            g_underruns++;
            if (now - g_underrun_logged_ms >= 1000) {
                g_underrun_logged_ms = now;
                fprintf(stderr, "[SDL2] underrun x%u\n", g_underruns);
            }
        }
    } else {
        g_dry = 0;
    }
    return (int)(bytes / (SDLA_CHANNELS * sizeof(int16_t)));
}

void xa2_expect_drain(void)
{
    g_dry = 1;
}

unsigned xa2_underruns(void)
{
    return g_underruns;
}

/* What the device thread has pulled off the queue; it pulls one device
 * buffer per callback, so this leads the speaker by up to one buffer. */
uint64_t xa2_samples_played(void)
{
    uint64_t queued;

    if (!g_initialized || !g_dev) return 0;
    queued = SDL_GetQueuedAudioSize(g_dev) / (SDLA_CHANNELS * sizeof(int16_t));
    return g_samples_queued > queued ? g_samples_queued - queued : 0;
}

#endif /* !_WIN32 */
