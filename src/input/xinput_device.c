/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput
 *   POSIX  -> SDL2 GameController
 */

#include "xinput_xbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <time.h>
#endif

/* ---- keyboard and mouse ---------------------------------------------------
 *
 * A title that waits on PRESS START is unreachable on a machine with no
 * controller plugged in, which is most machines someone brings this up on --
 * and once it is played rather than looked at, the keyboard and mouse are
 * how a PC player expects to play it. So they stand in for pad 1: each
 * binding names the pad control it presses, and the mouse turns the right
 * stick (the camera, in every title that has one).
 *
 * RECOMP_KEYBOARD=1 turns this on. RECOMP_INPUT picks what plays:
 *   auto        keyboard, mouse and a pad all at once (the default): the
 *               keyboard is merged on top of a pad on port 0, so either
 *               works at any moment
 *   controller  the pad only; the keyboard and mouse do nothing
 *   keyboard    keyboard and mouse only; a pad on port 0 is ignored (one
 *               with a drifting stick would otherwise move the player)
 *
 * The bindings are RECOMP_BIND_<CONTROL>=<key>[,<key>...], e.g.
 * RECOMP_BIND_A=Space,MouseRight; "none" leaves a control unbound. The
 * defaults below are the WASD-and-mouse layout most PC action games use.
 *
 * Keys come from the framebuffer window, which only receives them while it
 * has the focus, so typing in another window does not drive the game. Shared
 * by both backends: the window is Win32 or SDL2, the keys the same (Win32
 * virtual-key codes; the SDL window translates).
 */
enum { PLAY_AUTO, PLAY_CONTROLLER, PLAY_KEYBOARD };

static int input_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *v = getenv("RECOMP_INPUT");
        mode = PLAY_AUTO;
        if (v && (!strcmp(v, "controller") || !strcmp(v, "gamepad") || !strcmp(v, "pad")))
            mode = PLAY_CONTROLLER;
        else if (v && !strcmp(v, "keyboard"))
            mode = PLAY_KEYBOARD;
    }
    return mode;
}

static BOOL keyboard_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_KEYBOARD");
        on = (v && *v && *v != '0' && input_mode() != PLAY_CONTROLLER) ? 1 : 0;
    }
    return on ? TRUE : FALSE;
}

/* For the window: whether a click should take the mouse. */
int xbox_InputUsesMouse(void)
{
    return keyboard_enabled() ? 1 : 0;
}

/* The framebuffer window records what is held; see src/video/fb_present.c.
 *
 * GetAsyncKeyState was the first attempt and reads nothing here -- under
 * Wine it answers about a state a GDI-drawing guest never touches. Going
 * through the window is also the better gate: a window only receives keys
 * while it has the focus, so there is no separate focus check to get wrong
 * and no way for typing in another application to drive the game.
 *
 * Declared rather than included so src/input does not depend on the video
 * module's include path. */
extern int xbox_FramebufferKeyDown(int vk);
extern void xbox_FramebufferMouseTake(int *dx, int *dy);

static BOOL key_down(int vk)
{
    return xbox_FramebufferKeyDown(vk) ? TRUE : FALSE;
}

/* Key names for the bindings, case-insensitive, as Win32 virtual keys.
 * Letters and digits are their own codes and are not listed. F1 (frame
 * counter), F2 (30 or 60 fps) and F11 (fullscreen) are the window's. */
