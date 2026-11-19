/*
 * KeyForward: AppKit keys to the session.
 *
 * NSEvent.charactersIgnoringModifiers gives a layout-aware character (Shift
 * still applied, which core/src/input_map.c undoes); the function-key table
 * is X11 keysym values (0xFF52 = XK_Up), the session does the PMD-key
 * translation. The modifiers are keys of the PMD's own (Shift, Ctrl = STOP)
 * and arrive as flagsChanged events, not keyDown/keyUp: their press and
 * release are read from the device-dependent bits of modifierFlags.
 *
 * From fujinet-go-coco-desktop's KeyForward.m.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#import "KeyForward.h"

/* X11 keysyms */
enum {
    XK_BACKSPACE = 0xFF08, XK_TAB = 0xFF09, XK_RETURN = 0xFF0D, XK_ESCAPE = 0xFF1B,
    XK_HOME = 0xFF50, XK_LEFT = 0xFF51, XK_UP = 0xFF52, XK_RIGHT = 0xFF53,
    XK_DOWN = 0xFF54, XK_PAGE_UP = 0xFF55, XK_PAGE_DOWN = 0xFF56, XK_END = 0xFF57,
    XK_INSERT = 0xFF63, XK_KP_ENTER = 0xFF8D, XK_KP_MULT = 0xFFAA, XK_KP_ADD = 0xFFAB,
    XK_KP_SUB = 0xFFAD, XK_KP_DEC = 0xFFAE, XK_KP_DIV = 0xFFAF, XK_KP_0 = 0xFFB0,
    XK_F1 = 0xFFBE, XK_DELETE = 0xFFFF,
    XK_SHIFT_L = 0xFFE1, XK_SHIFT_R = 0xFFE2, XK_CONTROL_L = 0xFFE3, XK_CONTROL_R = 0xFFE4,
    XK_ALT_L = 0xFFE9, XK_ALT_R = 0xFFEA, XK_SUPER_L = 0xFFEB, XK_SUPER_R = 0xFFEC,
};

static uint32_t keysym_for_char(unichar chr, BOOL keypad)
{
    if (keypad) {
        /* the numeric keypad: its own keys on the PMD ("numpad" setting) */
        if (chr >= '0' && chr <= '9') return XK_KP_0 + (uint32_t)(chr - '0');
        switch (chr) {
        case '*': return XK_KP_MULT;
        case '+': return XK_KP_ADD;
        case '-': return XK_KP_SUB;
        case '.': case ',': return XK_KP_DEC;
        case '/': return XK_KP_DIV;
        case 0x03: case 0x0D: return XK_KP_ENTER;
        default: break;
        }
    }
    switch (chr) {
    case NSUpArrowFunctionKey:    return XK_UP;
    case NSDownArrowFunctionKey:  return XK_DOWN;
    case NSLeftArrowFunctionKey:  return XK_LEFT;
    case NSRightArrowFunctionKey: return XK_RIGHT;
    case NSHomeFunctionKey:       return XK_HOME;
    case NSEndFunctionKey:        return XK_END;
    case NSPageUpFunctionKey:     return XK_PAGE_UP;
    case NSPageDownFunctionKey:   return XK_PAGE_DOWN;
    case NSInsertFunctionKey:
    case NSHelpFunctionKey:       return XK_INSERT;   /* Help sits where Insert is */
    case NSDeleteFunctionKey:     return XK_DELETE;   /* forward delete */
    case 0x7F: case 0x08:         return XK_BACKSPACE;
    case 0x0D: case 0x03:         return XK_RETURN;
    case 0x09: case 0x19:         return XK_TAB;      /* 0x19: Shift+Tab */
    case 0x1B:                    return XK_ESCAPE;
    default:
        /* F1..F11 are the PMD's K0..K10 (F12 is the debugger's) */
        if (chr >= NSF1FunctionKey && chr <= NSF1FunctionKey + 10)
            return XK_F1 + (uint32_t)(chr - NSF1FunctionKey);
        return 0;
    }
}

