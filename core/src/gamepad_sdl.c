/*
 * gamepad_sdl -- SDL3 gamepads (and unmapped joysticks) as the PMD 85's
 * 4004/482 joysticks, on their own polling thread. After the Atari 800
 * app's gamepad_sdl.c (its v0.7.0 design), cut down to what a PMD joystick
 * is: four directions and one fire button.
 *
 * A polling thread rather than SDL's event queue on the UI thread: this
 * app's video belongs to GTK (and the other toolkits), not SDL, and only the
 * gamepad subsystem is initialised here -- never SDL video. Devices come and
 * go as they appear: SDL posts JOYSTICK_ and GAMEPAD_ add/remove pairs and
 * both are taken, with idempotent open/close. A device SDL has no gamepad
 * mapping for (a plain HID joystick) is opened as a raw joystick.
 *
 * What a pad drives:
 *   - directions: the D-pad, and (setting "analog_stick", on) the left
 *     stick as an eight-way switch with hysteresis; a raw joystick's hat 0
 *     and axes 0/1;
 *   - fire: South, East, the shoulders and the triggers; a raw joystick's
 *     buttons 0-3.
 * Its buttons and its stick are separate sources (joy_input.h), so letting
 * go of one never cancels the other, the frontend's or another pad's hold.
 *
 * Pads take the joysticks in connection order unless put on one explicitly;
 * an explicit choice is remembered (setting "pad_assign", pad_assign.h) and
 * comes back with the pad, by its SDL GUID, else its name. Every connect and
 * disconnect is reported for the frontend's toast.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "joy_input.h"
#include "pad_assign.h"
#include "session_internal.h"

#define MAX_PADS PMD85_MAX_PADS

/* A third of full deflection engages, a sixth lets go: low enough that a
 * worn stick still registers, high enough that resting drift does not, and
 * a gap wide enough that jitter across one threshold cannot flutter. */
#define STICK_ENTER 0.35f
#define STICK_EXIT  0.15f
#define TRIGGER_ON  0.5f

typedef struct {
    SDL_JoystickID id;
    int owner;                /* joy_input source number: stable while connected */
    SDL_Gamepad *handle;      /* mapped gamepad, or NULL */
    SDL_Joystick *joy;        /* raw joystick when not a gamepad */
    int assign;               /* explicit joystick, -1 = automatic */
    int by_name;              /* assign came from a name-only match */
    int port;                 /* the joystick it drove on the last poll, or -1 */
    unsigned stick_dir;       /* the left stick's state (hysteresis) */
    unsigned sent_stick;      /* what its stick source holds */
    unsigned fire_mask;       /* buttons holding fire, one bit each */
    int trigger_down[2];
    char guidstr[PMD85_PAD_GUID_MAX];
    char name[PMD85_PAD_NAME_MAX];
} pad_slot;

typedef struct {
    pmd85session *session;
    pthread_t thread;
    pthread_mutex_t lock;
    volatile int stop_requested;
    pad_slot pads[MAX_PADS];
    int npads;
    unsigned generation;
    int quiet_scan;           /* a restarted session's first scan re-finds pads
                                 without announcing them */
    pmd85_pad_table table;    /* the remembered assignments; guarded by lock */
} gamepad_state;

/* ---- slots ------------------------------------------------------------------ */

static int slot_for_id(gamepad_state *g, SDL_JoystickID id)
{
    for (int i = 0; i < g->npads; i++)
        if (g->pads[i].id == id) return i;
    return -1;
}

static const char *slot_name(const pad_slot *slot)
{
    const char *n = slot->handle ? SDL_GetGamepadName(slot->handle)
                                 : SDL_GetJoystickName(slot->joy);
    return n ? n : "Gamepad";
}

static void slot_release(gamepad_state *g, pad_slot *slot)
{
    slot->sent_stick = 0;
    slot->stick_dir = 0;
    slot->fire_mask = 0;
    slot->trigger_down[0] = slot->trigger_down[1] = 0;
    joy_input_clear_pad(g->session, slot->owner);
}

/* The joystick a slot drives now. Locked by the caller. */
static int effective_port_locked(gamepad_state *g, int idx)
{
    int assign[MAX_PADS], out[MAX_PADS];
    for (int i = 0; i < g->npads; i++) assign[i] = g->pads[i].assign;
    pmd85_pad_ports(assign, g->npads, session_joysticks(g->session), out);
    return out[idx];
}

