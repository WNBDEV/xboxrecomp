/*
 * Frame profiler: where each frame's time goes, across the title's thread and
 * the NV2A executor. Off unless asked for; when off, every hook is one test of
 * a flag.
 *
 * RECOMP_FRAME_STATS=1 prints a line every ten seconds:
 *
 *   [FRAMES] last 10.0 s: 598 frames, 16.72 ms each (p50 16.67, p95 18.3,
 *   p99 25.1, max 41.2); title: CPU 6.1 ms, kernel waits 10.2 ms a frame --
 *   0016CAFB 5.8 ms (1.3), ...; executor: CPU 7.9 ms, waiting for the title
 *   3.1, flip pacing 5.2, Present 0.0; 412 draws a frame
 *
 * The title's CPU time is its thread's own clock; its waits are the kernel
 * calls it blocked in, by the guest address that called them (the same key as
 * RECOMP_WAIT_PROFILE). The executor's figures come from its poll loop:
 * running commands on the CPU, idle with nothing submitted, asleep in
 * flip_pace, and in Present.
 *
 * RECOMP_TRACE=<file.json> also records a timeline, in the Chrome trace format
 * that ui.perfetto.dev and chrome://tracing open: a row per thread, with the
 * executor's command runs, pacing sleeps, Presents and software-method stalls,
 * every kernel call of 50 us or more on any guest thread (named by ordinal and
 * caller), and a mark at each flip. RECOMP_TRACE_FOR seconds of it (5 by
 * default), starting once RECOMP_TRACE_TRIGGER names a file that exists, or
 * RECOMP_TRACE_AT seconds after the first frame (10 by default).
 */
#include "kernel.h"
#include "frame_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#endif

static int s_stats = -1, s_trace = -1;
static const char *s_trace_path, *s_trigger;
static double s_trace_at = 10.0, s_trace_for = 5.0;

int xbox_FrameTraceOn(void)
{
    if (s_stats < 0) {
        const char *e;
        s_stats = getenv("RECOMP_FRAME_STATS") != NULL;
        s_trace_path = getenv("RECOMP_TRACE");
        s_trace = s_trace_path && *s_trace_path;
        s_trigger = getenv("RECOMP_TRACE_TRIGGER");
        if ((e = getenv("RECOMP_TRACE_AT")) != NULL)
            s_trace_at = atof(e);
        if ((e = getenv("RECOMP_TRACE_FOR")) != NULL)
            s_trace_for = atof(e);
        if (s_trace)
            s_stats = 1;
    }
    return s_stats;
}

double xbox_FrameTraceNowUs(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1e6 / (double)f.QuadPart;
}

/* ---- the title's thread ---------------------------------------------------- */

static DWORD s_title_tid;
#ifdef _WIN32
static HANDLE s_title_handle;
#else
static clockid_t s_title_clock;
static int s_title_clock_ok;
#endif

static CRITICAL_SECTION s_wait_cs;    /* the title's waits; set up here, */
static int s_wait_cs_ready;            /* before the title runs */

void xbox_FrameTraceTitleThread(void)
{
    if (!s_wait_cs_ready) {
        InitializeCriticalSection(&s_wait_cs);
        s_wait_cs_ready = 1;
    }
    s_title_tid = GetCurrentThreadId();
#ifdef _WIN32
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &s_title_handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
#else
    s_title_clock_ok = pthread_getcpuclockid(pthread_self(), &s_title_clock) == 0;
#endif
}

