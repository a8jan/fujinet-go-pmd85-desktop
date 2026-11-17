/*
 * The FujiNet web UI (in the system browser) and the console log window.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "pmd85session.h"

G_BEGIN_DECLS

/* Opens the FujiNet web UI in the system browser; shows (raising an existing
 * one) the FujiNet console log window. */
void pmd85_fujinet_config_show(GtkWindow *parent, pmd85session *session);
void pmd85_fujinet_log_show(GtkWindow *parent, pmd85session *session);

G_END_DECLS
