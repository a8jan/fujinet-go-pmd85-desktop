/*
 * joy_input -- the pure parts of the joystick merge (see joy_input.h); the
 * per-session state is in session.c.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "joy_input.h"

unsigned pmd85_joy_merge(const unsigned *held, int n)
{
    unsigned any = 0, out;
    for (int i = 0; i < n; i++)
        any |= held[i];
    out = any & PMD85SESSION_JOY_FIRE;
    /* opposite directions together are neither */
    if ((any & (PMD85SESSION_JOY_UP | PMD85SESSION_JOY_DOWN)) !=
        (PMD85SESSION_JOY_UP | PMD85SESSION_JOY_DOWN))
        out |= any & (PMD85SESSION_JOY_UP | PMD85SESSION_JOY_DOWN);
    if ((any & (PMD85SESSION_JOY_LEFT | PMD85SESSION_JOY_RIGHT)) !=
        (PMD85SESSION_JOY_LEFT | PMD85SESSION_JOY_RIGHT))
        out |= any & (PMD85SESSION_JOY_LEFT | PMD85SESSION_JOY_RIGHT);
    return out;
}

void pmd85_pad_ports(const int *assign, int npads, unsigned avail, int *out)
{
    unsigned taken = 0;
    for (int i = 0; i < npads; i++) {
        out[i] = -1;
        if (assign[i] >= 0) {
            if (assign[i] < PMD85_JOY_PORTS && (avail & (1u << assign[i]))) {
                out[i] = assign[i];
                taken |= 1u << assign[i];
            }
        }
    }
    for (int i = 0; i < npads; i++) {
        if (assign[i] >= 0)
            continue;
        for (int p = 0; p < PMD85_JOY_PORTS; p++)
            if ((avail & (1u << p)) && !(taken & (1u << p))) {
                out[i] = p;
                taken |= 1u << p;
                break;
            }
    }
}

unsigned pmd85_dir_from_stick(float x, float y, unsigned prev, float enter, float exit)
{
    unsigned dir = 0;
    /* each axis on its own, so a diagonal is two bits and a stick held
     * exactly sideways never picks up a vertical bit from noise */
    if (x <= -enter || (x <= -exit && (prev & PMD85SESSION_JOY_LEFT))) dir |= PMD85SESSION_JOY_LEFT;
    if (x >=  enter || (x >=  exit && (prev & PMD85SESSION_JOY_RIGHT))) dir |= PMD85SESSION_JOY_RIGHT;
    if (y <= -enter || (y <= -exit && (prev & PMD85SESSION_JOY_UP))) dir |= PMD85SESSION_JOY_UP;
    if (y >=  enter || (y >=  exit && (prev & PMD85SESSION_JOY_DOWN))) dir |= PMD85SESSION_JOY_DOWN;
    return dir;
}
