/*
 * Shared between the engine (debugger.c), the prompt (prompt.c) and the
 * memory tools (memtools.c). Not API.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PMD85_DEBUGGER_INTERNAL_H
#define PMD85_DEBUGGER_INTERNAL_H

#include "pmd85debug.h"
#include "symtab.h"

/* Every name an expression knows besides symbols, for Tab completion. */
extern const char *const pmd85debug_reg_names[];

pmd85symtab *pmd85debug_symtab(pmd85debug *d);

/* v as `digits` hex digits in the debugger's chosen style, into a small
 * per-thread ring (fine for several in one printf). d may be NULL. */
const char *pmd85hx(pmd85debug *d, unsigned v, int digits);

#endif /* PMD85_DEBUGGER_INTERNAL_H */
