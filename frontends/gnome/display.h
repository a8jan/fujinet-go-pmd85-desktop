/*
 * Pmd85Display: the emulator video widget for the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "pmd85session.h"

G_BEGIN_DECLS

#define PMD85_TYPE_DISPLAY (pmd85_display_get_type())
G_DECLARE_FINAL_TYPE(Pmd85Display, pmd85_display, PMD85, DISPLAY, GtkWidget)

typedef enum {
    /* Default: what the machine looked like on a television -- the PMD's
     * 288x256 picture filled a 4:3 screen, so its pixels are wider than
     * tall. */
    PMD85_ASPECT_TV_4_3 = 0,
    /* One emulated pixel per display pixel, i.e. the frame's own ratio. */
    PMD85_ASPECT_SQUARE_PIXELS = 1,
    PMD85_ASPECT_INTEGER = 2,
} Pmd85AspectMode;

/* How the picture is stretched to the window (integer mode is always
 * nearest: its scale is whole already). */
typedef enum {
    /* Default: an integer nearest-neighbour blow-up, then a linear filter
     * for the remaining fraction -- even, crisp pixels at any size. */
    PMD85_SCALING_SHARP = 0,
    /* Nearest-neighbour: crisp, but pixels of uneven width at most sizes. */
    PMD85_SCALING_NEAREST = 1,
    /* Linear: soft. */
    PMD85_SCALING_SMOOTH = 2,
} Pmd85Scaling;

GtkWidget *pmd85_display_new(pmd85session *session);
void pmd85_display_set_aspect_mode(Pmd85Display *self, Pmd85AspectMode mode);
void pmd85_display_set_scaling(Pmd85Display *self, Pmd85Scaling scaling);

G_END_DECLS
