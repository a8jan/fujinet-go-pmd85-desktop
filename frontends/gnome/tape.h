/*
 * The tape browser: the deck's block list, transport and tape options.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "window.h"

G_BEGIN_DECLS

/* Shows (raising an existing one) the tape browser of main's session. */
void pmd85_tape_window_show(Pmd85Window *main, pmd85session *session);

G_END_DECLS