static const struct { const char *name; int vk; } s_key_names[] = {
    {"MouseLeft", 0x01}, {"MouseRight", 0x02}, {"MouseMiddle", 0x04},
    {"Mouse4", 0x05}, {"Mouse5", 0x06},
    {"WheelUp", 0x0A}, {"WheelDown", 0x0B},     /* fb_present.c's codes */
    {"Backspace", 0x08}, {"Tab", 0x09}, {"Enter", 0x0D}, {"Return", 0x0D},
    {"Shift", 0x10}, {"Ctrl", 0x11}, {"Control", 0x11}, {"Alt", 0x12},
    {"CapsLock", 0x14}, {"Escape", 0x1B}, {"Esc", 0x1B}, {"Space", 0x20},
    {"PageUp", 0x21}, {"PageDown", 0x22}, {"End", 0x23}, {"Home", 0x24},
    {"Left", 0x25}, {"Up", 0x26}, {"Right", 0x27}, {"Down", 0x28},
    {"Insert", 0x2D}, {"Delete", 0x2E},
    {"Numpad0", 0x60}, {"Numpad1", 0x61}, {"Numpad2", 0x62}, {"Numpad3", 0x63},
    {"Numpad4", 0x64}, {"Numpad5", 0x65}, {"Numpad6", 0x66}, {"Numpad7", 0x67},
    {"Numpad8", 0x68}, {"Numpad9", 0x69},
    {"F3", 0x72}, {"F4", 0x73}, {"F5", 0x74}, {"F6", 0x75},
    {"F7", 0x76}, {"F8", 0x77}, {"F9", 0x78}, {"F10", 0x79}, {"F12", 0x7B},
    {"Semicolon", 0xBA}, {"Equals", 0xBB}, {"Comma", 0xBC}, {"Minus", 0xBD},
    {"Period", 0xBE}, {"Slash", 0xBF}, {"Grave", 0xC0}, {"LeftBracket", 0xDB},
    {"Backslash", 0xDC}, {"RightBracket", 0xDD}, {"Apostrophe", 0xDE},
};

static int same_name(const char *a, size_t n, const char *b)
{
    size_t i;
    for (i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (!y)
            return 0;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y)
            return 0;
    }
    return b[n] == 0;
}

static int key_from_name(const char *s, size_t n)
{
    size_t i;
    if (n == 1) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return c;
    }
    for (i = 0; i < sizeof s_key_names / sizeof s_key_names[0]; i++)
        if (same_name(s, n, s_key_names[i].name))
            return s_key_names[i].vk;
    return -1;
}

/* What a binding presses: a bit of the digital byte, an analog button at
 * full pressure, or one direction of a stick at full throw. */
enum { BIND_DIGITAL, BIND_ANALOG, BIND_STICK };
enum { AXIS_LX, AXIS_LY, AXIS_RX, AXIS_RY };
#define BIND_KEYS 4

typedef struct {
    const char *name;        /* RECOMP_BIND_<NAME>, settings.ini bind_<name> */
    int kind;
    int what;                /* the bit, the analog index, or axis*2 + (dir > 0) */
    const char *keys;        /* the default */
    int vk[BIND_KEYS];
} Binding;

/* The defaults follow what each control does in Alter Echo, read off the
 * game's own move list: X attacks (and fires, in gun form), Y is the
 * special, A jumps, B smashes; clicking the left stick blocks and with A
 * rolls, so Shift blocks and Shift+Space rolls; the triggers change form;
 * Black cycles guns. */
static Binding s_binds[] = {
    {"A",            BIND_ANALOG,  XBOX_BUTTON_A,        "Space",        {0}},
    {"B",            BIND_ANALOG,  XBOX_BUTTON_B,        "F",            {0}},
    {"X",            BIND_ANALOG,  XBOX_BUTTON_X,        "MouseLeft",    {0}},
    {"Y",            BIND_ANALOG,  XBOX_BUTTON_Y,        "MouseRight",   {0}},
    {"BLACK",        BIND_ANALOG,  XBOX_BUTTON_BLACK,    "WheelUp,WheelDown,Tab", {0}},
    {"WHITE",        BIND_ANALOG,  XBOX_BUTTON_WHITE,    "C",            {0}},
    {"LT",           BIND_ANALOG,  XBOX_BUTTON_LTRIGGER, "Q",            {0}},
    {"RT",           BIND_ANALOG,  XBOX_BUTTON_RTRIGGER, "E",            {0}},
    {"START",        BIND_DIGITAL, XBOX_GAMEPAD_START,   "Enter,Escape", {0}},
    {"BACK",         BIND_DIGITAL, XBOX_GAMEPAD_BACK,    "Backspace",    {0}},
    {"DPAD_UP",      BIND_DIGITAL, XBOX_GAMEPAD_DPAD_UP,    "Up",        {0}},
    {"DPAD_DOWN",    BIND_DIGITAL, XBOX_GAMEPAD_DPAD_DOWN,  "Down",      {0}},
    {"DPAD_LEFT",    BIND_DIGITAL, XBOX_GAMEPAD_DPAD_LEFT,  "Left",      {0}},
    {"DPAD_RIGHT",   BIND_DIGITAL, XBOX_GAMEPAD_DPAD_RIGHT, "Right",     {0}},
    {"LSTICK_CLICK", BIND_DIGITAL, XBOX_GAMEPAD_LEFT_THUMB,  "Shift",    {0}},
    {"RSTICK_CLICK", BIND_DIGITAL, XBOX_GAMEPAD_RIGHT_THUMB, "MouseMiddle,R", {0}},
    {"MOVE_UP",      BIND_STICK,   AXIS_LY * 2 + 1, "W", {0}},
    {"MOVE_DOWN",    BIND_STICK,   AXIS_LY * 2,     "S", {0}},
    {"MOVE_LEFT",    BIND_STICK,   AXIS_LX * 2,     "A", {0}},
    {"MOVE_RIGHT",   BIND_STICK,   AXIS_LX * 2 + 1, "D", {0}},
    {"LOOK_UP",      BIND_STICK,   AXIS_RY * 2 + 1, "I", {0}},
    {"LOOK_DOWN",    BIND_STICK,   AXIS_RY * 2,     "K", {0}},
    {"LOOK_LEFT",    BIND_STICK,   AXIS_RX * 2,     "J", {0}},
    {"LOOK_RIGHT",   BIND_STICK,   AXIS_RX * 2 + 1, "L", {0}},
};
#define NBINDS ((int)(sizeof s_binds / sizeof s_binds[0]))

