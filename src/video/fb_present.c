/**
 * Show the guest framebuffer in a window.
 *
 * The title renders into its own framebuffer in guest RAM and tells the kernel
 * where it is through AvSetDisplayMode; on hardware the CRTC scans that memory
 * out. Nothing here scans anything out, so however much of the GPU is
 * implemented, none of it is observable. This is the other half: a window that
 * reads that memory and puts it on screen.
 *
 * Deliberately plain GDI rather than the D3D8 layer. The point is to display
 * whatever the guest actually wrote, so the fewer stages between guest memory
 * and the screen the better -- and it must keep working while the D3D8 layer
 * is busy with something else, such as the FMV player's own window.
 *
 * Off unless RECOMP_FB_WINDOW is set.
 *
 * The window is Win32 on Windows and SDL2 elsewhere; everything else here --
 * the frame copy at the flip, the title, the keys, the BMP dump -- is shared.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>   /* _exit */
#endif

#if defined(_WIN32)
#include <windows.h>
#define FB_WINDOW 1
typedef wchar_t fb_char;
#elif defined(__has_include)
#if __has_include(<SDL.h>)
#include <SDL.h>
#include "platform/xbox_winnt.h"
#define FB_WINDOW 1
typedef char fb_char;
#endif
#endif

#if FB_WINDOW

extern ptrdiff_t xbox_GetMemoryOffset(void);

static volatile LONG s_fb_running;
static uint32_t      s_fb_va, s_fb_pitch, s_fb_width = 640, s_fb_height = 480;
static uint32_t     *s_rgb;           /* converted 32-bit copy for GDI */

/* A finished frame, taken at the flip and shown until the next one.
 *
 * The window used to convert straight out of guest memory every 16 ms. Even
 * pointed at the buffer the title had just finished, that races the executor
 * drawing the next frame into the other one and, whenever the two swap, puts
 * a half-drawn image on the screen -- which is the flicker. Copying the
 * finished frame once per flip means the window never reads memory the
 * rasteriser is writing, so what it shows cannot be half of anything.
 *
 * Two buffers and an index, swapped after the copy completes, so the window
 * thread is never reading the one being filled. */
static uint32_t     *s_present[2];
static volatile LONG s_present_idx = -1;   /* -1 until the first flip */

/* The window, for the GPU backend to present into (nv2a_gpu_present), and
 * whether it now does. Once it does, this thread stops drawing with GDI --
 * two presenters on one window flicker -- and only keeps the copy that
 * RECOMP_FB_DUMP reads, which the executor still refreshes now and then. */
static void         *s_hwnd;          /* the HWND, or the SDL_Window */
static volatile LONG s_gpu_owned;

void *xbox_FramebufferWindowHwnd(void)
{
    return s_fb_running ? (void *)s_hwnd : NULL;
}

void xbox_FramebufferWindowGpuOwned(int on)
{
    InterlockedExchange(&s_gpu_owned, on ? 1 : 0);
}

/* The GPU backend will not present after all (no device, no swap chain).
 * Only the SDL window needs to know: with RECOMP_GPU=1 it is made for Vulkan
 * and leaves the drawing to the GPU, and this is its cue to draw the software
 * frames itself instead. */
static volatile LONG s_gpu_unavailable;

void xbox_FramebufferWindowGpuUnavailable(void)
{
    InterlockedExchange(&s_gpu_unavailable, 1);
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    /* RECOMP_FB_VA pins the window to one guest address instead of following
     * whichever surface is being drawn into. A black window cannot distinguish
     * "the read path is broken" from "the title rendered black", and pointing
     * it at memory known to have content settles that. */
    const char *pin = getenv("RECOMP_FB_VA");

    s_fb_va = pin ? (uint32_t)strtoul(pin, NULL, 0) : fb_va;
    if (pitch)
        s_fb_pitch = pitch;
}

/* Called by the pushbuffer executor when the title flips. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    const uint8_t *src;
    LONG next;
    uint32_t bpp, x, y;

    if (!s_fb_running || !fb_va || !pitch)
        return;
    if (getenv("RECOMP_FB_VA"))
        return;                       /* pinned: leave the old path alone */
    next = (s_present_idx == 0) ? 1 : 0;
    if (!s_present[next]) {
        s_present[next] = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height,
                                             4);
        if (!s_present[next])
            return;
    }
    bpp = pitch / s_fb_width;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * pitch;
        uint32_t *dst = s_present[next] + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
    /* Published only once it is whole. */
    InterlockedExchange(&s_present_idx, next);
}

