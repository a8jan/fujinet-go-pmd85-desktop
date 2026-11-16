/*
 * Pmd85Window: main window of the GNOME frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "pmd85session.h"

G_BEGIN_DECLS

#define PMD85_TYPE_WINDOW (pmd85_window_get_type())
G_DECLARE_FINAL_TYPE(Pmd85Window, pmd85_window, PMD85, WINDOW,
                     AdwApplicationWindow)

GtkWidget *pmd85_window_new(AdwApplication *app, pmd85session *session);
void pmd85_window_toast(Pmd85Window *self, const char *message);
/* Stop and start the session with the current settings (FujiNet options). */
void pmd85_window_restart_session(Pmd85Window *self);
/* Re-read the display settings (aspect, smoothing) from the session. */
void pmd85_window_apply_display(Pmd85Window *self);
/* Bring the menu's radio items in line with the session (after Preferences
 * changed something). */
void pmd85_window_sync_menu(Pmd85Window *self);

/* main.c: the icon name to use, which differs between an installed app and
 * one running out of the build tree. */
const char *pmd85_icon_name(void);

G_END_DECLS
