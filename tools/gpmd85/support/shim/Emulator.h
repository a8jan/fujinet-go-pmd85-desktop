/*
 * Emulator.h -- a stand-in for GPMD85Emulator's application header, for the
 * staged TapeBrowser.cpp ONLY.
 *
 * Upstream's tape deck (TTapeBrowser) is machine-level code, but it reads
 * four of the application's settings through Emulator.h's `Settings` global:
 * Settings->TapeBrowser->{flash, monitoring, autoStop, fileName}. TEmulator
 * itself is never built here (core/pmd85/pmd85_host.cpp stands in for it),
 * so this supplies just that much; pmd85_host.cpp owns the object and sets
 * the values from the session's settings. Found ahead of anything else on
 * the staged core's include path, like SDL.h next to it.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PMD85_EMULATOR_SHIM_H
#define PMD85_EMULATOR_SHIM_H

#include "globals.h"

struct TSettings {
    struct SetTapeBrowser {
        bool hex;
        bool flash;               /* blocks into memory at once (ROM traps) */
        bool monitoring;          /* the tape signal on the speaker */
        TAutoStopType autoStop;
        char *fileName;           /* only read by the constructor: NULL */
    };
    SetTapeBrowser *TapeBrowser;
};

extern TSettings *Settings;

#endif /* PMD85_EMULATOR_SHIM_H */
