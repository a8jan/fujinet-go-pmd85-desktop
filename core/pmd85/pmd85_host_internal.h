/*
 * pmd85host internals shared with the debugger engine. Nothing here is API
 * for the frontends; they include pmd85session.h / pmd85debug.h only.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PMD85_HOST_INTERNAL_H
#define PMD85_HOST_INTERNAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called on the emulator thread before every instruction, with the PC it is
 * about to execute. It may block (the debugger parks the machine here while
 * it is stopped); the instruction runs when it returns. NULL = no hook, which
 * is the normal, full-speed path. */
typedef void (*Pmd85InstrHook)(void *ctx, uint16_t pc);
void pmd85host_set_instr_hook(Pmd85InstrHook hook, void *ctx);

#ifdef __cplusplus
}

class PmdMachine;
/* The running machine; emulator thread only (it is replaced by a
 * reconfiguration at a frame boundary). */
PmdMachine *pmd85host_machine(void);
#endif

#endif /* PMD85_HOST_INTERNAL_H */
