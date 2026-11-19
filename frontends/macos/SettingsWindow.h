/*
 * The Settings window: Machine, Display, Input and FujiNet, as the GNOME
 * app's Preferences (frontends/gnome/prefs.c), over the same settings.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "pmd85session.h"

@class PmdAppDelegate;

@interface PmdSettingsWindow : NSObject <NSWindowDelegate, NSTextFieldDelegate>
- (instancetype)initWithSession:(pmd85session *)session app:(PmdAppDelegate *)app;
- (void)show;
@end
