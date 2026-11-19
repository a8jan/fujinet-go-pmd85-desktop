/*
 * The Tape window: GPMD85Emulator's tape browser (as the GNOME app's
 * frontends/gnome/tape.c) -- the tape's files, the position and the STOP
 * marker, Play/Stop, previous/next file, flash load and tape sound.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "pmd85session.h"

@class PmdAppDelegate;

@interface PmdTapeWindow : NSObject <NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate,
                                     NSMenuItemValidation>
- (instancetype)initWithSession:(pmd85session *)session app:(PmdAppDelegate *)app;
- (void)show;
@end
