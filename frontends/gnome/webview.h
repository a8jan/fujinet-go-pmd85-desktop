/*
 * FujiNet configuration (embedded web UI) and console log windows.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

#include "pmd85session.h"

G_BEGIN_DECLS

/* Web UI window (WebKitGTK when built with it, otherwise opens the system
 * browser) and the FujiNet console log window. */
void pmd85_fujinet_config_show(GtkWindow *parent, pmd85session *session);
void pmd85_fujinet_log_show(GtkWindow *parent, pmd85session *session);

G_END_DECLS