/* Gives the pad in slot idx its remembered joystick. An entry with the pad's
 * own GUID is the pad's: it takes it from a pad that got it only through the
 * name fallback. A pad known only by name takes the entry if no other pad
 * carries it. Locked by the caller. */
static void restore_assignment(gamepad_state *g, int idx)
{
    pad_slot *slot = &g->pads[idx];
    int ent = pmd85_pad_table_find(&g->table, slot->guidstr, slot->name), exact;
    if (ent < 0 || g->table.entry[ent].port >= PMD85_JOY_PORTS) return;
    exact = !strcmp(g->table.entry[ent].guid, slot->guidstr);
    for (int k = 0; k < g->npads; k++) {
        pad_slot *other = &g->pads[k];
        if (k == idx || other->assign < 0 ||
            pmd85_pad_table_find(&g->table, other->guidstr, other->name) != ent)
            continue;
        if (!exact || !other->by_name) return;
        other->assign = -1;
        other->by_name = 0;
    }
    slot->assign = g->table.entry[ent].port;
    slot->by_name = !exact;
}

static void open_pad(gamepad_state *g, SDL_JoystickID id)
{
    char msg[160];
    int announce = 0;

    pthread_mutex_lock(&g->lock);
    if (g->npads < MAX_PADS && slot_for_id(g, id) < 0) {
        /* a device SDL calls a gamepad can still fail to open as one: then
         * it is a raw joystick */
        SDL_Gamepad *gp = SDL_IsGamepad(id) ? SDL_OpenGamepad(id) : NULL;
        SDL_Joystick *j = gp ? NULL : SDL_OpenJoystick(id);
        if (gp || j) {
            pad_slot *slot = &g->pads[g->npads++];
            unsigned used = 0;
            int k, port;
            for (k = 0; k < g->npads - 1; k++) used |= 1u << g->pads[k].owner;
            memset(slot, 0, sizeof *slot);
            for (k = 0; k < MAX_PADS && (used & (1u << k)); k++) {}
            slot->owner = k;
            slot->id = id;
            slot->handle = gp;
            slot->joy = j;
            slot->assign = -1;
            slot->port = -1;
            snprintf(slot->name, sizeof slot->name, "%s", slot_name(slot));
            SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), slot->guidstr, sizeof slot->guidstr);
            restore_assignment(g, g->npads - 1);
            fprintf(stderr, "pmd85 gamepad: %s connected (%s)\n", slot->name,
                    gp ? "gamepad" : "raw joystick");
            if (!g->quiet_scan) {
                port = effective_port_locked(g, g->npads - 1);
                if (port >= 0)
                    snprintf(msg, sizeof msg, "Connected: %s (joystick %d)", slot->name, port + 1);
                else
                    snprintf(msg, sizeof msg, "Connected: %s (no joystick free)", slot->name);
                announce = 1;
            }
            g->generation++;
        }
    }
    pthread_mutex_unlock(&g->lock);
    if (announce) session_gamepad_event(g->session, msg);
}

static void close_pad(gamepad_state *g, SDL_JoystickID id)
{
    char msg[160];
    int idx, announce = 0;
    pthread_mutex_lock(&g->lock);
    idx = slot_for_id(g, id);
    if (idx >= 0) {
        pad_slot *slot = &g->pads[idx];
        fprintf(stderr, "pmd85 gamepad: %s disconnected\n", slot->name);
        snprintf(msg, sizeof msg, "Disconnected: %s", slot->name);
        /* its explicit assignment stays in the table: it comes back with it */
        slot_release(g, slot);
        if (slot->handle) SDL_CloseGamepad(slot->handle);
        if (slot->joy) SDL_CloseJoystick(slot->joy);
        for (int i = idx; i < g->npads - 1; i++) g->pads[i] = g->pads[i + 1];
        g->npads--;
        g->generation++;
        announce = 1;
    }
    pthread_mutex_unlock(&g->lock);
    if (announce) session_gamepad_event(g->session, msg);
}

/* The slot's joystick as of now; when it changed (assigned elsewhere, the
 * PMD 32 took port A), what it held belongs to the old one and is let go.
 * Locked by the caller. */
static void slot_sync_port(gamepad_state *g, pad_slot *slot, int port)
{
    if (port == slot->port) return;
    if (slot->port >= 0) slot_release(g, slot);
    slot->port = port;
}

/* ---- buttons: fire ------------------------------------------------------------ */

