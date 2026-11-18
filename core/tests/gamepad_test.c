/*
 * gamepad_test: the SDL gamepad thread end to end, with SDL's virtual
 * gamepads standing in for real ones (no hardware needed):
 *
 *   - a pad plugged in is announced and takes joystick 1; the D-pad, the
 *     left stick (and only while "analog_stick" is on), South and a trigger
 *     reach it, merged;
 *   - a second pad takes joystick 2; putting the first on joystick 2 by hand
 *     moves the second to joystick 1 and is remembered ("pad_assign");
 *   - a pad unplugged lets go of what it held;
 *   - with the PMD 32 on GPIO port A, joystick 1 is gone and the automatic
 *     pad takes joystick 2.
 *
 * Skips (77) when SDL cannot start its gamepad subsystem.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL.h>

#include "pmd85session.h"
#include "test_util.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

enum { U = PMD85SESSION_JOY_UP, D = PMD85SESSION_JOY_DOWN, L = PMD85SESSION_JOY_LEFT,
       R = PMD85SESSION_JOY_RIGHT, F = PMD85SESSION_JOY_FIRE };

static void sleep_ms(int ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

/* wait (up to 2 s) for joystick port to show want */
static int held_is(pmd85session *s, int port, unsigned want)
{
    for (int i = 0; i < 100; i++) {
        if (pmd85session_joystick_held(s, port) == want) return 1;
        sleep_ms(20);
    }
    return 0;
}

static int count_is(pmd85session *s, int n)
{
    for (int i = 0; i < 100; i++) {
        if (pmd85session_gamepad_count(s) == n) return 1;
        sleep_ms(20);
    }
    return 0;
}

static SDL_Joystick *plug(const char *name)
{
    SDL_VirtualJoystickDesc desc;
    SDL_JoystickID id;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = name;
    id = SDL_AttachVirtualJoystick(&desc);
    return id ? SDL_OpenJoystick(id) : NULL;
}

static void press(SDL_Joystick *j, int button, int down)
{
    SDL_SetJoystickVirtualButton(j, button, down != 0);
}

