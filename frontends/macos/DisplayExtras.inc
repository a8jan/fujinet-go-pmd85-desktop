/*
 * DisplayExtras.inc -- the keyboard, the Mouse 602 and the aspect modes,
 * included into the display view's @implementation (see DisplayGlue.h).
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

- (void)extraInit {}

/* The frame clock follows the window (viewDidMoveToWindow); the app asks for
 * it explicitly once the window is ordered in. */
- (void)start { [self updateVisibility]; }

/* PMD85SESSION_ASPECT_*: the TV's 4:3, square pixels, whole-number scale. */
- (void)setAspectMode:(int)mode
{
    atomic_store(&s_fn_aspect_mode, mode);
    atomic_store(&_redraw, 1);
    [self setNeedsDisplay:YES];
}

/* ---- the keyboard ------------------------------------------------------------
 * Every key goes to the PMD (KeyForward.m), modifiers included: Shift and
 * Ctrl (STOP) are keys of the PMD's own, pressed and released as such. The
 * app's own keys are Command ones (the menu takes them before they get
 * here); Command itself goes to the machine as the key GPMD85Emulator
 * clears its keyboard for while held, so a shortcut never types into the
 * PMD. */

- (BOOL)acceptsFirstResponder
{
    return YES;
}

- (void)keyDown:(NSEvent *)event
{
    PmdForwardKey(_session, event, 1);
}

- (void)keyUp:(NSEvent *)event
{
    PmdForwardKey(_session, event, 0);
}

- (void)flagsChanged:(NSEvent *)event
{
    PmdForwardModifier(_session, event);
}

/* ---- the Mouse 602 -----------------------------------------------------------
 * The host pointer over the picture, in the PMD's pixels, to the session (as
 * GPMD85Emulator hands the pointer over its screen to the mouse); off the
 * picture is "left the screen". The pointer is hidden over the picture
 * while the machine has the mouse (setting "mouse_hide_pointer", on): PMD
 * software draws its own. */

- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc] initWithRect:[self bounds]
        options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                NSTrackingActiveInKeyWindow | NSTrackingCursorUpdate
        owner:self userInfo:nil];
    [self addTrackingArea:_tracking];
}

static NSCursor *pmd_blank_cursor(void)
{
    static NSCursor *blank;
    if (!blank) {
        NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(16, 16)];
        blank = [[NSCursor alloc] initWithImage:img hotSpot:NSZeroPoint];
    }
    return blank;
}

- (void)setPointerHidden:(BOOL)hide
{
    if (hide == _ptrHidden) return;
    _ptrHidden = hide;
    [(hide ? pmd_blank_cursor() : [NSCursor arrowCursor]) set];
}

- (void)cursorUpdate:(NSEvent *)e
{
    (void)e;
    [(_ptrHidden ? pmd_blank_cursor() : [NSCursor arrowCursor]) set];
}

/* where: the event's place; inside NO when the pointer left the view */
- (void)pointer:(NSEvent *)e inView:(BOOL)inView
{
    const BOOL on = pmd85session_mouse_on(_session) != 0;
    const NSRect pic = [self pictureRect];
    BOOL inside = NO;
    int x = 0, y = 0;

    if (on && inView && pic.size.width > 0 && pic.size.height > 0 && _width > 0 && _height > 0) {
        const NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
        /* AppKit counts up from the bottom; the frame down from the top */
        const double u = (p.x - pic.origin.x) / pic.size.width;
        const double v = (pic.origin.y + pic.size.height - p.y) / pic.size.height;
        if (u >= 0 && u < 1 && v >= 0 && v < 1) {
            inside = YES;
            x = (int)(u * _width);
            y = (int)(v * _height);
        }
    }
    if (on) {
        const NSUInteger pressed = inside ? [NSEvent pressedMouseButtons] : 0;
        const unsigned buttons = ((pressed & 1) ? PMD85SESSION_MOUSE_LEFT : 0u) |
                                 ((pressed & 2) ? PMD85SESSION_MOUSE_RIGHT : 0u) |
                                 ((pressed & 4) ? PMD85SESSION_MOUSE_MIDDLE : 0u);
        pmd85session_mouse(_session, x, y, buttons, inside);
    }
    [self setPointerHidden:inside && pmd85session_get_int(_session, "mouse_hide_pointer", 1)];
}

- (void)mouseMoved:(NSEvent *)e { [self pointer:e inView:YES]; }
- (void)mouseDragged:(NSEvent *)e { [self pointer:e inView:YES]; }
- (void)rightMouseDragged:(NSEvent *)e { [self pointer:e inView:YES]; }
- (void)otherMouseDragged:(NSEvent *)e { [self pointer:e inView:YES]; }
- (void)mouseDown:(NSEvent *)e { [self pointer:e inView:YES]; }
- (void)mouseUp:(NSEvent *)e { [self pointer:e inView:YES]; }
- (void)rightMouseDown:(NSEvent *)e { [self pointer:e inView:YES]; }
- (void)rightMouseUp:(NSEvent *)e { [self pointer:e inView:YES]; }
- (void)otherMouseDown:(NSEvent *)e { [self pointer:e inView:YES]; }
- (void)otherMouseUp:(NSEvent *)e { [self pointer:e inView:YES]; }
- (void)mouseEntered:(NSEvent *)e { [self pointer:e inView:YES]; }
- (void)mouseExited:(NSEvent *)e { [self pointer:e inView:NO]; }

/* the PMD's right button is a click here, not the view's context menu */
- (NSMenu *)menuForEvent:(NSEvent *)e
{
    (void)e;
    return nil;
}