/* Which bit of fire_mask a button is, -1 when it does not fire. Gamepad
 * buttons are 0..SDL_GAMEPAD_BUTTON_COUNT-1, the triggers 30/31, a raw
 * joystick's buttons 0..3 the same as SOUTH..NORTH. */
#define FIRE_TRIGGER_L 30
#define FIRE_TRIGGER_R 31

static int fire_bit(const pad_slot *slot, int button)
{
    if (!slot->handle)
        return (button >= 0 && button < 4) ? button : -1;
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
    case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
    case FIRE_TRIGGER_L:
    case FIRE_TRIGGER_R:
        return button;
    default:
        return -1;
    }
}

static void button(gamepad_state *g, SDL_JoystickID which, int b, int down)
{
    int idx, port = -1, owner = 0, bit;
    unsigned before = 0, after = 0;
    pthread_mutex_lock(&g->lock);
    idx = slot_for_id(g, which);
    if (idx >= 0) {
        pad_slot *slot = &g->pads[idx];
        port = effective_port_locked(g, idx);
        slot_sync_port(g, slot, port);
        bit = fire_bit(slot, b);
        if (port >= 0 && bit >= 0) {
            owner = slot->owner;
            before = slot->fire_mask;
            if (down) slot->fire_mask |= 1u << bit;
            else slot->fire_mask &= ~(1u << bit);
            after = slot->fire_mask;
        }
    }
    pthread_mutex_unlock(&g->lock);
    if (port >= 0 && (before != 0) != (after != 0))
        joy_input_set(g->session, PMD85_SRC_PAD(owner), port, after ? PMD85SESSION_JOY_FIRE : 0);
}

/* ---- directions --------------------------------------------------------------- */

static float axis_norm(Sint16 v)
{
    return v < 0 ? (float)v / 32768.0f : (float)v / 32767.0f;
}

/* A trigger crossing its threshold, applied by poll_sticks once it has let go
 * of the lock (button() takes it too; it is not recursive). */
typedef struct {
    SDL_JoystickID id;
    int button;
    int down;
} trigger_edge;

#define MAX_EDGES (MAX_PADS * 2)

typedef struct {
    int owner, port;
    unsigned bits;
} stick_change;

