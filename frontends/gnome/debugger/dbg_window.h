/*
 * Debugger window for the GNOME frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>

#include "pmd85session.h"

G_BEGIN_DECLS

/* Shows (creating on first use) the debugger window for the session.
 * Showing engages the debugger engine and stops the machine. */
void pmd85_debugger_show(GtkWindow *parent, pmd85session *session);
/* Alt+F12: shows it, or hides it (releasing the engine, so the machine runs on). */
void pmd85_debugger_toggle(GtkWindow *parent, pmd85session *session);

G_END_DECLS