static void binds_parse(Binding *b, const char *s)
{
    int n = 0;
    memset(b->vk, 0, sizeof b->vk);
    while (*s) {
        const char *e = s;
        int vk;
        while (*e && *e != ',' && *e != ' ')
            e++;
        if (e > s && !same_name(s, (size_t)(e - s), "none")) {
            vk = key_from_name(s, (size_t)(e - s));
            if (vk < 0)
                fprintf(stderr, "  [INPUT] bind_%s: no key called '%.*s' (ignored)\n",
                        b->name, (int)(e - s), s);
            else if (n < BIND_KEYS)
                b->vk[n++] = vk;
        }
        s = *e ? e + 1 : e;
    }
}

static void binds_load(void)
{
    static int done;
    int i;
    if (done)
        return;
    done = 1;
    for (i = 0; i < NBINDS; i++) {
        char var[48];
        const char *v;
        snprintf(var, sizeof var, "RECOMP_BIND_%s", s_binds[i].name);
        v = getenv(var);
        binds_parse(&s_binds[i], v ? v : s_binds[i].keys);
    }
}

static BOOL bind_held(const Binding *b)
{
    int k;
    for (k = 0; k < BIND_KEYS && b->vk[k]; k++)
        if (key_down(b->vk[k]))
            return TRUE;
    return FALSE;
}

/* ---- the mouse as the right stick ----
 *
 * A stick sets a turning speed; a mouse moves a distance. The motion goes
 * into a store that the stick drains over a few hundredths of a second, so
 * moving the mouse further turns further and stopping it stops the turn
 * almost at once. Fast flicks clip at the stick's full throw, and what the
 * stick could not express is dropped rather than turned later.
 *
 * RECOMP_MOUSE_SENS [1]: 0 turns mouse look off. At 1, about 2500 counts
 * a second -- a brisk move on an 800 dpi mouse -- is the stick fully over.
 * RECOMP_MOUSE_INVERT=1 swaps up and down (the game's own Pitch Axis option
 * inverts the stick and the mouse alike).
 * RECOMP_MOUSE_DEADZONE [0.40]: the part of the stick's travel the title
 * ignores; the smallest mouse motion starts there, so slow aiming still
 * turns. 0.40 is Alter Echo's, measured with a steady fake motion and the
 * scene's shift between timed frames: the camera stood still up to 0.40 of
 * full throw and turned about in proportion past it. */
#define MOUSE_FULL_RATE 2500.0
#define MOUSE_DRAIN_S   0.025

static double env_double(const char *name, double def)
{
    const char *v = getenv(name);
    return (v && *v) ? atof(v) : def;
}