/* Which keys are down, for the pad stand-in in src/input.
 *
 * GetAsyncKeyState looked like the cheaper way to ask and does not work
 * here: it reads a state Wine keeps for the X server, and a guest process
 * drawing through GDI never sees it change. The window that has the focus
 * is the thing that receives the keys, so that is what has to remember
 * them.
 *
 * Reading this needs no lock. Each entry is written only by the window
 * thread and read only by the USB thread, one byte at a time, and a press
 * seen a frame late is indistinguishable from one made a frame later. */
static volatile unsigned char s_key_down[256];

/* The mouse, for the same stand-in. Buttons go into s_key_down under their
 * Win32 virtual keys (VK_LBUTTON 0x01, VK_RBUTTON 0x02, VK_MBUTTON 0x04,
 * VK_XBUTTON1/2 0x05/0x06), so a binding names a button as it names a key;
 * the wheel is two codes Windows leaves unassigned, one notch one press;
 * motion is summed here until the input side takes it.
 *
 * All of it only while the window holds the mouse. A click in the window
 * takes it -- pointer hidden and kept inside -- and losing the focus gives
 * it back, so Alt+Tab is the way out. The click that takes the mouse is not
 * a press: it would otherwise attack. */
#define FB_VK_WHEEL_UP   0x0A
#define FB_VK_WHEEL_DOWN 0x0B
static volatile LONG s_mouse_held;
static volatile LONG s_mouse_dx, s_mouse_dy;
static volatile LONG s_wheel[2];      /* notches not yet pressed: up, down */

/* In src/input: whether the settings use the mouse at all (input = controller
 * leaves it alone). */
extern int xbox_InputUsesMouse(void);

static void fb_mouse_add(long dx, long dy)
{
    if (s_mouse_held) {
        InterlockedExchangeAdd(&s_mouse_dx, (LONG)dx);
        InterlockedExchangeAdd(&s_mouse_dy, (LONG)dy);
    }
}

static void fb_wheel_add(int down)
{
    /* A fast spin is a handful of presses, not a queue that runs for
     * seconds after the wheel stops. */
    if (s_mouse_held && s_wheel[down] < 3)
        InterlockedIncrement(&s_wheel[down]);
}

static void fb_mouse_buttons_up(void)
{
    s_key_down[0x01] = s_key_down[0x02] = s_key_down[0x04] = 0;
    s_key_down[0x05] = s_key_down[0x06] = 0;
    InterlockedExchange(&s_wheel[0], 0);
    InterlockedExchange(&s_wheel[1], 0);
    InterlockedExchange(&s_mouse_dx, 0);
    InterlockedExchange(&s_mouse_dy, 0);
}

void xbox_FramebufferMouseTake(int *dx, int *dy)
{
    *dx = (int)InterlockedExchange(&s_mouse_dx, 0);
    *dy = (int)InterlockedExchange(&s_mouse_dy, 0);
}

/* The frame counter in the corner, F1 to show or hide (RECOMP_SHOW_FPS=1
 * starts with it shown). The GPU backend draws it at the present; this
 * only keeps the number, measured by the window thread with the title's. */
static volatile LONG s_show_fps = -1;
static volatile LONG s_fps_now;

int xbox_FramebufferOverlayFps(void)
{
    if (s_show_fps < 0) {
        const char *e = getenv("RECOMP_SHOW_FPS");
        InterlockedExchange(&s_show_fps, (e && *e == '1') ? 1 : 0);
    }
    return s_show_fps ? (int)s_fps_now : -1;
}

static void fb_toggle_fps(void)
{
    xbox_FramebufferOverlayFps();
    InterlockedExchange(&s_show_fps, !s_show_fps);
}

/* F2 holds the frame rate at 30 instead of 60, for comparing the two
 * (RECOMP_FPS30=1 starts at 30): the swap chain presents on every second
 * vblank and flip_pace's cap halves. */
static volatile LONG s_half_rate = -1;

int xbox_FramebufferHalfRate(void)
{
    if (s_half_rate < 0) {
        const char *e = getenv("RECOMP_FPS30");
        InterlockedExchange(&s_half_rate, (e && *e == '1') ? 1 : 0);
    }
    return (int)s_half_rate;
}

static void fb_toggle_half_rate(void)
{
    int on = !xbox_FramebufferHalfRate();
    InterlockedExchange(&s_half_rate, on);
    fprintf(stderr, "[FB] F2: frame rate held at %d\n", on ? 30 : 60);
}

/* Title bar, the way ps3recomp's window shows it. Written by the flip,
 * read once a second by the window thread; a torn read shows one stale
 * number for a second, which nobody can tell apart from a real one. */
#if defined(_WIN32)
static fb_char       s_title[48] = L"Xbox Recomp";
#else
static fb_char       s_title[48] = "Xbox Recomp";
#endif
static volatile LONG s_flips;
static volatile LONG s_frame_draws;

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    int i;
    for (i = 0; i < max_chars && i < 47 && name[i]; i++)
