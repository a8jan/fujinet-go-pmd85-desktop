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

#include "pmd85debug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called on the emulator thread before every instruction, with the PC it is
 * about to execute. It may block (the debugger parks the machine here while
 * it is stopped); the instruction runs when it returns. NULL = no hook, which
 * is the normal, full-speed path. */
typedef void (*Pmd85InstrHook)(void *ctx, uint16_t pc);
void pmd85host_set_instr_hook(Pmd85InstrHook hook, void *ctx);

/* ---- the debugger's view of the machine ----------------------------------
 * Emulator thread, or any thread while the emulator thread is parked in the
 * instruction hook (the machine is then quiescent). Each returns 0 / -1 when
 * there is no machine. */

typedef struct {
    uint16_t pc, sp, af, bc, de, hl;
    int iff;    /* interrupts enabled */
    int halt;   /* in HLT */
    int inta;   /* an interrupt is acknowledged by the next DoInstruction */
} pmd85host_cpu;
int pmd85host_dbg_cpu(pmd85host_cpu *out);
/* PC SP AF BC DE HL from `in`; the flip-flops are left alone. */
int pmd85host_dbg_cpu_set(const pmd85host_cpu *in);

/* A byte as the CPU would read it now, without a bus cycle (0xFF where
 * nothing is mapped), or -1 with no machine. */
int pmd85host_dbg_peek(uint16_t addr);
/* Stores where the CPU reads RAM or VRAM now; 1 if stored. Not where reads
 * come from ROM, even when writes there reach the RAM below. */
int pmd85host_dbg_poke(uint16_t addr, uint8_t value);
/* How the CPU sees addr now: PMD85HOST_MEM_* bits. */
enum { PMD85HOST_MEM_R = 1, PMD85HOST_MEM_W = 2, PMD85HOST_MEM_VRAM = 4,
       PMD85HOST_MEM_VRAM_ASIDE = 8,
       PMD85HOST_MEM_ROM = 16 };   /* reads come from the ROM image */
int pmd85host_dbg_mem_state(uint16_t addr);

/* CPU T-states since the machine was built, frames run, and T-states into
 * the current frame (0..40959). */
int pmd85host_dbg_clock(uint64_t *tstates, uint64_t *frame, int *in_frame);

/* The running machine's identity: changes when a reconfiguration rebuilds
 * it. Never dereferenced outside the host. */
void *pmd85host_machine_ptr(void);

/* The model of the running machine (PMD85_MODEL_*), -1 for none. */
int pmd85host_dbg_model(void);

/* The name of what answers an I/O port on this machine as configured
 * ("SYS PIO PA", "BECKER data"...), or NULL when nothing does. */
const char *pmd85host_port_name(uint8_t port);

/* The machine's I/O decode as configured, in the order the CPU looks for a
 * device (the first match answers). The PMD's chips decode only some address
 * bits, so each register answers at several port numbers (mirrors). */
typedef struct {
    const char *name;           /* "SYS PIO", "BECKER" (static) */
    uint8_t mask, adr;          /* it answers when (port & mask) == adr */
    uint8_t reg;                /* the port bits that pick its register */
    const char *const *regs;    /* the register names, by (port & reg) */
} pmd85host_port_dev;
#define PMD85HOST_PORT_DEVS_MAX 16
int pmd85host_port_devs(pmd85host_port_dev *out, int max);

/* The chips, into the debugger's public structure. */
int pmd85host_dbg_io(pmd85debug_io *out);

#ifdef __cplusplus
}

class PmdMachine;
/* The running machine; emulator thread only (it is replaced by a
 * reconfiguration at a frame boundary). */
PmdMachine *pmd85host_machine(void);
#endif

#endif /* PMD85_HOST_INTERNAL_H */