static double now_seconds(void)
{
#if defined(_WIN32)
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

/* One axis: the store in counts, drained by what the stick shows. */
static SHORT mouse_axis(double *store, double add, double dt, double sens, double dz)
{
    double rate, s, out, keep;

    *store += add;
    rate = *store / MOUSE_DRAIN_S;               /* counts a second */
    s = rate * sens / MOUSE_FULL_RATE;           /* of full throw */
    if (s > 1.0) s = 1.0;
    if (s < -1.0) s = -1.0;
    if (s > -0.03 && s < 0.03) {
        *store = 0.0;                            /* the tail: done */
        return 0;
    }
    if (dt >= MOUSE_DRAIN_S)
        *store = 0.0;                            /* a long poll gap drains it all */
    else
        *store -= s * MOUSE_FULL_RATE / sens * dt;
    keep = MOUSE_FULL_RATE / sens * MOUSE_DRAIN_S;   /* no more than a full-throw drain's worth */
    if (*store > keep) *store = keep;
    if (*store < -keep) *store = -keep;
    out = (s > 0 ? 1.0 : -1.0) * (dz + (1.0 - dz) * (s > 0 ? s : -s));
    return (SHORT)(out * 32767.0);
}

static void mouse_stick(SHORT *rx, SHORT *ry)
{
    static double sens = -1.0, dz, last, sx, sy, fake_x, fake_y;
    static int invert;
    double now = now_seconds(), dt;
    int dx, dy;

    if (sens < 0.0) {
        sens = env_double("RECOMP_MOUSE_SENS", 1.0);
        dz = env_double("RECOMP_MOUSE_DEADZONE", 0.40);
        invert = env_double("RECOMP_MOUSE_INVERT", 0.0) != 0.0;
        if (dz < 0.0) dz = 0.0;
        if (dz > 0.9) dz = 0.9;
    }
    /* RECOMP_MOUSE_FAKE=<x>,<y>: steady motion in counts a second, as if
     * the mouse were moving -- for testing the turn without one. With
     * =@<file> the two numbers are read from the file twice a second, so
     * one run can step through speeds. */
    {
        static double next;
        const char *f = getenv("RECOMP_MOUSE_FAKE");
        if (f && now >= next) {
            char buf[64] = "";
            next = now + 0.5;
            if (*f == '@') {
                FILE *fp = fopen(f + 1, "r");
                if (fp) {
                    if (!fgets(buf, sizeof buf, fp))
                        buf[0] = 0;
                    fclose(fp);
                }
                f = buf;
            }
            fake_x = atof(f);
            f = strchr(f, ',');
            fake_y = f ? atof(f + 1) : 0.0;
        }
    }
    xbox_FramebufferMouseTake(&dx, &dy);
    dt = last > 0.0 ? now - last : 0.0;
    last = now;
    if (dt > 0.1)
        dt = 0.1;                    /* a stall is not a long, fast move */
    if (sens <= 0.0)
        return;
    *rx = mouse_axis(&sx, (double)dx + fake_x * dt, dt, sens, dz);
    *ry = mouse_axis(&sy, (double)(invert ? dy : -dy) - fake_y * dt, dt, sens, dz);
}

static SHORT stick_sum(int v)
{
    return (SHORT)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
}

/* The title's test for "the D-pad is the stick now", or NULL; and the
 * D-pad bit for each left-stick binding (axis * 2 + positive). */
static int (*s_move_as_dpad)(void);
static const WORD s_move_dpad_bit[4] = {
    XBOX_GAMEPAD_DPAD_LEFT, XBOX_GAMEPAD_DPAD_RIGHT,
    XBOX_GAMEPAD_DPAD_DOWN, XBOX_GAMEPAD_DPAD_UP,
};

void xbox_InputSetMoveKeysAsDpad(int (*active)(void))
{
    s_move_as_dpad = active;
}

static void keyboard_state(XBOX_INPUT_STATE *pState)
{
    static DWORD packet;
    int axis[4] = { 0, 0, 0, 0 };
    SHORT mx = 0, my = 0;
    WORD b = 0;
    int i, move_dpad = s_move_as_dpad && s_move_as_dpad();

    binds_load();
    memset(pState, 0, sizeof(*pState));

    for (i = 0; i < NBINDS; i++) {
        const Binding *bd = &s_binds[i];
        if (!bind_held(bd))
            continue;
        switch (bd->kind) {
        case BIND_DIGITAL:
            b |= (WORD)bd->what;
            break;
        /* Analog on the console, so a key is 255 rather than a flag -- a
         * title that reads these as a pressure never sees a press if they
         * are 1. */
        case BIND_ANALOG:
            pState->Gamepad.bAnalogButtons[bd->what] = 255;
            break;
        case BIND_STICK:
            axis[bd->what / 2] += (bd->what & 1) ? 32767 : -32767;
            if (move_dpad && bd->what < AXIS_RX * 2)
                b |= s_move_dpad_bit[bd->what];
            break;
        }
    }
    pState->Gamepad.wButtons = b;

    mouse_stick(&mx, &my);
    pState->Gamepad.sThumbLX = stick_sum(axis[AXIS_LX]);
    pState->Gamepad.sThumbLY = stick_sum(axis[AXIS_LY]);
    pState->Gamepad.sThumbRX = stick_sum(axis[AXIS_RX] + mx);
    pState->Gamepad.sThumbRY = stick_sum(axis[AXIS_RY] + my);

    /* The title's input layer looks for button edges, so the packet number
     * has to move whenever the state does or a press is never noticed. */
    pState->dwPacketNumber = ++packet;
}

/* Merge the keyboard on top rather than only standing in for a missing
 * pad.
 *
 * The first version put the keyboard behind XInput's failure, on the
 * assumption that with nothing plugged in the call would fail. It does
 * not: under Wine XInputGetState returns ERROR_SUCCESS and a gamepad
 * with every button at rest, so the fallback was unreachable and
 * pressing a key did nothing at all.
 *
 * Merging is also the better rule. A real pad keeps working -- its
 * buttons are already in pState and the keyboard only adds to them --
 * and there is no special case left to get wrong. */
static void keyboard_merge(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (dwPort == 0 && keyboard_enabled()) {
        XBOX_INPUT_STATE kb;
        int i;
        keyboard_state(&kb);
        pState->Gamepad.wButtons |= kb.Gamepad.wButtons;
        for (i = 0; i < 8; i++)
            if (kb.Gamepad.bAnalogButtons[i] > pState->Gamepad.bAnalogButtons[i])
                pState->Gamepad.bAnalogButtons[i] = kb.Gamepad.bAnalogButtons[i];
        if (kb.Gamepad.sThumbLX) pState->Gamepad.sThumbLX = kb.Gamepad.sThumbLX;
        if (kb.Gamepad.sThumbLY) pState->Gamepad.sThumbLY = kb.Gamepad.sThumbLY;
        if (kb.Gamepad.sThumbRX) pState->Gamepad.sThumbRX = kb.Gamepad.sThumbRX;
        if (kb.Gamepad.sThumbRY) pState->Gamepad.sThumbRY = kb.Gamepad.sThumbRY;
        /* The input layer records edges, so an unchanged packet number is
         * read as the same state and the press never happens. */
        pState->dwPacketNumber = kb.dwPacketNumber;
    }
}


/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };

