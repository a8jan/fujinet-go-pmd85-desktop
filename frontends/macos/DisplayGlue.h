/*
 * DisplayGlue.h -- what DisplayView.m needs to know about THIS machine: the
 * session's functions, the frame size and the shape of the picture.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdatomic.h>

#import "KeyForward.h"
#include "pmd85session.h"

#define FN_VIEW      DisplayView
#define FN_SESSION   pmd85session
#define FN_FB_W      PMD85SESSION_FB_MAX_WIDTH
#define FN_FB_H      PMD85SESSION_FB_MAX_HEIGHT
#define FN_INITIAL_W 960
#define FN_INITIAL_H 720

/* 288 wide, 384 on a Consul 2717: the frame's own size drives the picture. */
#define FN_VARIABLE_WIDTH 1

/* The keyboard, the Mouse 602, start() and the aspect modes
 * (DisplayExtras.inc). */
#define FN_EXTRA_IVARS \
    NSTrackingArea *_tracking; \
    BOOL _ptrHidden;
#define FN_EXTRA_METHODS "DisplayExtras.inc"

/* Whole-number scaling in mode 2. */
#define FN_SNAP_SIZE 1

/* 0: the television's 4:3, 1: square pixels, 2: whole-number scale
 * (PMD85SESSION_ASPECT_*). */
static _Atomic int s_fn_aspect_mode = 0;

/* The PMD runs at 50 Hz, which no Mac panel does: the pacer keeps its own
 * clock (pmd85session_notify_vsync). */
static inline int fn_machine_hz(FN_SESSION *s) { (void)s; return 50; }

static inline void fn_notify_vsync(FN_SESSION *s, int64_t ns)
{
    pmd85session_notify_vsync(s, ns);
}

static inline int fn_pull_frame_w(FN_SESSION *s, uint32_t *dst, int *width, int *lines, uint64_t *serial)
{
    int w = 0, h = 0;
    if (!pmd85session_copy_frame(s, dst, serial, &w, &h) || w <= 0 || h <= 0)
        return 0;
    *width = w;
    *lines = h;
    return 1;
}

static inline double fn_picture_aspect_w(FN_SESSION *s, int width, int lines, BOOL tv)
{
    (void)s; (void)tv;
    return atomic_load(&s_fn_aspect_mode) == PMD85SESSION_ASPECT_TV ? 4.0 / 3.0
                                                                    : (double)width / (double)lines;
}

/* Mode 2: the largest whole multiple of the frame that fits (at least 1). */
static inline BOOL fn_snap_size(CGSize avail, int width, int lines, CGSize *out)
{
    if (atomic_load(&s_fn_aspect_mode) != PMD85SESSION_ASPECT_INTEGER) return NO;
    int scale = (int)fmin(avail.width / width, avail.height / lines);
    if (scale < 1) scale = 1;
    *out = CGSizeMake(scale * width, scale * lines);
    return YES;
}