int main(void)
{
    char dir[800], cfg[900], data[900], ev[160];
    pmd85session_paths paths;
    pmd85session_start_opts opts;
    pmd85session *s;
    SDL_Joystick *a, *b;
    unsigned serial;

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "no SDL gamepads: %s\n", SDL_GetError());
        return 77;
    }
    if (test_mkdtemp(dir, sizeof dir, "pmd85-pad") != 0) return 77;
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";
    s = pmd85session_new(&paths);
    if (!s) return 1;
    pmd85session_default_opts(s, &opts);
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 1;
    CHECK(pmd85session_start(s, &opts) == 0, "start: %s", pmd85session_last_error(s));
    CHECK(pmd85session_joysticks(s) == 3, "joysticks %u", pmd85session_joysticks(s));

    /* one pad: joystick 1 */
    serial = pmd85session_gamepad_last_event(s, NULL, 0);
    a = plug("Pad A");
    CHECK(a != NULL, "attach: %s", SDL_GetError());
    CHECK(count_is(s, 1), "pad A not seen (%d pads)", pmd85session_gamepad_count(s));
    CHECK(pmd85session_gamepad_last_event(s, ev, sizeof ev) != serial &&
          !strcmp(ev, "Connected: Pad A (joystick 1)"), "event '%s'", ev);
    CHECK(pmd85session_gamepad_effective_port(s, 0) == 0, "pad A on %d",
          pmd85session_gamepad_effective_port(s, 0));

    press(a, SDL_GAMEPAD_BUTTON_DPAD_UP, 1);
    CHECK(held_is(s, 0, U), "D-pad up: %x", pmd85session_joystick_held(s, 0));
    press(a, SDL_GAMEPAD_BUTTON_SOUTH, 1);
    CHECK(held_is(s, 0, U | F), "and South: %x", pmd85session_joystick_held(s, 0));
    SDL_SetJoystickVirtualAxis(a, SDL_GAMEPAD_AXIS_LEFTX, 32767);
    CHECK(held_is(s, 0, U | R | F), "and the stick right: %x", pmd85session_joystick_held(s, 0));
    SDL_SetJoystickVirtualAxis(a, SDL_GAMEPAD_AXIS_LEFTY, 32767);   /* down: cancels the D-pad's up */
    CHECK(held_is(s, 0, R | F), "stick down against D-pad up: %x", pmd85session_joystick_held(s, 0));
    press(a, SDL_GAMEPAD_BUTTON_SOUTH, 0);
    SDL_SetJoystickVirtualAxis(a, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    CHECK(held_is(s, 0, R | F), "the trigger fires: %x", pmd85session_joystick_held(s, 0));
    SDL_SetJoystickVirtualAxis(a, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    press(a, SDL_GAMEPAD_BUTTON_DPAD_UP, 0);
    CHECK(held_is(s, 0, R | D), "the stick alone: %x", pmd85session_joystick_held(s, 0));
    pmd85session_set_int(s, "analog_stick", 0);
    CHECK(held_is(s, 0, 0), "stick off: %x", pmd85session_joystick_held(s, 0));
    pmd85session_set_int(s, "analog_stick", 1);
    SDL_SetJoystickVirtualAxis(a, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(a, SDL_GAMEPAD_AXIS_LEFTY, 0);
    CHECK(held_is(s, 0, 0), "let go: %x", pmd85session_joystick_held(s, 0));

    /* the frontend's own source merges with the pad's */
    pmd85session_set_joystick(s, 0, L);
    press(a, SDL_GAMEPAD_BUTTON_EAST, 1);
    CHECK(held_is(s, 0, L | F), "frontend left + East: %x", pmd85session_joystick_held(s, 0));
    press(a, SDL_GAMEPAD_BUTTON_EAST, 0);
    CHECK(held_is(s, 0, L), "East let go, the frontend's left stays: %x",
          pmd85session_joystick_held(s, 0));
    pmd85session_set_joystick(s, 0, 0);

    /* a second pad: joystick 2 */
    b = plug("Pad B");
    CHECK(count_is(s, 2), "pad B not seen");
    CHECK(pmd85session_gamepad_effective_port(s, 1) == 1, "pad B on %d",
          pmd85session_gamepad_effective_port(s, 1));
    press(b, SDL_GAMEPAD_BUTTON_DPAD_LEFT, 1);
    CHECK(held_is(s, 1, L) && pmd85session_joystick_held(s, 0) == 0, "pad B steers joystick 2");
    press(b, SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0);

    /* pad A put on joystick 2 by hand: B (automatic) moves to joystick 1 */
    pmd85session_gamepad_assign(s, 0, 1);
    CHECK(pmd85session_gamepad_assignment(s, 0) == 1, "A's assignment");
    CHECK(pmd85session_gamepad_effective_port(s, 0) == 1 &&
          pmd85session_gamepad_effective_port(s, 1) == 0, "after assigning: A %d, B %d",
          pmd85session_gamepad_effective_port(s, 0), pmd85session_gamepad_effective_port(s, 1));
    CHECK(strstr(pmd85session_get_str(s, "pad_assign", ""), "=1") != NULL,
          "not remembered: '%s'", pmd85session_get_str(s, "pad_assign", ""));
    press(a, SDL_GAMEPAD_BUTTON_SOUTH, 1);
    CHECK(held_is(s, 1, F) && pmd85session_joystick_held(s, 0) == 0, "A fires joystick 2");

    /* A unplugged while it fires: joystick 2 lets go */
    serial = pmd85session_gamepad_last_event(s, NULL, 0);
    SDL_CloseJoystick(a);
    SDL_DetachVirtualJoystick(SDL_GetJoystickID(a));
    CHECK(count_is(s, 1), "pad A still there");
    CHECK(held_is(s, 1, 0), "A's fire outlived it: %x", pmd85session_joystick_held(s, 1));
    CHECK(pmd85session_gamepad_last_event(s, ev, sizeof ev) != serial &&
          !strcmp(ev, "Disconnected: Pad A"), "event '%s'", ev);

    /* the PMD 32 takes port A: B (automatic) goes to joystick 2 */
    pmd85session_set_peripheral(s, "pmd32", 1);
    CHECK(pmd85session_joysticks(s) == 2, "with the PMD 32: %u", pmd85session_joysticks(s));
    for (int i = 0; i < 50 && pmd85session_gamepad_effective_port(s, 0) != 1; i++) sleep_ms(20);
    CHECK(pmd85session_gamepad_effective_port(s, 0) == 1, "B on %d with the PMD 32",
          pmd85session_gamepad_effective_port(s, 0));
    press(b, SDL_GAMEPAD_BUTTON_DPAD_DOWN, 1);
    CHECK(held_is(s, 1, D), "B steers joystick 2: %x", pmd85session_joystick_held(s, 1));

    SDL_CloseJoystick(b);
    pmd85session_free(s);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    test_rmtree(dir);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("gamepad: ok\n");
    return 0;
}