void xbox_InputInit(void)
{
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        DWORD result = XInputGetState(i, &state);
        g_controller_connected[i] = (result == ERROR_SUCCESS);
    }
}

static DWORD backend_get_state(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XINPUT_STATE xi_state;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;
    /* input = keyboard: a pad on port 0 is not asked at all. */
    if (dwPort == 0 && input_mode() == PLAY_KEYBOARD && keyboard_enabled()) {
        keyboard_state(pState);
        return ERROR_SUCCESS;
    }

    result = XInputGetState(dwPort, &xi_state);
    if (result != ERROR_SUCCESS) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0 && keyboard_enabled()) {
            keyboard_state(pState);
            return ERROR_SUCCESS;
        }
        return result;
    }

    g_controller_connected[dwPort] = TRUE;
    g_last_packet[dwPort] = xi_state.dwPacketNumber;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = xi_state.dwPacketNumber;
    pState->Gamepad.wButtons = xi_state.Gamepad.wButtons & 0x00FF;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = xi_state.Gamepad.bLeftTrigger;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = xi_state.Gamepad.bRightTrigger;

    pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
    pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
    pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
    pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;

    /* Merge the keyboard on top rather than only standing in for a missing
     * pad: see keyboard_merge. */
    keyboard_merge(dwPort, pState);

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    return XInputSetState(dwPort, &xi_vib);
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetCapabilities(dwPort, dwFlags, &xi_caps);
    if (result != ERROR_SUCCESS) return result;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>
