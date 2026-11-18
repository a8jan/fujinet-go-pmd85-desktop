/*
 * joy_input -- the PMD 85's two joysticks (the 4004/482 interface: 1 on GPIO
 * port A, 2 on port B) merged from separate sources, after the Atari 800
 * app's input_merge.h: the frontends' own (an on-screen joystick, a test),
 * and for each connected pad its buttons and its stick / D-pad apart. A
 * joystick shows a direction while any source holds it -- opposite
 * directions held together cancel, as on a real stick whose switches cannot
 * both close -- and fire while any source fires. So letting go of one
 * source (a button released, a pad unplugged) never cancels another's hold,
 * and two pads on one joystick do not fight.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PMD85_JOY_INPUT_H
#define PMD85_JOY_INPUT_H

#include "pmd85session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PMD85_JOY_PORTS 2   /* 0 = joystick 1 (GPIO A), 1 = joystick 2 (GPIO B) */
#define PMD85_MAX_PADS 8

#define PMD85_SRC_FRONTEND 0                              /* pmd85session_set_joystick */
#define PMD85_SRC_PAD0     1                              /* + owner: its buttons */
#define PMD85_SRC_STICK0   (PMD85_SRC_PAD0 + PMD85_MAX_PADS)  /* + owner: stick, D-pad */
#define PMD85_SRC_COUNT    (PMD85_SRC_STICK0 + PMD85_MAX_PADS)
#define PMD85_SRC_PAD(owner)   (PMD85_SRC_PAD0 + (owner))
#define PMD85_SRC_STICK(owner) (PMD85_SRC_STICK0 + (owner))

/* The merge, pure: what a joystick shows (PMD85SESSION_JOY_* bits) for the
 * holds of n sources on it. */
unsigned pmd85_joy_merge(const unsigned *held, int n);

/* Which joystick each of npads pads drives: out[i] = 0, 1 or -1 (none).
 * assign[i] is the pad's explicit joystick (-1 automatic); avail has bit p
 * set for each joystick the machine has (port A is the PMD 32's when that is
 * connected). An explicit pad keeps its joystick (none if the machine lacks
 * it); automatic pads take the remaining joysticks in connection order, one
 * each. Two pads may be put on one joystick explicitly. Pure. */
void pmd85_pad_ports(const int *assign, int npads, unsigned avail, int *out);

/* What a stick at (x, y) (-1..1, +y down) holds, with hysteresis: an axis
 * engages past `enter` and lets go inside `exit`; prev is the last result.
 * PMD85SESSION_JOY_* direction bits. Pure. */
unsigned pmd85_dir_from_stick(float x, float y, unsigned prev, float enter, float exit);

/* The session's side (session.c): a source's hold on a joystick, the
 * PMD85SESSION_JOY_* bits it holds now (replacing what it held there). Any
 * thread. */
void joy_input_set(pmd85session *s, int src, int port, unsigned bits);
/* A pad gone (or moved): what its two sources held is let go everywhere. */
void joy_input_clear_pad(pmd85session *s, int owner);
/* Every source lets go (the session stops). */
void joy_input_clear_all(pmd85session *s);

#ifdef __cplusplus
}
#endif

#endif