#if defined(_WIN32)
        s_title[i] = (wchar_t)name[i];
#else
        s_title[i] = (char)(name[i] < 0x80 ? name[i] : '?');   /* SDL wants UTF-8 */
#endif
    if (i)
        s_title[i] = 0;
}

void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    InterlockedIncrement(&s_flips);
    InterlockedExchange(&s_frame_draws, (LONG)draws);
}

static uint32_t fb_ticks(void);

/* A wheel notch as a press: down 60 ms, so a title reading the pad once a
 * frame sees it, then up 40 ms, so the next notch is a new press. Asked
 * only by the input thread, which owns the two timers. */
static int fb_wheel_pressed(int down)
{
    static uint32_t until[2], gap[2];
    uint32_t now = fb_ticks();

    if ((int32_t)(now - until[down]) < 0)
        return 1;
    if ((int32_t)(now - gap[down]) < 0 || s_wheel[down] <= 0)
        return 0;
    InterlockedDecrement(&s_wheel[down]);
    until[down] = now + 60;
    gap[down] = now + 100;
    return 1;
}

int xbox_FramebufferKeyDown(int vk)
{
    if ((unsigned)vk > 255)
        return 0;
    if (vk == FB_VK_WHEEL_UP || vk == FB_VK_WHEEL_DOWN)
        return fb_wheel_pressed(vk == FB_VK_WHEEL_DOWN);
    return s_key_down[vk] != 0;
}

/* The player closed the window (Alt+F4, the close button, the desktop's
 * quit): end the program. A console title never exits by itself, so
 * stopping only this window's thread left the game running with nothing on
 * screen and no way to close it short of killing the process. Terminated
 * outright rather than through ExitProcess: with the title's threads still
 * running, the graphics and sound libraries' detach code can hang. Shared by
 * the Win32 window (WM_CLOSE) and the SDL one (SDL_QUIT). */
static void fb_player_closed(void)
{
    fprintf(stderr, "[FB] window closed: exiting\n");
    fflush(NULL);
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), 0);
#endif
    _exit(0);
}

#if defined(_WIN32)
static uint32_t fb_ticks(void) { return (uint32_t)GetTickCount(); }
#else
static uint32_t fb_ticks(void) { return (uint32_t)SDL_GetTicks(); }
#endif

#if defined(_WIN32)
/* Take the mouse (on) or give it back. Taken, it is kept inside the client
 * area and hidden (WM_SETCURSOR), and its motion read raw (WM_INPUT), which
 * goes on reporting movement when the pointer is against an edge. */
static void fb_mouse_hold(HWND h, int on)
{
    if (on) {
        RECT r;
        GetClientRect(h, &r);
        MapWindowPoints(h, NULL, (POINT *)&r, 2);
        ClipCursor(&r);
        SetCursor(NULL);
        InterlockedExchange(&s_mouse_held, 1);
    } else if (s_mouse_held) {
        InterlockedExchange(&s_mouse_held, 0);
        ClipCursor(NULL);
        fb_mouse_buttons_up();
    }
}

/* Borderless fullscreen on the window's monitor, and back. Alt+Enter or F11
 * toggles; RECOMP_FULLSCREEN=1 starts that way. The frame keeps the
 * console's 4:3 with black bars either side (nv2a_gpu_present letterboxes
 * into whatever size the window is), so nothing is stretched. */
static RECT s_windowed_rect;
static int  s_fullscreen;