#include <pthread.h>

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

/* SDL may be driven from one thread at a time. The window's thread pumps its
 * events, so once it calls xbox_InputPump all the controller work -- opening,
 * reading, rumble -- happens there too, and the title's USB thread reads the
 * pads as that thread last saw them. Before that (and with no window) the
 * USB thread asks SDL itself. Updating the pads from the USB thread while the
 * window's thread waited on its events crashed one start in five on Wayland
 * ("malloc(): unaligned tcache chunk detected"): SDL wakes the waiting thread
 * with a Wayland call made from the thread that saw the input. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_pumped;
static struct { int attached; XBOX_GAMEPAD pad; } g_snap[XBOX_MAX_CONTROLLERS];
static struct { int pending; WORD left, right; } g_rumble[XBOX_MAX_CONTROLLERS];

/* Each attached game controller not open yet goes into the first free slot.
 * Matched by instance id, not list position, so a pad that stays connected
 * keeps its slot while others come and go. */
static void open_controllers(void)
{
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_JoystickID id;
        int slot, free_slot = -1, have = 0;
        if (!SDL_IsGameController(i))
            continue;
        id = SDL_JoystickGetDeviceInstanceID(i);
        for (slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++) {
            if (!g_pads[slot]) {
                if (free_slot < 0)
                    free_slot = slot;
            } else if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pads[slot])) == id) {
                have = 1;
            }
        }
        if (have || free_slot < 0)
            continue;
        g_pads[free_slot] = SDL_GameControllerOpen(i);
        g_controller_connected[free_slot] = g_pads[free_slot] != NULL;
        if (g_pads[free_slot])
            fprintf(stderr, "  [INPUT] pad %d: %s\n", free_slot + 1,
                    SDL_GameControllerName(g_pads[free_slot]));
        else
            fprintf(stderr, "  [INPUT] could not open %s: %s\n",
                    SDL_JoystickNameForIndex(i), SDL_GetError());
    }
}

