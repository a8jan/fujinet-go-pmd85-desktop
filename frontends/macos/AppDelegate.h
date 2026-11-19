/*
 * The application delegate: the main window, the menu bar, the session and
 * the auxiliary windows (Settings, Tape, FujiNet Console Log).
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "pmd85session.h"

@interface PmdAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, NSMenuDelegate,
                                      NSMenuItemValidation>

- (instancetype)initWithSession:(pmd85session *)session;

/* A short message over the picture. */
- (void)toast:(NSString *)message;

/* The display options from the settings (aspect, scaling). */
- (void)applyDisplaySettings;
/* Stop and start the session with the settings (FujiNet, sound...). */
- (void)restartSession;

/* Asks for a ROM module file and switches to it; done(YES) when one was
 * taken. parent: the window the panel is a sheet of (nil: the main one). */
- (void)chooseModuleFileFrom:(NSWindow *)parent done:(void (^)(BOOL chosen))done;

/* The tape deck, shared by the menu and the Tape window. */
- (void)openTapeAppend:(BOOL)append from:(NSWindow *)parent;
- (void)saveTapeFrom:(NSWindow *)parent;
- (void)ejectTapeFrom:(NSWindow *)parent;
/* The previous (-1) or next (1) file on the tape -- a header block; the
 * data blocks a program loads by itself are passed over -- or -1. */
- (int)tapeFile:(int)delta;
/* ... made the tape's position: 0, or -1 (none, or the deck said no). */
- (int)tapeStep:(int)delta;

@end
