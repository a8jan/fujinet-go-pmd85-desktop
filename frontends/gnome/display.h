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

GtkWidget *pmd85_display_new(pmd85session *session);
void pmd85_display_set_aspect_mode(Pmd85Display *self, Pmd85AspectMode mode);
void pmd85_display_set_smooth(Pmd85Display *self, gboolean smooth);

G_END_DECLS
