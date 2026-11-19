/*
 * DisplayView: the emulator's picture, the keyboard and the Mouse 602.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "pmd85session.h"

/* How the picture is stretched (setting "scaling", as the GNOME app's). */
enum {
    PMD_SCALING_SHARP = 0,   /* whole-pixel blow-up, a linear blend only at the seams */
    PMD_SCALING_NEAREST,
    PMD_SCALING_SMOOTH,
};

@interface DisplayView : NSView

- (instancetype)initWithSession:(pmd85session *)session;
- (void)start;
- (void)stop;
/* PMD85SESSION_ASPECT_* */
- (void)setAspectMode:(int)mode;
/* PMD_SCALING_* */
- (void)setScaling:(int)scaling;

@end
