/*
 * The debugger window: the session's 8080 debugger engine (pmd85debug.h),
 * as the GNOME app's frontends/gnome/debugger/dbg_window.c. Option+F12 in
 * the main window or View > Debugger (Cmd+D) shows it, or hides it.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#import <Cocoa/Cocoa.h>

#include "pmd85session.h"

@interface PmdDebuggerWindow : NSObject
/* Shows the window (engaging the engine and stopping the machine), or hides
 * it (releasing the engine, so the machine runs on at full speed). */
+ (void)toggleForSession:(pmd85session *)session;
+ (void)showForSession:(pmd85session *)session;
+ (BOOL)isShown;
/* At quit, before the session stops: the timer, the key monitor and the
 * engine let go. */
+ (void)shutdown;
@end