static double title_cpu_ms(void)
{
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!s_title_handle || !GetThreadTimes(s_title_handle, &c, &e, &k, &u))
        return 0.0;
    return ((double)(((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime)
            + (double)(((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 1e4;
#else
    struct timespec ts;
    if (!s_title_clock_ok || clock_gettime(s_title_clock, &ts))
        return 0.0;
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

/* ---- the timeline ---------------------------------------------------------- */

typedef struct {
    const char *name;
    double ts, dur;            /* us; dur < 0: an instant */
    DWORD tid;
    uint32_t a, b;
} Event;

#define MAX_EVENTS (1u << 20)
static Event *s_ev;
static volatile LONG s_nev;
static volatile LONG s_capturing;      /* 1 while recording */
static int s_trace_done;
static double s_trace_start, s_trace_end, s_first_frame;
static DWORD s_exec_tid;

void xbox_FrameTraceSpan(const char *name, double t0_us, double t1_us, uint32_t a, uint32_t b)
{
    LONG i;
    if (!s_capturing)
        return;
    i = InterlockedIncrement(&s_nev) - 1;
    if ((uint32_t)i >= MAX_EVENTS)
        return;
    s_ev[i].name = name;
    s_ev[i].ts = t0_us;
    s_ev[i].dur = t1_us < 0 ? -1.0 : t1_us - t0_us;
    s_ev[i].tid = GetCurrentThreadId();
    s_ev[i].a = a;
    s_ev[i].b = b;
}

static void trace_write(void)
{
    FILE *f = fopen(s_trace_path, "w");
    LONG i, n = s_nev < (LONG)MAX_EVENTS ? s_nev : (LONG)MAX_EVENTS;
    if (!f) {
        fprintf(stderr, "[TRACE] cannot write %s\n", s_trace_path);
        return;
    }
    fprintf(f, "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n");
    fprintf(f, "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":%lu,"
               "\"args\":{\"name\":\"title (game code)\"}},\n", (unsigned long)s_title_tid);
    fprintf(f, "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":%lu,"
               "\"args\":{\"name\":\"NV2A executor\"}}", (unsigned long)s_exec_tid);
    for (i = 0; i < n; i++) {
        const Event *e = &s_ev[i];
        if (e->dur < 0)
            fprintf(f, ",\n{\"name\":\"%s\",\"ph\":\"i\",\"s\":\"t\",\"ts\":%.1f,\"pid\":1,"
                       "\"tid\":%lu,\"args\":{\"a\":%u}}",
                    e->name, e->ts - s_trace_start, (unsigned long)e->tid, e->a);
        else
            fprintf(f, ",\n{\"name\":\"%s\",\"ph\":\"X\",\"ts\":%.1f,\"dur\":%.1f,\"pid\":1,"
                       "\"tid\":%lu,\"args\":{\"a\":\"%08X\",\"b\":\"%08X\"}}",
                    e->name, e->ts - s_trace_start, e->dur, (unsigned long)e->tid, e->a, e->b);
    }
    fprintf(f, "\n]}\n");
    fclose(f);
    fprintf(stderr, "[TRACE] %ld events, %.1f s, written to %s\n", (long)n,
            (s_trace_end - s_trace_start) / 1e6, s_trace_path);
    fflush(stderr);
}

static int trigger_exists(void)
{
#ifdef _WIN32
    return GetFileAttributesA(s_trigger) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(s_trigger, &st) == 0;
#endif
}

/* ---- the ten-second line --------------------------------------------------- */

#define MAX_FRAMES 2048
#define MAX_CALLERS 64
static struct {
    double frame_ms[MAX_FRAMES];
    unsigned frames;
    double exec_cpu, exec_idle, pacing, present;
    unsigned draws;
    double title_cpu0;
    struct { uint32_t caller; double ms; unsigned n; } wait[MAX_CALLERS];
} s_w;
void xbox_FrameTraceAdd(int what, double ms)
{
    switch (what) {
    case FTR_EXEC_CPU: s_w.exec_cpu += ms; break;
    case FTR_EXEC_IDLE: s_w.exec_idle += ms; break;
    case FTR_PACING: s_w.pacing += ms; break;
    case FTR_PRESENT: s_w.present += ms; break;
    default: break;
    }
}

void xbox_FrameTraceDraws(unsigned n)
{
    s_w.draws += n;
}

/* A kernel call that took `us` on this thread: a span on the timeline, and
 * for the title's thread, its waits by caller. */
void xbox_FrameTraceKernelCall(uint32_t ordinal, uint32_t caller, uint32_t arg,
                               double t0_us, double t1_us)
{
    static const char *names[] = { "kernel call", "KeWaitForSingleObject",
                                   "KeWaitForMultipleObjects", "KeDelayExecutionThread" };
    double us = t1_us - t0_us;
    int i, k;

    if (us < 50.0)
        return;
    k = ordinal == 159 ? 1 : ordinal == 158 ? 2 : ordinal == 99 ? 3 : 0;
    /* a: the guest caller; b: the object waited on, or the ordinal */
    xbox_FrameTraceSpan(names[k], t0_us, t1_us, caller, k ? arg : ordinal);
    if (!s_wait_cs_ready || GetCurrentThreadId() != s_title_tid)
        return;
    EnterCriticalSection(&s_wait_cs);
    for (i = 0; i < MAX_CALLERS && s_w.wait[i].n && s_w.wait[i].caller != caller; i++)
        ;
    if (i < MAX_CALLERS) {
        s_w.wait[i].caller = caller;
        s_w.wait[i].ms += us / 1000.0;
        s_w.wait[i].n++;
    }
    LeaveCriticalSection(&s_wait_cs);
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void stats_line(double secs)
{
    unsigned n = s_w.frames < MAX_FRAMES ? s_w.frames : MAX_FRAMES, i, j;
    double sum = 0.0, title_cpu, total_wait = 0.0, sorted[MAX_FRAMES];
    char waits[512];
    int len = 0;

    if (!n || !s_wait_cs_ready)
        return;
    for (i = 0; i < n; i++)
        sum += s_w.frame_ms[i];
    memcpy(sorted, s_w.frame_ms, n * sizeof sorted[0]);
    qsort(sorted, n, sizeof sorted[0], cmp_double);
    title_cpu = title_cpu_ms() - s_w.title_cpu0;
    EnterCriticalSection(&s_wait_cs);
    for (i = 0; i < MAX_CALLERS && s_w.wait[i].n; i++)
        total_wait += s_w.wait[i].ms;
    waits[0] = 0;
    for (j = 0; j < 4; j++) {          /* the four costliest callers */
        int best = -1;
        for (i = 0; i < MAX_CALLERS && s_w.wait[i].n; i++)
            if (s_w.wait[i].ms > 0 && (best < 0 || s_w.wait[i].ms > s_w.wait[best].ms))
                best = (int)i;
        if (best < 0)
            break;
        len += snprintf(waits + len, sizeof waits - (size_t)len, "%s%08X %.2f ms (%.1fx)",
                        j ? ", " : "", s_w.wait[best].caller, s_w.wait[best].ms / s_w.frames,
                        (double)s_w.wait[best].n / s_w.frames);
        s_w.wait[best].ms = -s_w.wait[best].ms;   /* shown */
    }
    memset(s_w.wait, 0, sizeof s_w.wait);
    LeaveCriticalSection(&s_wait_cs);
    fprintf(stderr, "[FRAMES] last %.1f s: %u frames, %.2f ms each (p50 %.2f, p95 %.2f, p99 %.2f,"
            " max %.1f); title: CPU %.2f ms, kernel waits %.2f ms a frame -- %s; executor: CPU"
            " %.2f ms, waiting for the title %.2f, flip pacing %.2f, Present %.2f; %.0f draws a"
            " frame\n", secs, s_w.frames, sum / n, sorted[n / 2], sorted[n * 95 / 100],
            sorted[n * 99 / 100], sorted[n - 1], title_cpu / s_w.frames, total_wait / s_w.frames,
            waits, s_w.exec_cpu / s_w.frames, s_w.exec_idle / s_w.frames, s_w.pacing / s_w.frames,
            s_w.present / s_w.frames, (double)s_w.draws / s_w.frames);
    fflush(stderr);
}

/* At every flip, on the executor's thread. */
void xbox_FrameTraceFrame(void)
{
    static double last_us, window_us;
    double now = xbox_FrameTraceNowUs();

    if (!s_exec_tid)
        s_exec_tid = GetCurrentThreadId();
    if (!s_first_frame) {
        s_first_frame = now;
        window_us = now;
        s_w.title_cpu0 = title_cpu_ms();
    }
    if (last_us && s_w.frames < MAX_FRAMES)
        s_w.frame_ms[s_w.frames] = (now - last_us) / 1000.0;
    if (last_us)
        s_w.frames++;
    last_us = now;
    xbox_FrameTraceSpan("flip", now, -1.0, s_w.frames, 0);

    if (s_trace && !s_trace_done) {
        if (!s_capturing) {
            int go = s_trigger ? trigger_exists()
                               : now - s_first_frame >= s_trace_at * 1e6;
            if (go && !s_ev)
                s_ev = (Event *)malloc(sizeof(Event) * MAX_EVENTS);
            if (go && s_ev) {
                s_trace_start = now;
                InterlockedExchange(&s_capturing, 1);
                fprintf(stderr, "[TRACE] recording %.1f s\n", s_trace_for);
                fflush(stderr);
            }
        } else if (now - s_trace_start >= s_trace_for * 1e6) {
            InterlockedExchange(&s_capturing, 0);
            s_trace_end = now;
            s_trace_done = 1;
            Sleep(5);                  /* let a span being written land */
            trace_write();
        }
    }
    if (now - window_us >= 10e6) {
        stats_line((now - window_us) / 1e6);   /* clears the waits */
        s_w.frames = s_w.draws = 0;
        s_w.exec_cpu = s_w.exec_idle = s_w.pacing = s_w.present = 0.0;
        s_w.title_cpu0 = title_cpu_ms();
        window_us = now;
    }
}
