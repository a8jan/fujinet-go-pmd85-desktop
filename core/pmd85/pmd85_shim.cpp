/*
 * The one function the SDL stand-in (tools/gpmd85/support/shim/SDL.h)
 * declares rather than defines, plus the path globals GPMD85Emulator's
 * CommonUtils expects its main() to have set.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <chrono>

#include "CommonUtils.h"

extern "C" Uint32 SDL_GetTicks(void)
{
    using namespace std::chrono;
    static const steady_clock::time_point t0 = steady_clock::now();
    return (Uint32)duration_cast<milliseconds>(steady_clock::now() - t0).count();
}

/* CommonUtils' ComposeFilePath() resolves a relative path against
 * PathApplication (upstream: the executable's directory). The host only ever
 * hands the machine absolute paths; this keeps a relative one from
 * dereferencing NULL. */
namespace {
struct PathInit {
    PathInit()
    {
        static char dot[] = ".";
        if (!PathApplication) PathApplication = dot;
    }
} g_path_init;
}
