/*
 * AppKit key events to the X11 keysym / unicode pair pmd85session_key takes.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>
#include <stdint.h>

#include "pmd85session.h"

/* The keysym and unicode of a key event; NO when it is not a key the PMD
 * has. */
BOOL PmdKeyFromEvent(NSEvent *event, uint32_t *keysym, uint32_t *unicode);

/* A key press (down 1) or release (down 0) to the session. Keys typed with
 * Command held are the app's (a shortcut the menu did not take) and are not
 * forwarded. */
void PmdForwardKey(pmd85session *session, NSEvent *event, int down);

/* A modifier went down or up (NSEvent flagsChanged): Shift, Control (the
 * PMD's STOP), Option and Command, each side on its own, to the session. */
void PmdForwardModifier(pmd85session *session, NSEvent *event);