BOOL PmdKeyFromEvent(NSEvent *event, uint32_t *keysym, uint32_t *unicode)
{
    NSString *chars = event.charactersIgnoringModifiers;
    const unichar chr = chars.length ? [chars characterAtIndex:0] : 0;
    const BOOL keypad = (event.modifierFlags & NSEventModifierFlagNumericPad) != 0 &&
                        !(chr >= 0xF700 && chr <= 0xF8FF);   /* arrows report keypad too */

    *keysym = keysym_for_char(chr, keypad);
    *unicode = (chr >= 0x20 && chr < 0x7F) ? chr : 0;
    if (!*keysym && *unicode) *keysym = *unicode;   /* printable ASCII: the keysym is the char */
    return *keysym != 0;
}

void PmdForwardKey(pmd85session *session, NSEvent *event, int down)
{
    uint32_t keysym = 0, unicode = 0;

    if (!session) return;
    if (down && (event.modifierFlags & NSEventModifierFlagCommand))
        return;   /* the app's (or the system's), not the PMD's */
    if (down && event.isARepeat)
        return;   /* the PMD repeats by itself */
    if (!PmdKeyFromEvent(event, &keysym, &unicode))
        return;
    pmd85session_key(session, down, keysym, unicode,
                     (event.modifierFlags & NSEventModifierFlagControl) != 0,
                     (event.modifierFlags & NSEventModifierFlagShift) != 0);
}

/* The device-dependent modifier bits (IOKit's NX_DEVICE*KEYMASK) say which
 * side is down. */
enum {
    DEV_LCTL = 0x0001, DEV_LSHIFT = 0x0002, DEV_RSHIFT = 0x0004, DEV_LCMD = 0x0008,
    DEV_RCMD = 0x0010, DEV_LALT = 0x0020, DEV_RALT = 0x0040, DEV_RCTL = 0x2000,
};

void PmdForwardModifier(pmd85session *session, NSEvent *event)
{
    uint32_t keysym;
    unsigned bit;
    const NSUInteger flags = event.modifierFlags;

    if (!session) return;
    switch (event.keyCode) {
    case 56: keysym = XK_SHIFT_L;   bit = DEV_LSHIFT; break;
    case 60: keysym = XK_SHIFT_R;   bit = DEV_RSHIFT; break;
    case 59: keysym = XK_CONTROL_L; bit = DEV_LCTL;   break;
    case 62: keysym = XK_CONTROL_R; bit = DEV_RCTL;   break;
    case 58: keysym = XK_ALT_L;     bit = DEV_LALT;   break;
    case 61: keysym = XK_ALT_R;     bit = DEV_RALT;   break;
    case 55: keysym = XK_SUPER_L;   bit = DEV_LCMD;   break;
    case 54: keysym = XK_SUPER_R;   bit = DEV_RCMD;   break;
    default: return;   /* Caps Lock, Fn: not the PMD's */
    }
    const int down = (flags & bit) != 0;
    if (down && (keysym == XK_SUPER_L || keysym == XK_SUPER_R)) {
        /* With Command held AppKit sends no key-up for a letter let go, so
         * nothing held before it may outlive it: let go of everything, then
         * hold Command (the PMD's keyboard is ignored while it is). */
        pmd85session_release_all(session);
    }
    pmd85session_key(session, down, keysym, 0, (flags & NSEventModifierFlagControl) != 0,
                     (flags & NSEventModifierFlagShift) != 0);
    if (!down && (keysym == XK_SUPER_L || keysym == XK_SUPER_R)) {
        /* Shift or Ctrl still held when Command goes: the PMD has them again */
        if (flags & DEV_LSHIFT) pmd85session_key(session, 1, XK_SHIFT_L, 0, 0, 1);
        if (flags & DEV_RSHIFT) pmd85session_key(session, 1, XK_SHIFT_R, 0, 0, 1);
        if (flags & DEV_LCTL) pmd85session_key(session, 1, XK_CONTROL_L, 0, 1, 0);
        if (flags & DEV_RCTL) pmd85session_key(session, 1, XK_CONTROL_R, 0, 1, 0);
    }
}