static void fb_set_fullscreen(HWND h, int on)
{
    if (on == s_fullscreen)
        return;
    if (on) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        GetWindowRect(h, &s_windowed_rect);
        if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        SetWindowLongA(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongA(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(h, HWND_NOTOPMOST, s_windowed_rect.left, s_windowed_rect.top,
                     s_windowed_rect.right - s_windowed_rect.left,
                     s_windowed_rect.bottom - s_windowed_rect.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    s_fullscreen = on;
}

static LRESULT CALLBACK fb_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* Alt+Enter arrives as a system key, F11 as an ordinary one; neither is
     * a pad key, so they are taken here and not recorded as held. */
    if ((m == WM_SYSKEYDOWN && w == VK_RETURN && (l & (1 << 29)) && !(l & (1 << 30)))
        || (m == WM_KEYDOWN && w == VK_F11 && !(l & (1 << 30)))) {
        fb_set_fullscreen(h, !s_fullscreen);
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_F1 && !(l & (1 << 30))) {
        fb_toggle_fps();
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_F2 && !(l & (1 << 30))) {
        fb_toggle_half_rate();
        return 0;
    }
    if (m == WM_SYSCHAR && w == VK_RETURN)
        return 0;                       /* no beep for Alt+Enter */
    switch (m) {
    case WM_CLOSE:
        fb_player_closed();
        return 0;
    case WM_DESTROY:
        InterlockedExchange(&s_fb_running, 0);
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if ((unsigned)w < 256)
            s_key_down[w] = 1;
        /* RECOMP_KEY_TRACE: each key as it arrives, edge-triggered.
         *
         * The obvious diagnostic -- sampling which keys are held, once a
         * second, from the input path -- cannot tell a key that was never
         * pressed from one that was tapped: a 100 ms press is caught about
         * one time in ten. That ambiguity is expensive when the only way
         * to test is to ask someone to press a key and describe what
         * happened. This answers "did it arrive" on its own. */
        if (getenv("RECOMP_KEY_TRACE")) {
            static unsigned n;
            if (n++ < 40) {
                fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)w);
                fflush(stderr);
            }
        }
        /* System keys still go to Windows, or Alt+F4 stops closing us. */
        return m == WM_SYSKEYDOWN ? DefWindowProcA(h, m, w, l) : 0;

    case WM_KEYUP:
    case WM_SYSKEYUP:
        if ((unsigned)w < 256)
            s_key_down[w] = 0;
        return m == WM_SYSKEYUP ? DefWindowProcA(h, m, w, l) : 0;

    /* Alt-tabbing away with a key held would leave it held for ever. */
    case WM_KILLFOCUS:
        fb_mouse_hold(h, 0);
        memset((void *)s_key_down, 0, sizeof s_key_down);
        return 0;

    case WM_INPUT: {
        RAWINPUT ri;
        UINT n = sizeof ri;
        if (s_mouse_held
                && GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &n,
                                   sizeof(RAWINPUTHEADER)) != (UINT)-1
                && ri.header.dwType == RIM_TYPEMOUSE
                && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
            fb_mouse_add(ri.data.mouse.lLastX, ri.data.mouse.lLastY);
        break;                          /* DefWindowProc frees the input */
    }
    case WM_LBUTTONDOWN:
        if (!s_mouse_held) {
            if (xbox_InputUsesMouse())
                fb_mouse_hold(h, 1);
            return 0;
        }
        s_key_down[0x01] = 1;
        return 0;
    case WM_LBUTTONUP:   s_key_down[0x01] = 0; return 0;
    case WM_RBUTTONDOWN: s_key_down[0x02] = (unsigned char)s_mouse_held; return 0;
    case WM_RBUTTONUP:   s_key_down[0x02] = 0; return 0;
    case WM_MBUTTONDOWN: s_key_down[0x04] = (unsigned char)s_mouse_held; return 0;
    case WM_MBUTTONUP:   s_key_down[0x04] = 0; return 0;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        s_key_down[HIWORD(w) == XBUTTON1 ? 0x05 : 0x06] =
            (unsigned char)(m == WM_XBUTTONDOWN && s_mouse_held);
        return TRUE;
    case WM_MOUSEWHEEL:
        fb_wheel_add(GET_WHEEL_DELTA_WPARAM(w) < 0);
        return 0;
    case WM_SETCURSOR:
        if (s_mouse_held && LOWORD(l) == HTCLIENT) {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    /* Fullscreen and back, or the window dragged: keep the pointer inside
     * where the window now is. */
    case WM_SIZE:
    case WM_MOVE:
        if (s_mouse_held)
            fb_mouse_hold(h, 1);
        break;
    }
    return DefWindowProcA(h, m, w, l);
}
#endif /* _WIN32 */

/* The display formats an Xbox front buffer is actually set to. The pitch says
 * how wide a row is in bytes, so pitch/width gives the pixel size; the exact
 * component layout only matters for 16-bit, where 5:6:5 and 1:5:5:5 differ. */
static void fb_convert(const uint8_t *src, uint32_t bpp)
{
    uint32_t x, y;

    for (y = 0; y < s_fb_height; y++) {
        const uint8_t *row = src + (size_t)y * s_fb_pitch;
        uint32_t *dst = s_rgb + (size_t)y * s_fb_width;

        if (bpp == 4) {
            memcpy(dst, row, (size_t)s_fb_width * 4);
        } else if (bpp == 2) {
            const uint16_t *p = (const uint16_t *)row;
            for (x = 0; x < s_fb_width; x++) {
                uint16_t v = p[x];
                uint32_t r = (uint32_t)((v >> 11) & 0x1F) * 255u / 31u;
                uint32_t g = (uint32_t)((v >>  5) & 0x3F) * 255u / 63u;
                uint32_t b = (uint32_t)( v        & 0x1F) * 255u / 31u;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        } else {
            memset(dst, 0, (size_t)s_fb_width * 4);
        }
    }
}

/* Write what the window is currently showing to a 24-bit BMP.
 *
 * A black window is ambiguous: it means either that the read path is wrong or
 * that the title really did render black. Dumping the same converted pixels
 * the window draws settles which, and does it without a screenshot. */
int xbox_FramebufferDumpBmp(const char *path)
{
    FILE *f;
    uint32_t row = ((s_fb_width * 3u) + 3u) & ~3u;
    uint32_t img = row * s_fb_height, total = 54u + img, y, x;
    uint8_t hdr[54], *line;

    if (!s_rgb || !s_fb_va)
        return -1;
    f = fopen(path, "wb");
    if (!f)
        return -1;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &s_fb_width, 4);
    memcpy(hdr + 22, &s_fb_height, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    line = (uint8_t *)calloc(1, row);
    for (y = 0; y < s_fb_height; y++) {
        const uint32_t *src = s_rgb + (size_t)(s_fb_height - 1 - y) * s_fb_width;
        for (x = 0; x < s_fb_width; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x] & 0xFF);
            line[x * 3 + 1] = (uint8_t)((src[x] >> 8) & 0xFF);
            line[x * 3 + 2] = (uint8_t)((src[x] >> 16) & 0xFF);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "  [FBWIN] wrote %s (%ux%u from 0x%08X)\n",
            path, s_fb_width, s_fb_height, s_fb_va);
    return 0;
}

#if defined(_WIN32)
static DWORD WINAPI fb_thread(LPVOID unused)
{
    HWND hwnd;
    HDC hdc;
    BITMAPINFO bi;
    RECT r;

    (void)unused;

    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = fb_wndproc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, IDC_ARROW);
        wc.lpszClassName = "XboxRecompFramebuffer";
        RegisterClassA(&wc);
    }
    /* RECOMP_SCALE renders N times the console's resolution (the GPU
     * backend); open the window that large too, shrunk to fit the screen's
     * work area with the console's 4:3 kept. It can be resized freely: the
     * swap chain scales to whatever the window is. */
    {
        const char *e = getenv("RECOMP_SCALE");
        int sc = e ? atoi(e) : 1;
        RECT wa;
        LONG cw, ch;
        if (sc < 1 || sc > 4) sc = 1;
        cw = (LONG)s_fb_width * sc; ch = (LONG)s_fb_height * sc;
        if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
            LONG aw = wa.right - wa.left - 16, ah = wa.bottom - wa.top - 48;
            if (ch > ah) { cw = cw * ah / ch; ch = ah; }
            if (cw > aw) { ch = ch * aw / cw; cw = aw; }
        }
        r.left = 0; r.top = 0; r.right = cw; r.bottom = ch;
    }
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowExA(0, "XboxRecompFramebuffer", "Xbox Recomp - Framebuffer",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd) {
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    hdc = GetDC(hwnd);
    s_hwnd = hwnd;
    if (xbox_InputUsesMouse()) {
        RAWINPUTDEVICE rid;
        rid.usUsagePage = 0x01;         /* generic desktop: mouse */
        rid.usUsage     = 0x02;
        rid.dwFlags     = 0;
        rid.hwndTarget  = hwnd;
        if (!RegisterRawInputDevices(&rid, 1, sizeof rid))
            fprintf(stderr, "  [FBWIN] no raw mouse input (error %lu)\n",
                    (unsigned long)GetLastError());
    }
    {
        const char *fs = getenv("RECOMP_FULLSCREEN");
        if (fs && *fs == '1')
            fb_set_fullscreen(hwnd, 1);
    }

    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = (LONG)s_fb_width;
    bi.bmiHeader.biHeight      = -(LONG)s_fb_height;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);

    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u)\n",
            s_fb_width, s_fb_height);

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        /* Input wakes this loop as it arrives (the wait at the bottom), so
         * keys and mouse motion reach the pad at once instead of in 16 ms
         * batches, which made mouse look judder; the frame work below still
         * runs once every 16 ms. */
        {
            static DWORD last;
            DWORD now = GetTickCount();
            if (now - last < 16) {
                MsgWaitForMultipleObjects(0, NULL, FALSE, 16 - (now - last), QS_ALLINPUT);
                continue;
            }
            last = now;
        }
        if (s_present_idx >= 0 && s_rgb) {
            /* A finished frame, published by the flip. Copied into s_rgb so
             * the dump path and GDI see one consistent image even if the
             * next flip lands mid-blit. */
            LONG idx = s_present_idx;
            if (s_present[idx])
                memcpy(s_rgb, s_present[idx],
                       (size_t)s_fb_width * s_fb_height * 4);
            if (!s_gpu_owned) {
                RECT cr;
                GetClientRect(hwnd, &cr);
                StretchDIBits(hdc, 0, 0, cr.right, cr.bottom,
                              0, 0, (int)s_fb_width, (int)s_fb_height,
                              s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
            }
        } else if (s_fb_va && s_fb_pitch && s_rgb && !s_gpu_owned) {
            /* No flip yet, or pinned with RECOMP_FB_VA: read guest memory as
             * before, which is also what a title that never flips needs. */
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            RECT cr;
            fb_convert(src, s_fb_pitch / s_fb_width);
            GetClientRect(hwnd, &cr);
            StretchDIBits(hdc, 0, 0, cr.right, cr.bottom,
                          0, 0, (int)s_fb_width, (int)s_fb_height,
                          s_rgb, &bi, DIB_RGB_COLORS, SRCCOPY);
        }
        {
            /* One dump a few seconds in, so the title has had time to render
             * something rather than catching the first blank frame.
             *
             * RECOMP_FB_WINDOW_DUMP_EVERY=<frames> dumps repeatedly instead.
             * This window follows the address AvSetDisplayMode gave, which is
             * what the CRTC scans and therefore what a person sees; the
             * pushbuffer executor's own dump follows its draw surface. With
             * double buffering those are different buffers, and measuring
             * progress from the executor's dump reports a blank screen while
             * the window is showing the title's logo. Ask the window. */
            const char *dump = getenv("RECOMP_FB_DUMP");
            const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
            static int frames;
            int period = every ? atoi(every) : 0;
            frames++;
            if (dump && period > 0) {
                if (frames % period == 0)
                    xbox_FramebufferDumpBmp(dump);
            } else if (dump && frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
        {
            static DWORD t0;
            static LONG f0;
            DWORD now = GetTickCount();
            if (now - t0 >= 500) {
                LONG f = s_flips;
                double fps = t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0;
                wchar_t tb[128];
                InterlockedExchange(&s_fps_now, (LONG)(fps + 0.5));
                _snwprintf(tb, 127, L"%ls | FPS: %.1f | draws: %ld", s_title,
                           fps, (long)s_frame_draws);
                tb[127] = 0;
                SetWindowTextW(hwnd, tb);
                t0 = now; f0 = f;
            }
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 16, QS_ALLINPUT);
    }

    s_hwnd = NULL;
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    free(s_rgb);
    s_rgb = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    HANDLE th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = CreateThread(NULL, 0, fb_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}

#else /* SDL2 */

/* SDL keys as the Win32 virtual-key codes the keyboard pad asks about. */
static int fb_vk_from_sdl(SDL_Keycode k)
{
    if (k >= SDLK_a && k <= SDLK_z) return 'A' + (int)(k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return '0' + (int)(k - SDLK_0);
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return 0x61 + (int)(k - SDLK_KP_1);
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + (int)(k - SDLK_F1);
    switch (k) {
    case SDLK_KP_0:      return 0x60;
    case SDLK_KP_ENTER:  return 0x0D;
    case SDLK_CAPSLOCK:  return 0x14;
    case SDLK_PAGEUP:    return 0x21;
    case SDLK_PAGEDOWN:  return 0x22;
    case SDLK_END:       return 0x23;
    case SDLK_HOME:      return 0x24;
    case SDLK_INSERT:    return 0x2D;
    case SDLK_DELETE:    return 0x2E;
    case SDLK_SEMICOLON: return 0xBA;
    case SDLK_EQUALS:    return 0xBB;
    case SDLK_COMMA:     return 0xBC;
    case SDLK_MINUS:     return 0xBD;
    case SDLK_PERIOD:    return 0xBE;
    case SDLK_SLASH:     return 0xBF;
    case SDLK_BACKQUOTE: return 0xC0;
    case SDLK_LEFTBRACKET:  return 0xDB;
    case SDLK_BACKSLASH:    return 0xDC;
    case SDLK_RIGHTBRACKET: return 0xDD;
    case SDLK_QUOTE:     return 0xDE;
    case SDLK_UP:        return 0x26;
    case SDLK_DOWN:      return 0x28;
    case SDLK_LEFT:      return 0x25;
    case SDLK_RIGHT:     return 0x27;
    case SDLK_RETURN:    return 0x0D;
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_ESCAPE:    return 0x1B;
    case SDLK_SPACE:     return 0x20;
    case SDLK_TAB:       return 0x09;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10;
    case SDLK_LCTRL:  case SDLK_RCTRL:  return 0x11;
    case SDLK_LALT:   case SDLK_RALT:   return 0x12;
    default:             return -1;
    }
}

/* The same loop as the Win32 thread: events, show the newest finished frame,
 * dumps, the title once a second. The renderer keeps the console's 4:3 with
 * black bars at any window size (SDL_RenderSetLogicalSize) and presents with
 * vsync, which also paces this thread. Window, renderer and events all stay
 * on this one thread, as SDL requires. */
/* The renderer that shows software frames. Made on first use: with the GPU
 * presenting, an SDL_Renderer (GL) on the window would turn it into a GL
 * window, and DXVK's Vulkan swap chain could not then attach to it. */
static int fb_sdl_renderer(SDL_Window *win, SDL_Renderer **ren, SDL_Texture **tex)
{
    if (*tex)
        return 1;
    *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED
                                     | SDL_RENDERER_PRESENTVSYNC);
    if (*ren && !SDL_RenderSetLogicalSize(*ren, (int)s_fb_width, (int)s_fb_height))
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    *tex = *ren ? SDL_CreateTexture(*ren, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING,
                                    (int)s_fb_width, (int)s_fb_height) : NULL;
    if (!*tex)
        fprintf(stderr, "  [FBWIN] no renderer: %s\n", SDL_GetError());
    return *tex != NULL;
}

static int fb_thread(void *unused)
{
    SDL_Window *win;
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex = NULL;
    const char *ge = getenv("RECOMP_GPU");
    int gpu = ge && *ge == '1';
    int cw, ch;

    (void)unused;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "  [FBWIN] SDL video: %s\n", SDL_GetError());
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    {
        const char *e = getenv("RECOMP_SCALE");
        int sc = e ? atoi(e) : 1;
        SDL_Rect wa;
        if (sc < 1 || sc > 4) sc = 1;
        cw = (int)s_fb_width * sc; ch = (int)s_fb_height * sc;
        if (SDL_GetDisplayUsableBounds(0, &wa) == 0) {
            int aw = wa.w - 16, ah = wa.h - 48;
            if (ch > ah) { cw = cw * ah / ch; ch = ah; }
            if (cw > aw) { ch = ch * aw / cw; cw = aw; }
        }
    }
    win = SDL_CreateWindow("Xbox Recomp - Framebuffer", SDL_WINDOWPOS_UNDEFINED,
                           SDL_WINDOWPOS_UNDEFINED, cw, ch,
                           SDL_WINDOW_RESIZABLE | (gpu ? SDL_WINDOW_VULKAN : 0));
    if (!win || (!gpu && !fb_sdl_renderer(win, &ren, &tex))) {
        fprintf(stderr, "  [FBWIN] no window: %s\n", SDL_GetError());
        if (win) SDL_DestroyWindow(win);
        InterlockedExchange(&s_fb_running, 0);
        return 0;
    }
    s_hwnd = win;
    {
        const char *fs = getenv("RECOMP_FULLSCREEN");
        if (fs && *fs == '1')
            SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    s_rgb = (uint32_t *)calloc((size_t)s_fb_width * s_fb_height, 4);
    fprintf(stderr, "  [FBWIN] framebuffer window open (%ux%u, SDL %s)\n",
            s_fb_width, s_fb_height, SDL_GetCurrentVideoDriver());

    while (InterlockedCompareExchange(&s_fb_running, 1, 1)) {
        SDL_Event ev;
        int have = 0;

        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                fb_player_closed();
            } else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
                SDL_Keycode k = ev.key.keysym.sym;
                int vk;
                /* Alt+Enter or F11: fullscreen, not a pad key. */
                if (ev.type == SDL_KEYDOWN && !ev.key.repeat
                        && (k == SDLK_F11 || (k == SDLK_RETURN
                                              && (ev.key.keysym.mod & KMOD_ALT)))) {
                    int on = !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN);
                    SDL_SetWindowFullscreen(win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    continue;
                }
                if (k == SDLK_F1) {
                    if (ev.type == SDL_KEYDOWN && !ev.key.repeat)
                        fb_toggle_fps();
                    continue;
                }
                if (k == SDLK_F2) {
                    if (ev.type == SDL_KEYDOWN && !ev.key.repeat)
                        fb_toggle_half_rate();
                    continue;
                }
                vk = fb_vk_from_sdl(k);
                if (vk >= 0)
                    s_key_down[vk] = ev.type == SDL_KEYDOWN;
                if (ev.type == SDL_KEYDOWN && getenv("RECOMP_KEY_TRACE")) {
                    static unsigned n;
                    if (n++ < 40)
                        fprintf(stderr, "  [KEY] down vk=0x%02X\n", (unsigned)vk);
                }
            } else if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) {
                /* SDL's left, middle, right, X1, X2 as the Win32 keys */
                static const int vk_of[6] = { -1, 0x01, 0x04, 0x02, 0x05, 0x06 };
                int down = ev.type == SDL_MOUSEBUTTONDOWN;
                if (down && !s_mouse_held) {
                    if (ev.button.button == SDL_BUTTON_LEFT && xbox_InputUsesMouse()
                            && SDL_SetRelativeMouseMode(SDL_TRUE) == 0)
                        InterlockedExchange(&s_mouse_held, 1);
                    continue;
                }
                if (ev.button.button < 6 && vk_of[ev.button.button] >= 0)
                    s_key_down[vk_of[ev.button.button]] = (unsigned char)down;
            } else if (ev.type == SDL_MOUSEMOTION) {
                fb_mouse_add(ev.motion.xrel, ev.motion.yrel);
            } else if (ev.type == SDL_MOUSEWHEEL) {
                int y = ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -ev.wheel.y
                                                                     : ev.wheel.y;
                if (y)
                    fb_wheel_add(y < 0);
            } else if (ev.type == SDL_WINDOWEVENT
                       && ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                if (s_mouse_held) {
                    InterlockedExchange(&s_mouse_held, 0);
                    SDL_SetRelativeMouseMode(SDL_FALSE);
                    fb_mouse_buttons_up();
                }
                memset((void *)s_key_down, 0, sizeof s_key_down);
            }
        }
        /* As the Win32 loop: input wakes it at once, the frame work runs
         * every 16 ms. Only while the GPU presents -- drawing the software
         * frames, vsync paces this loop instead. */
        if (s_gpu_owned || (gpu && !s_gpu_unavailable)) {
            static Uint32 last;
            Uint32 now = SDL_GetTicks();
            if (now - last < 16) {
                SDL_WaitEventTimeout(NULL, (int)(16 - (now - last)));
                continue;
            }
            last = now;
        }
        if (s_present_idx >= 0 && s_rgb) {
            LONG idx = s_present_idx;
            if (s_present[idx])
                memcpy(s_rgb, s_present[idx],
                       (size_t)s_fb_width * s_fb_height * 4);
            have = 1;
        } else if (s_fb_va && s_fb_pitch && s_rgb) {
            const uint8_t *src =
                (const uint8_t *)((uintptr_t)s_fb_va + xbox_GetMemoryOffset());
            fb_convert(src, s_fb_pitch / s_fb_width);
            have = 1;
        }
        if (have && !s_gpu_owned && (!gpu || s_gpu_unavailable)
                && fb_sdl_renderer(win, &ren, &tex)) {
            /* The frame is X8R8G8B8 with the X byte whatever the title left
             * there; ARGB8888 with blending off ignores it. Vsync paces. */
            SDL_UpdateTexture(tex, NULL, s_rgb, (int)s_fb_width * 4);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);
        } else {
            SDL_WaitEventTimeout(NULL, 16);   /* the GPU presents, or nothing to show */
        }
        {
            const char *dump = getenv("RECOMP_FB_DUMP");
            const char *every = getenv("RECOMP_FB_WINDOW_DUMP_EVERY");
            static int frames;
            int period = every ? atoi(every) : 0;
            frames++;
            if (dump && *dump && period > 0) {
                if (frames % period == 0)
                    xbox_FramebufferDumpBmp(dump);
            } else if (dump && *dump && frames == 600) {
                xbox_FramebufferDumpBmp(dump);
            }
        }
        {
            static Uint32 t0;
            static LONG f0;
            Uint32 now = SDL_GetTicks();
            if (now - t0 >= 500) {
                LONG f = s_flips;
                double fps = t0 ? (f - f0) * 1000.0 / (now - t0) : 0.0;
                char tb[128];
                InterlockedExchange(&s_fps_now, (LONG)(fps + 0.5));
                snprintf(tb, sizeof tb, "%s | FPS: %.1f | draws: %ld", s_title,
                         fps, (long)s_frame_draws);
                SDL_SetWindowTitle(win, tb);
                t0 = now; f0 = f;
            }
        }
    }

    s_hwnd = NULL;
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    free(s_rgb);
    s_rgb = NULL;
    return 0;
}

void xbox_FramebufferWindowStart(void)
{
    SDL_Thread *th;

    if (!getenv("RECOMP_FB_WINDOW"))
        return;
    if (InterlockedCompareExchange(&s_fb_running, 1, 0) != 0)
        return;
    th = SDL_CreateThread(fb_thread, "fb_window", NULL);
    if (th)
        SDL_DetachThread(th);
    else
        InterlockedExchange(&s_fb_running, 0);
}
#endif /* SDL2 */

#else /* no window on this host */
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowStart(void) {}
int xbox_FramebufferKeyDown(int vk) { (void)vk; return 0; }
void xbox_FramebufferMouseTake(int *dx, int *dy) { *dx = *dy = 0; }
int xbox_FramebufferOverlayFps(void) { return -1; }
int xbox_FramebufferHalfRate(void) { return 0; }
void xbox_FramebufferWindowSetTitle(const uint16_t *n, int m) { (void)n; (void)m; }
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void *xbox_FramebufferWindowHwnd(void) { return (void *)0; }
void xbox_FramebufferWindowGpuOwned(int on) { (void)on; }
void xbox_FramebufferWindowGpuUnavailable(void) {}
int xbox_FramebufferDumpBmp(const char *path) { (void)path; return -1; }
#endif
