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
/* Ask for a ROM module image (.rmm .mrm) and, if one is picked and loads,
 * switch the slot to it. done(data, chosen) runs either way, then
 * destroy(data). */
typedef void (*Pmd85ModuleFileDone)(gpointer data, gboolean chosen);
void pmd85_window_choose_module_file(Pmd85Window *self, Pmd85ModuleFileDone done,
                                     gpointer data, GDestroyNotify destroy);

/* The tape deck's file dialogs and Eject (with its unsaved-changes question),
 * shown over parent (NULL: this window); results are toasts here. step: the
 * previous (-1) or next (1) file -- a header block, data blocks passed
 * over -- as the tape's position, 0 or -1. */
void pmd85_window_tape_open(Pmd85Window *self, GtkWindow *parent, gboolean append);
void pmd85_window_tape_save(Pmd85Window *self, GtkWindow *parent);
void pmd85_window_tape_eject(Pmd85Window *self, GtkWidget *parent);
int  pmd85_window_tape_step(Pmd85Window *self, int delta);
/* the block the step would go to: the previous/next file, -1 none */
int  pmd85_window_tape_file(Pmd85Window *self, int delta);

/* main.c: the icon name to use, which differs between an installed app and
 * one running out of the build tree. */
const char *pmd85_icon_name(void);

G_END_DECLS