void xbox_InputInit(void)
{
    /* The pad keeps working while another window has the focus, as XInput
     * does on Windows. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER)
            && SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        fprintf(stderr, "  [INPUT] SDL game controllers unavailable: %s\n", SDL_GetError());
    open_controllers();
    if (!g_pads[0])
        fprintf(stderr, "  [INPUT] no game controller yet (SDL sees %d joysticks); "
                "one connected later is picked up\n", SDL_NumJoysticks());
}

/* Nothing calls xbox_InputInit -- XInput needs no setup, so the Windows
 * build never missed it -- and without it SDL's controller subsystem never
 * started: natively only the keyboard and mouse worked. So it runs on first
 * use, and once a second after that pads that went away are closed and new
 * ones opened, as XInput finds a pad plugged in mid-game. */
static void controllers_poll(void)
{
    static int started;
    static Uint32 last;
    Uint32 now;

    if (!started) {
        started = 1;
        xbox_InputInit();
        last = SDL_GetTicks();
        return;
    }
    now = SDL_GetTicks();
    if (now - last < 1000)
        return;
    last = now;
    for (int slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++) {
        if (g_pads[slot] && !SDL_GameControllerGetAttached(g_pads[slot])) {
            fprintf(stderr, "  [INPUT] pad %d disconnected\n", slot + 1);
            SDL_GameControllerClose(g_pads[slot]);
            g_pads[slot] = NULL;
            g_controller_connected[slot] = FALSE;
        }
    }
    open_controllers();
}

static void read_pad(SDL_GameController *c, XBOX_GAMEPAD *g)
{
    WORD btn = 0;

    memset(g, 0, sizeof *g);
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    g->wButtons = btn;

    g->bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;
    g->bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    g->bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    g->bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    g->sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    g->sThumbLY = (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    g->sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    g->sThumbRY = (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));
}

/* On the window's thread, after it pumps SDL's events (which updates the
 * pads): opens and closes pads, reads them, passes rumble on. */
void xbox_InputPump(void)
{
    pthread_mutex_lock(&g_lock);
    controllers_poll();
    for (int slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++) {
        SDL_GameController *c = g_pads[slot];
        g_snap[slot].attached = c && SDL_GameControllerGetAttached(c);
        if (g_snap[slot].attached)
            read_pad(c, &g_snap[slot].pad);
        if (g_rumble[slot].pending && g_snap[slot].attached)
            SDL_GameControllerRumble(c, g_rumble[slot].left, g_rumble[slot].right, 1000);
        g_rumble[slot].pending = 0;
    }
    __atomic_store_n(&g_pumped, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_lock);
}

static DWORD backend_get_state(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XBOX_GAMEPAD pad;
    int attached;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;
    static int window = -1;
    if (window < 0) {
        const char *e = getenv("RECOMP_FB_WINDOW");
        window = e && *e == '1';
    }
    pthread_mutex_lock(&g_lock);
    if (__atomic_load_n(&g_pumped, __ATOMIC_ACQUIRE)) {
        attached = g_snap[dwPort].attached;
        pad = g_snap[dwPort].pad;
    } else if (window) {
        attached = 0;                   /* until the window's thread has the pads */
    } else {
        SDL_GameController *c;
        controllers_poll();
        c = g_pads[dwPort];
        attached = c && SDL_GameControllerGetAttached(c);
        if (attached) {
            SDL_GameControllerUpdate();
            read_pad(c, &pad);
        }
    }
    pthread_mutex_unlock(&g_lock);

    /* input = keyboard: a pad on port 0 is not asked at all. */
    if (dwPort == 0 && input_mode() == PLAY_KEYBOARD && keyboard_enabled()) {
        keyboard_state(pState);
        return ERROR_SUCCESS;
    }
    if (!attached) {
        g_controller_connected[dwPort] = FALSE;
        if (dwPort == 0 && keyboard_enabled()) {
            keyboard_state(pState);
            return ERROR_SUCCESS;
        }
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    g_controller_connected[dwPort] = TRUE;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];
    pState->Gamepad = pad;
    keyboard_merge(dwPort, pState);
    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c) return ERROR_DEVICE_NOT_CONNECTED;

    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). */
    pthread_mutex_lock(&g_lock);
    if (__atomic_load_n(&g_pumped, __ATOMIC_ACQUIRE)) {
        g_rumble[dwPort].pending = 1;
        g_rumble[dwPort].left = pVibration->wLeftMotorSpeed;
        g_rumble[dwPort].right = pVibration->wRightMotorSpeed;
    } else {
        SDL_GameControllerRumble(c, pVibration->wLeftMotorSpeed,
                                 pVibration->wRightMotorSpeed, 1000);
    }
    pthread_mutex_unlock(&g_lock);
    return ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_pads[dwPort])
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */

/* RECOMP_CAMERA_FLIP_X=1 and RECOMP_CAMERA_FLIP_Y=1: the right stick's left
 * and right, or up and down, swapped for a pad, the mouse and the look keys
 * alike. A title whose camera swings round the player the way the stick
 * pushes turns the view the other way: push right and the scene turns left,
 * push up and it tilts down. Alter Echo's does both; its Pitch Axis option
 * flips up and down again on top of this. A PC player expects the view to go
 * the way the stick or the mouse goes; the game's launcher turns both on,
 * and 0 keeps the console's way. */
static int env_on(const char *name)
{
    const char *e = getenv(name);
    return e && *e == '1';
}

static SHORT stick_flip(SHORT v)
{
    return v == -32768 ? 32767 : (SHORT)-v;
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    static int flip_x = -1, flip_y;
    DWORD result = backend_get_state(dwPort, pState);
    if (flip_x < 0) {
        flip_y = env_on("RECOMP_CAMERA_FLIP_Y");
        flip_x = env_on("RECOMP_CAMERA_FLIP_X");
    }
    if (result == ERROR_SUCCESS) {
        if (flip_x)
            pState->Gamepad.sThumbRX = stick_flip(pState->Gamepad.sThumbRX);
        if (flip_y)
            pState->Gamepad.sThumbRY = stick_flip(pState->Gamepad.sThumbRY);
    }
    return result;
}
