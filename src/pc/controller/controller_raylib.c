#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <math.h>

#include "raylib.h"
#include "controller_api.h"

// N64 button masks (guarded in case the engine headers already define them).
#ifndef A_BUTTON
#define A_BUTTON       0x8000
#define B_BUTTON       0x4000
#define Z_TRIG         0x2000
#define START_BUTTON   0x1000
#define U_JPAD         0x0800
#define D_JPAD         0x0400
#define L_JPAD         0x0200
#define R_JPAD         0x0100
#define L_TRIG         0x0020
#define R_TRIG         0x0010
#define U_CBUTTONS     0x0008
#define D_CBUTTONS     0x0004
#define L_CBUTTONS     0x0002
#define R_CBUTTONS     0x0001
#endif

// Set to 0 once input works. Writes controller_debug.txt next to the .exe so
// it works even if the build has no console (-mwindows).
#define CONTROLLER_DEBUG 1

#if CONTROLLER_DEBUG
#include <stdio.h>
static FILE *dbg_file(void) {
    static FILE *f = NULL;
    if (!f) f = fopen("controller_debug.txt", "w");
    return f;
}
#define DBG(...) do { FILE *f_ = dbg_file(); if (f_) { fprintf(f_, __VA_ARGS__); fflush(f_); } } while (0)
#else
#define DBG(...) do {} while (0)
#endif

#define PAD 0                  // gamepad index
#define STICK_MAX 127.0f
#define STICK_DEADZONE 0.15f   // radial, in raylib's -1..1 range
#define TRIGGER_THRESHOLD 0.5f

static void controller_raylib_init(void) {
    // raylib sets up input in InitWindow(); nothing to do.
    DBG("init called, window ready: %d\n", IsWindowReady());
}

static int8_t clamp_stick(int v) {
    if (v > 127) v = 127;
    if (v < -127) v = -127;
    return (int8_t)v;
}

static void controller_raylib_read(OSContPad *pad) {
    if (!pad) return;

    int kx = 0, ky = 0;

    // ---------------- Keyboard ----------------
    if (IsKeyDown(KEY_ENTER)) pad->button |= START_BUTTON;
    if (IsKeyDown(KEY_SPACE)) pad->button |= A_BUTTON;
    if (IsKeyDown(KEY_X))     pad->button |= B_BUTTON;
    if (IsKeyDown(KEY_Z))     pad->button |= Z_TRIG;
    if (IsKeyDown(KEY_Q))     pad->button |= L_TRIG;
    if (IsKeyDown(KEY_E))     pad->button |= R_TRIG;

    // Arrow keys -> C buttons
    if (IsKeyDown(KEY_UP))    pad->button |= U_CBUTTONS;
    if (IsKeyDown(KEY_DOWN))  pad->button |= D_CBUTTONS;
    if (IsKeyDown(KEY_LEFT))  pad->button |= L_CBUTTONS;
    if (IsKeyDown(KEY_RIGHT)) pad->button |= R_CBUTTONS;

    // WASD -> stick (opposite keys cancel instead of one winning)
    if (IsKeyDown(KEY_W)) ky += 127;
    if (IsKeyDown(KEY_S)) ky -= 127;
    if (IsKeyDown(KEY_A)) kx -= 127;
    if (IsKeyDown(KEY_D)) kx += 127;

    int sx = kx;
    int sy = ky;

    // ---------------- Gamepad ----------------
    if (IsGamepadAvailable(PAD)) {
        // Face buttons (Xbox naming; PlayStation in brackets)
        if (IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_MIDDLE_RIGHT))     pad->button |= START_BUTTON;  // Start [Options]
        if (IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))  pad->button |= A_BUTTON;      // A [Cross]
        if (IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_RIGHT_FACE_LEFT))  pad->button |= B_BUTTON;      // X [Square]
        if (IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT)) pad->button |= B_BUTTON;      // B [Circle]

        // Shoulders / triggers
        if (IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_LEFT_TRIGGER_1))   pad->button |= L_TRIG;        // LB
        if (IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))  pad->button |= R_TRIG;        // RB
        if (IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_LEFT_TRIGGER_2) ||
            GetGamepadAxisMovement(PAD, GAMEPAD_AXIS_LEFT_TRIGGER) > TRIGGER_THRESHOLD) {
            pad->button |= Z_TRIG;                                                                   // LT
        }
        if (IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_RIGHT_TRIGGER_2) ||
            GetGamepadAxisMovement(PAD, GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER_THRESHOLD) {
            pad->button |= R_TRIG;                                                                   // RT
        }

        // D-pad (the LEFT_FACE_* buttons are the D-pad in raylib)
        if (IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_LEFT_FACE_UP))     pad->button |= U_JPAD;
        if (IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_LEFT_FACE_DOWN))   pad->button |= D_JPAD;
        if (IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_LEFT_FACE_LEFT))   pad->button |= L_JPAD;
        if (IsGamepadButtonDown(PAD, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))  pad->button |= R_JPAD;

        // Right stick -> C buttons
        float c_x = GetGamepadAxisMovement(PAD, GAMEPAD_AXIS_RIGHT_X);
        float c_y = GetGamepadAxisMovement(PAD, GAMEPAD_AXIS_RIGHT_Y);
        if (c_y < -0.5f) pad->button |= U_CBUTTONS;
        if (c_y >  0.5f) pad->button |= D_CBUTTONS;
        if (c_x < -0.5f) pad->button |= L_CBUTTONS;
        if (c_x >  0.5f) pad->button |= R_CBUTTONS;

        // Left stick -> analog stick, radial deadzone with rescaling
        float ax = GetGamepadAxisMovement(PAD, GAMEPAD_AXIS_LEFT_X);
        float ay = -GetGamepadAxisMovement(PAD, GAMEPAD_AXIS_LEFT_Y); // raylib: down is +, N64: up is +
        float mag = sqrtf(ax * ax + ay * ay);
        if (mag > STICK_DEADZONE) {
            float scaled = (mag - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
            if (scaled > 1.0f) scaled = 1.0f;
            float k = scaled / mag;
            int gx = (int)(ax * k * STICK_MAX);
            int gy = (int)(ay * k * STICK_MAX);
            // Analog wins over keyboard when it's actually being pushed
            sx = gx;
            sy = gy;
        }
    }

    pad->stick_x = clamp_stick(sx);
    pad->stick_y = clamp_stick(sy);

#if CONTROLLER_DEBUG
    static unsigned calls = 0;
    static int last_key = 0;
    static unsigned last_buttons = 0;
    calls++;
    if (calls == 1) DBG("first read call\n");
    if (calls % 150 == 0) DBG("read calls so far: %u, focused: %d\n", calls, IsWindowFocused());
    int key = GetKeyPressed();   // any key raylib saw this frame
    if (key && key != last_key) DBG("raylib saw key code %d\n", key);
    if (key) last_key = key;
    if ((unsigned)pad->button != last_buttons) {
        DBG("buttons now 0x%04x, stick %d,%d\n", (unsigned)pad->button, pad->stick_x, pad->stick_y);
        last_buttons = pad->button;
    }
#endif
}

struct ControllerAPI controller_xinput = {
    controller_raylib_init,
    controller_raylib_read
};