static void poll_one(gamepad_state *g, pad_slot *slot, int port, int analog,
                     trigger_edge *edges, int *nedges, stick_change *ch, int *nch)
{
    unsigned dir = 0;
    float x = 0.f, y = 0.f;

    slot_sync_port(g, slot, port);
    if (port < 0) return;   /* more pads than joysticks: this one plays nothing */

    if (slot->handle) {
        if (SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_UP)) dir |= PMD85SESSION_JOY_UP;
        if (SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) dir |= PMD85SESSION_JOY_DOWN;
        if (SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) dir |= PMD85SESSION_JOY_LEFT;
        if (SDL_GetGamepadButton(slot->handle, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) dir |= PMD85SESSION_JOY_RIGHT;
        x = axis_norm(SDL_GetGamepadAxis(slot->handle, SDL_GAMEPAD_AXIS_LEFTX));
        y = axis_norm(SDL_GetGamepadAxis(slot->handle, SDL_GAMEPAD_AXIS_LEFTY));
    } else {
        if (SDL_GetNumJoystickHats(slot->joy) > 0) {
            Uint8 m = SDL_GetJoystickHat(slot->joy, 0);
            if (m & SDL_HAT_UP) dir |= PMD85SESSION_JOY_UP;
            if (m & SDL_HAT_DOWN) dir |= PMD85SESSION_JOY_DOWN;
            if (m & SDL_HAT_LEFT) dir |= PMD85SESSION_JOY_LEFT;
            if (m & SDL_HAT_RIGHT) dir |= PMD85SESSION_JOY_RIGHT;
        }
        /* a plain joystick's axes are its stick: always steer */
        if (SDL_GetNumJoystickAxes(slot->joy) > 1) {
            x = axis_norm(SDL_GetJoystickAxis(slot->joy, 0));
            y = axis_norm(SDL_GetJoystickAxis(slot->joy, 1));
        }
        analog = 1;
    }
    if (analog) {
        slot->stick_dir = pmd85_dir_from_stick(x, y, slot->stick_dir, STICK_ENTER, STICK_EXIT);
        dir |= slot->stick_dir;
    } else {
        slot->stick_dir = 0;
    }
    if (dir != slot->sent_stick && *nch < MAX_PADS) {
        slot->sent_stick = dir;
        ch[*nch].owner = slot->owner;
        ch[*nch].port = port;
        ch[*nch].bits = dir;
        (*nch)++;
    }

    if (slot->handle) {
        for (int t = 0; t < 2; t++) {
            float v = axis_norm(SDL_GetGamepadAxis(slot->handle,
                t ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
            int down = v >= TRIGGER_ON;
            if (down != slot->trigger_down[t] && *nedges < MAX_EDGES) {
                slot->trigger_down[t] = down;
                edges[*nedges].id = slot->id;
                edges[*nedges].button = t ? FIRE_TRIGGER_R : FIRE_TRIGGER_L;
                edges[*nedges].down = down;
                (*nedges)++;
            }
        }
    }
}

static void poll_sticks(gamepad_state *g)
{
    trigger_edge edges[MAX_EDGES];
    stick_change ch[MAX_PADS];
    int nedges = 0, nch = 0;
    const int analog = pmd85session_get_int(g->session, "analog_stick", 1) != 0;

    pthread_mutex_lock(&g->lock);
    for (int i = 0; i < g->npads; i++)
        poll_one(g, &g->pads[i], effective_port_locked(g, i), analog, edges, &nedges, ch, &nch);
    pthread_mutex_unlock(&g->lock);
    for (int i = 0; i < nch; i++)
        joy_input_set(g->session, PMD85_SRC_STICK(ch[i].owner), ch[i].port, ch[i].bits);
    for (int i = 0; i < nedges; i++)
        button(g, edges[i].id, edges[i].button, edges[i].down);
}

/* ---- the thread ------------------------------------------------------------- */

static void *thread_main(void *arg)
{
    gamepad_state *g = arg;
    int count = 0;
    /* every joystick, not just mapped gamepads: SDL_GetGamepads would leave
     * out a plain HID joystick */
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    if (ids) {
        for (int i = 0; i < count; i++) open_pad(g, ids[i]);
        SDL_free(ids);
    }
    g->quiet_scan = 0;

    while (!g->stop_requested) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_JOYSTICK_ADDED: open_pad(g, ev.jdevice.which); break;
            case SDL_EVENT_JOYSTICK_REMOVED: close_pad(g, ev.jdevice.which); break;
            case SDL_EVENT_GAMEPAD_ADDED: open_pad(g, ev.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: close_pad(g, ev.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                button(g, ev.gbutton.which, ev.gbutton.button,
                       ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP: {
                /* a mapped gamepad also sends these for the press it reported
                 * as a GAMEPAD_BUTTON event: only a raw joystick's count */
                int raw, idx;
                pthread_mutex_lock(&g->lock);
                idx = slot_for_id(g, ev.jbutton.which);
                raw = idx >= 0 && g->pads[idx].handle == NULL;
                pthread_mutex_unlock(&g->lock);
                if (raw)
                    button(g, ev.jbutton.which, ev.jbutton.button,
                           ev.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
                break;
            }
            default:
                break;
            }
        }
        poll_sticks(g);
        /* ~60 Hz with a pad (the PMD reads at 50); with none only hot-plug
         * events are waited for. Not SDL_WaitEventTimeout: SDL polls at
         * 1 kHz inside it. */
        SDL_Delay(g->npads > 0 ? 16 : 100);
    }

    pthread_mutex_lock(&g->lock);
    for (int i = 0; i < g->npads; i++) {
        slot_release(g, &g->pads[i]);
        if (g->pads[i].handle) SDL_CloseGamepad(g->pads[i].handle);
        if (g->pads[i].joy) SDL_CloseJoystick(g->pads[i].joy);
    }
    g->npads = 0;
    pthread_mutex_unlock(&g->lock);
    return NULL;
}

int gamepad_start(pmd85session *s)
{
    gamepad_state *g;
    pthread_attr_t attr;
    if (s->gamepad) return 0;
    /* the app owns its signals: SDL must not take SIGINT/SIGTERM */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* the poll thread is not the focused window: keep reading pads while
     * another window (the debugger) has the focus */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        session_set_error(s, "SDL gamepad init failed: %s", SDL_GetError());
        return -1;
    }
    g = calloc(1, sizeof *g);
    if (!g) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return -1;
    }
    g->session = s;
    pthread_mutex_init(&g->lock, NULL);
    /* the generation carries across a restart, so a frontend's baseline never
     * goes stale; a restarted thread's first scan re-finds the same pads,
     * which is no hot-plug: no toast */
    g->generation = s->pad_gen_base;
    g->quiet_scan = s->pad_restarted;
    pmd85_pad_table_decode(&g->table, pmd85session_get_str(s, "pad_assign", ""), PMD85_JOY_PORTS);
    /* an explicit stack: SDL's HID enumeration runs on this thread, and the
     * platform default is small on macOS */
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4u << 20);
    if (pthread_create(&g->thread, &attr, thread_main, g) != 0) {
        pthread_attr_destroy(&attr);
        session_set_error(s, "the gamepad thread could not start");
        pthread_mutex_destroy(&g->lock);
        free(g);
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        return -1;
    }
    pthread_attr_destroy(&attr);
    s->gamepad = g;
    return 0;
}

