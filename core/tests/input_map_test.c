/*
 * pmd85_key_from_event: X11 keysym -> USB HID usage (GPMD85Emulator's key
 * tables), positional on a US layout.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>

#include "pmd85session.h"

static int failures;

static void expect(const char *what, uint32_t keysym, uint32_t unicode, int want)
{
    unsigned usage = 0;
    int ok = pmd85_key_from_event(keysym, unicode, 0, 0, &usage);
    int got = ok ? (int)usage : 0;
    if (got != want) {
        fprintf(stderr, "FAIL %s: keysym 0x%X -> 0x%02X, want 0x%02X\n",
                what, keysym, got, want);
        failures++;
    }
}

int main(void)
{
    expect("a", 'a', 'a', 0x04);
    expect("A (shifted)", 'A', 'A', 0x04);
    expect("z", 'z', 'z', 0x1D);
    expect("1", '1', '1', 0x1E);
    expect("! is the 1 key", '!', '!', 0x1E);
    expect("0", '0', '0', 0x27);
    expect(") is the 0 key", ')', ')', 0x27);
    expect("space", ' ', ' ', 0x2C);
    expect("; ", ';', ';', 0x33);
    expect(": is the ; key", ':', ':', 0x33);
    expect("` (WRK)", '`', '`', 0x35);
    expect("Return (right EOL)", 0xFF0D, 0, 0x28);
    expect("KP_Enter (left EOL)", 0xFF8D, 0, 0x58);
    expect("Escape (STOP)", 0xFF1B, 0, 0x29);
    expect("Tab (C-D)", 0xFF09, 0, 0x2B);
    expect("ISO_Left_Tab (Shift+C-D)", 0xFE20, 0, 0x2B);
    expect("BackSpace", 0xFF08, 0, 0x2A);
    expect("Insert (INS)", 0xFF63, 0, 0x49);
    expect("Delete (DEL)", 0xFFFF, 0, 0x4C);
    expect("Home", 0xFF50, 0, 0x4A);
    expect("End", 0xFF57, 0, 0x4D);
    expect("Prior (RCL)", 0xFF55, 0, 0x4B);
    expect("Next (CLR)", 0xFF56, 0, 0x4E);
    expect("Left", 0xFF51, 0, 0x50);
    expect("Up", 0xFF52, 0, 0x52);
    expect("Right", 0xFF53, 0, 0x4F);
    expect("Down", 0xFF54, 0, 0x51);
    expect("F1 (K0)", 0xFFBE, 0, 0x3A);
    expect("F10 (K9)", 0xFFC7, 0, 0x43);
    expect("F11 (K10, CONFIG's BOOT)", 0xFFC8, 0, 0x44);
    expect("F12 (reserved: debugger)", 0xFFC9, 0, 0);
    expect("Shift_L", 0xFFE1, 0, 0xE1);
    expect("Control_L (STOP)", 0xFFE3, 0, 0xE0);
    expect("KP_0", 0xFFB0, 0, 0x62);
    expect("KP_1", 0xFFB1, 0, 0x59);
    expect("KP_9", 0xFFB9, 0, 0x61);
    expect("unknown keysym", 0x1008FF11, 0, 0);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("input_map: ok\n");
    return 0;
}
