/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "apu_xaudio2.h"

/* XAudio2 on Windows; SDL2 audio elsewhere, with the same queue depth and the
 * same drop-when-full behaviour, so the APU paces the same way on both. */
#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      3

static void xa2_stats(const int16_t *samples, int n, int dropped);

#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_TICKS() GetTickCount()

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    if (g_xa2_initialized) return 1;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;

    if (!g_xa2_initialized || !g_xa2_source) return 0;

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if ((int)state.BuffersQueued >= XA2_NUM_BUFS) {
        xa2_stats(samples, num_samples, 1);
        return 0;
    }
    xa2_stats(samples, num_samples, 0);

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    memcpy(g_xa2_bufs[idx], samples, copy_samples * XA2_CHANNELS * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) return 0;

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    return 1;
}

#elif defined(__has_include) && __has_include(<SDL.h>)

#include <SDL.h>

#define XA2_TICKS() SDL_GetTicks()

static SDL_AudioDeviceID g_sdl_dev;
static int               g_sdl_frames_written;

int xa2_init(void)
{
    SDL_AudioSpec want, have;

    if (g_sdl_dev) return 1;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[XA2] SDL audio: %s\n", SDL_GetError());
        return 0;
    }
    memset(&want, 0, sizeof(want));
    want.freq     = XA2_SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = XA2_CHANNELS;
    want.samples  = XA2_BUF_SAMPLES;
    want.callback = NULL;                   /* queued: SDL_QueueAudio */
    g_sdl_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_sdl_dev) {
        fprintf(stderr, "[XA2] SDL_OpenAudioDevice: %s\n", SDL_GetError());
        return 0;
    }
    SDL_PauseAudioDevice(g_sdl_dev, 0);
    g_sdl_frames_written = 0;
    fprintf(stderr, "[XA2] SDL audio initialized (%s, %d Hz stereo 16-bit, "
            "%d x %d-sample queue)\n", SDL_GetCurrentAudioDriver(),
            have.freq, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;
}

void xa2_shutdown(void)
{
    if (!g_sdl_dev) return;
    SDL_CloseAudioDevice(g_sdl_dev);
    g_sdl_dev = 0;
    fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_sdl_frames_written);
}

int xa2_is_active(void)
{
    return g_sdl_dev != 0;
}

/* As the XAudio2 path: at most XA2_NUM_BUFS buffers waiting, and a buffer
 * that would make it more is dropped rather than queued, so a mixer running
 * fast cannot build up latency. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    const Uint32 bytes_per = XA2_CHANNELS * sizeof(int16_t);
    int n = num_samples > XA2_BUF_SAMPLES ? XA2_BUF_SAMPLES : num_samples;

    if (!g_sdl_dev) return 0;
    if (SDL_GetQueuedAudioSize(g_sdl_dev) >= XA2_NUM_BUFS * XA2_BUF_SAMPLES * bytes_per) {
        xa2_stats(samples, num_samples, 1);
        return 0;
    }
    xa2_stats(samples, num_samples, 0);
    if (SDL_QueueAudio(g_sdl_dev, samples, (Uint32)n * bytes_per) != 0)
        return 0;
    g_sdl_frames_written++;
    return 1;
}

#else /* no audio output on this host */

#define XA2_TICKS() 0u
int  xa2_init(void)                                   { return 0; }
void xa2_shutdown(void)                               {}
int  xa2_is_active(void)                              { return 0; }
int  xa2_submit_samples(const int16_t *s, int n)      { (void)s; (void)n; return 0; }

#endif

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

/* RECOMP_AUDIO_STATS=1: is anything audible coming out? Every ten seconds,
 * how many mix buffers reached XAudio2, how many were dropped for a full
 * queue, and the peak and RMS of what was offered. Silence and sound look the
 * same in every other log line, and nobody running a bring-up headless can
 * listen. */
static void xa2_stats(const int16_t *samples, int n, int dropped)
{
    static int on = -1;
    static unsigned submitted, drops;
    static double sumsq;
    static uint64_t count;
    static int peak;
    static uint32_t last;
    uint32_t now;
    int i;

    /* RECOMP_AUDIO_DUMP=<file>: the samples offered, appended as raw 48 kHz
     * stereo s16le (ffmpeg -f s16le -ar 48000 -ac 2 -i <file> out.wav). */
    {
        static FILE *dump = (FILE *)-1;
        if (dump == (FILE *)-1) {
            const char *e = getenv("RECOMP_AUDIO_DUMP");
            dump = e ? fopen(e, "ab") : NULL;
        }
        if (dump && !dropped) {
            fwrite(samples, sizeof(int16_t) * XA2_CHANNELS, (size_t)n, dump);
            fflush(dump);
        }
    }
    if (on < 0)
        on = getenv("RECOMP_AUDIO_STATS") != NULL;
    if (!on)
        return;
    if (dropped)
        drops++;
    else
        submitted++;
    for (i = 0; i < n * XA2_CHANNELS; i++) {
        int v = samples[i] < 0 ? -samples[i] : samples[i];
        if (v > peak)
            peak = v;
        sumsq += (double)samples[i] * samples[i];
    }
    count += (uint64_t)n * XA2_CHANNELS;
    now = (uint32_t)XA2_TICKS();
    if (!last)
        last = now;
    if (now - last >= 10000) {
        fprintf(stderr, "[XA2] %.1f s: %u buffers submitted, %u dropped; peak %d, rms %.1f\n",
                (now - last) / 1000.0, submitted, drops, peak,
                count ? sqrt(sumsq / (double)count) : 0.0);
        submitted = drops = 0;
        sumsq = 0;
        count = 0;
        peak = 0;
        last = now;
    }
}