void gamepad_stop(pmd85session *s)
{
    gamepad_state *g = s->gamepad;
    if (!g) return;
    g->stop_requested = 1;
    pthread_join(g->thread, NULL);
    s->pad_gen_base = g->generation;
    s->pad_restarted = 1;
    pthread_mutex_destroy(&g->lock);
    s->gamepad = NULL;
    free(g);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

/* ---- public API ------------------------------------------------------------- */

int pmd85session_gamepad_count(pmd85session *s)
{
    gamepad_state *g = s ? s->gamepad : NULL;
    int n;
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    n = g->npads;
    pthread_mutex_unlock(&g->lock);
    return n;
}

int pmd85session_gamepad_name(pmd85session *s, int idx, char *dst, int dstsz)
{
    gamepad_state *g = s ? s->gamepad : NULL;
    int n = 0;
    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!g) return 0;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads)
        n = snprintf(dst, (size_t)dstsz, "%s", g->pads[idx].name);
    pthread_mutex_unlock(&g->lock);
    return n;
}

void pmd85session_gamepad_assign(pmd85session *s, int idx, int port)
{
    gamepad_state *g = s ? s->gamepad : NULL;
    char text[PMD85_PAD_ASSIGN_TEXT_MAX];
    int changed = 0;
    if (!g) return;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads) {
        pad_slot *slot = &g->pads[idx];
        if (port >= 0 && port < PMD85_JOY_PORTS) {
            slot->assign = port;
            slot->by_name = 0;
            changed = pmd85_pad_table_set(&g->table, slot->guidstr, slot->name, port) > 0;
        } else {
            const char *kguid[MAX_PADS], *kname[MAX_PADS];
            int nkeep = 0;
            slot->assign = -1;
            slot->by_name = 0;
            for (int k = 0; k < g->npads; k++)
                if (k != idx && g->pads[k].assign >= 0) {
                    kguid[nkeep] = g->pads[k].guidstr;
                    kname[nkeep++] = g->pads[k].name;
                }
            changed = pmd85_pad_table_remove(&g->table, slot->guidstr, slot->name,
                                             nkeep, kguid, kname);
        }
        g->generation++;
        if (changed && pmd85_pad_table_encode(&g->table, text, sizeof text) < 0)
            changed = 0;
    }
    pthread_mutex_unlock(&g->lock);
    /* the settings are written on the caller's thread, the pad lock let go:
     * the pad thread never waits on the settings store */
    if (changed)
        pmd85session_set_str(s, "pad_assign", text);
}

int pmd85session_gamepad_assignment(pmd85session *s, int idx)
{
    gamepad_state *g = s ? s->gamepad : NULL;
    int r = -1;
    if (!g) return -1;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads) r = g->pads[idx].assign;
    pthread_mutex_unlock(&g->lock);
    return r;
}

int pmd85session_gamepad_effective_port(pmd85session *s, int idx)
{
    gamepad_state *g = s ? s->gamepad : NULL;
    int r = -1;
    if (!g) return -1;
    pthread_mutex_lock(&g->lock);
    if (idx >= 0 && idx < g->npads) r = effective_port_locked(g, idx);
    pthread_mutex_unlock(&g->lock);
    return r;
}

unsigned pmd85session_gamepad_generation(pmd85session *s)
{
    gamepad_state *g = s ? s->gamepad : NULL;
    unsigned r;
    if (!s) return 0;
    if (!g) return s->pad_gen_base;
    pthread_mutex_lock(&g->lock);
    r = g->generation;
    pthread_mutex_unlock(&g->lock);
    return r;
}
