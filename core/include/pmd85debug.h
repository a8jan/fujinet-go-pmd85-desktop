/*
 * pmd85debug -- the 8080 debugger engine shared by every frontend's debugger
 * window. Its surface follows the family's (fujinet-go-coco-desktop's
 * cocodebug.h) wherever the concept carries over, plus the I/O-port
 * debugging of fujinet-go-adam-desktop (port breakpoints, port names, an I/O
 * access log).
 *
 * HOW IT SEES THE MACHINE. GPMD85Emulator's sources are compiled unmodified,
 * so there is no bus tap inside the CPU. Instead the host calls the engine
 * before every instruction (pmd85_host_internal.h), and that is enough on an
 * 8080: every memory or I/O access an instruction makes follows from its
 * opcode and the registers (HL, BC, DE, SP, a direct address, an IN/OUT
 * port). The engine decodes the accesses of the instruction about to run,
 * remembers the old values, and checks them at the next instruction, when
 * the new values are there -- so a read/write/port breakpoint stops right
 * AFTER the instruction that hit it, with "wrote 0400h = 41h (was 20h) at
 * A1CBh" as the reason. (An interrupt's RST push is not seen; the PMD 85
 * leaves interrupts off.) Memory is read without a bus cycle.
 *
 * THREADING. Everything is called from a UI thread except where noted.
 * While the machine is paused the emulator thread is parked inside the
 * hook, so the machine is quiescent and reading its memory and registers
 * from another thread is safe; that is why those calls are only valid while
 * paused, and say so.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PMD85DEBUG_H
#define PMD85DEBUG_H

#include <stdint.h>

#include "i8080dasm.h"
#include "pmd85session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PMD85DEBUG_MAX_BREAKPOINTS 128

/* The one address space the memory calls take: the 64K the CPU sees now. */
#define PMD85DEBUG_SPACE_CPU 0

/* ---- engage / pause / step ---------------------------------------------- */

/* Engaging installs the per-instruction hook (a session that never opens
 * the debugger pays nothing); it does not pause. Releasing resumes first. */
void pmd85debug_set_engaged(pmd85debug *d, int engaged);
int  pmd85debug_is_engaged(const pmd85debug *d);

/* Pause at the next instruction boundary (returns at once; the stop serial
 * is how a UI notices), or resume. */
void pmd85debug_pause(pmd85debug *d);
void pmd85debug_resume(pmd85debug *d);
int  pmd85debug_is_paused(const pmd85debug *d);

void pmd85debug_step(pmd85debug *d);               /* one instruction */
/* Over a CALL/Ccc/RST (a one-shot breakpoint after it); else a step. */
void pmd85debug_step_over(pmd85debug *d);
/* Until a RET-class instruction has run with SP above where it is now. */
void pmd85debug_step_out(pmd85debug *d);
void pmd85debug_run_to(pmd85debug *d, uint16_t addr);
/* Run n whole frames (20 ms each) and stop at the start of the next. */
void pmd85debug_run_frames(pmd85debug *d, int n);

uint64_t pmd85debug_stop_serial(const pmd85debug *d);

/* ---- why it stopped: one locked snapshot -------------------------------- */
typedef struct {
    int      paused;
    uint64_t serial;
    int      pc;           /* -1 while running */
    int      reason_addr;  /* the address the reason names, -1 while running */
    char     reason[160];  /* "stopped at 8C00h", "breakpoint: exec 8C00h",
                            * "wrote C000h = 41h (was 00h) at 8C12h",
                            * "OUT 3Fh (BECKER data) = E2h at 7012h" */
} pmd85debug_stop;
void pmd85debug_stop_snapshot(pmd85debug *d, pmd85debug_stop *out);
int  pmd85debug_stop_reason(pmd85debug *d, char *dst, int dstsz, int *address);
/* Bumped whenever a command ran or the machine stopped or resumed. */
unsigned pmd85debug_generation(pmd85debug *d);

/* ---- registers: valid only while paused ---------------------------------- */
typedef struct {
    uint16_t pc, sp;
    uint8_t  a, f, b, c, d, e, h, l;
    int      iff;    /* interrupts enabled (EI) */
    int      halt;   /* in HLT */
} pmd85debug_regs;

/* F bits: S Z 0 AC 0 P 1 CY */
enum {
    PMD85DEBUG_F_CY = 0x01, PMD85DEBUG_F_P = 0x04, PMD85DEBUG_F_AC = 0x10,
    PMD85DEBUG_F_Z = 0x40, PMD85DEBUG_F_S = 0x80
};

typedef enum {
    PMD85DEBUG_REG_PC = 0, PMD85DEBUG_REG_SP, PMD85DEBUG_REG_A, PMD85DEBUG_REG_F,
    PMD85DEBUG_REG_B, PMD85DEBUG_REG_C, PMD85DEBUG_REG_D, PMD85DEBUG_REG_E,
    PMD85DEBUG_REG_H, PMD85DEBUG_REG_L, PMD85DEBUG_REG_BC, PMD85DEBUG_REG_DE,
    PMD85DEBUG_REG_HL, PMD85DEBUG_REG_PSW,   /* PSW = A:F */
    PMD85DEBUG_REG_COUNT
} pmd85debug_reg;

int pmd85debug_regs_get(pmd85debug *d, pmd85debug_regs *out);
/* Returns 1, 0 while running. A PC edit takes effect at the next fetch. */
int pmd85debug_regs_set(pmd85debug *d, int reg, unsigned value);
int pmd85debug_flag_set(pmd85debug *d, unsigned mask, int on);

/* ---- memory: side effects none, valid while paused ----------------------- */
uint32_t pmd85debug_space_size(pmd85debug *d, int space);   /* 0x10000 */
int pmd85debug_mem_read(pmd85debug *d, int space, uint32_t addr, uint8_t *dst, int n);
/* RAM and VRAM only (ROM is skipped). Returns the bytes stored. */
int pmd85debug_mem_write(pmd85debug *d, int space, uint32_t addr, const uint8_t *src, int n);
int pmd85debug_read(pmd85debug *d, uint16_t addr, uint8_t *dst, int n);

enum { PMD85DEBUG_REGION_UNMAPPED = 0, PMD85DEBUG_REGION_RAM, PMD85DEBUG_REGION_ROM,
       PMD85DEBUG_REGION_VRAM, PMD85DEBUG_REGION_WRITE_ONLY };
typedef struct {
    uint16_t lo, hi;   /* inclusive */
    int      kind;
    int      writable;
    char     name[32]; /* "RAM", "ROM", "VRAM", "RAM under ROM (write only)" */
} pmd85debug_region;
/* Ordered, gap-free over #0000-#FFFF, as mapped now (the 2A/3 remap after
 * reset and ALLRAM change it). Valid while paused; the last stop's map while
 * running. */
int pmd85debug_memory_map(pmd85debug *d, pmd85debug_region *out, int max);

#define PMD85DEBUG_SEARCH_NOCASE 1
int pmd85debug_search(pmd85debug *d, int space, const uint8_t *pat, int patlen,
                      uint8_t mask, int flags, uint32_t from, uint32_t to,
                      uint32_t *out, int max);
int pmd85debug_search_next(pmd85debug *d, int space, const uint8_t *pat, int patlen,
                           uint8_t mask, int flags, uint32_t from, int backward,
                           uint32_t *found);

/* ---- how it reads ------------------------------------------------------
 * Intel 8080 mnemonics (the default) or the Z80 spelling of the same; and
 * hex as C000h (the default), $C000, 0xC000 or #C000 -- in the disassembly,
 * stop reasons and the prompt's output. Both persist in the session's
 * settings (debugger_syntax, debugger_hex). Input takes every style. */
void pmd85debug_set_syntax(pmd85debug *d, i8080_syntax syntax);
i8080_syntax pmd85debug_syntax(pmd85debug *d);
void pmd85debug_set_hex_style(pmd85debug *d, i8080_hex style);
i8080_hex pmd85debug_hex_style(pmd85debug *d);
/* A number in that style, for a window's own fields. Returns the length. */
int  pmd85debug_format_hex(pmd85debug *d, unsigned value, int digits, char *dst, int dstsz);

typedef struct {
    uint16_t address;
    int  is_pc;
    int  has_breakpoint;   /* an enabled execute breakpoint covers it */
    int  bp_disabled;      /* only a disabled one does */
    int  length;
    char bytes[16];        /* "C3 00 8C" */
    char label[48];        /* the symbol at this address */
    char disasm[32];       /* "JMP  8C00h" */
    char comment[48];      /* a target's symbol, or the port's name */
    unsigned flags;        /* I8080_* */
    uint16_t target;       /* with I8080_BRANCH */
    int  port;             /* with I8080_IN/OUT, else -1 */
} pmd85debug_line;
/* Up to max lines from addr; 0 while running. *pc_line: PC's index or -1. */
int pmd85debug_disassemble_ex(pmd85debug *d, uint16_t addr, pmd85debug_line *out,
                              int max, int *pc_line);
/* The address `rows` instructions before (negative) or after addr. Backwards
 * is a best guess. 0 while running. */
int pmd85debug_row_address(pmd85debug *d, uint16_t addr, int rows);

/* ---- breakpoints ------------------------------------------------------- */
enum {
    PMD85DEBUG_BP_EXEC = 1, PMD85DEBUG_BP_READ = 2, PMD85DEBUG_BP_WRITE = 4,
    PMD85DEBUG_BP_ACCESS = 6,           /* READ | WRITE */
    PMD85DEBUG_BP_IN = 8, PMD85DEBUG_BP_OUT = 16,
    PMD85DEBUG_BP_IO = 24               /* IN | OUT: start/end are ports */
};
typedef struct {
    int      id;
    int      type;            /* PMD85DEBUG_BP_* bits */
    uint32_t start, end;      /* inclusive: addresses, or ports for IN/OUT */
    int      enabled;
    unsigned hits;
    char     condition[128];
} pmd85debug_breakpoint;
/* A breakpoint is either memory (EXEC/READ/WRITE) or I/O (IN/OUT), not both.
 * Returns the new id, or -1 (full, bad type/range, or the condition does
 * not parse). Safe while running. In a condition, `value`, `addr` (or
 * `port`) and `old` describe the access. */
int  pmd85debug_breakpoint_add(pmd85debug *d, int type, uint32_t start, uint32_t end,
                               const char *condition);
void pmd85debug_breakpoint_remove(pmd85debug *d, int id);
void pmd85debug_breakpoint_enable(pmd85debug *d, int id, int enabled);
int  pmd85debug_breakpoints_list(pmd85debug *d, pmd85debug_breakpoint *out, int max);
void pmd85debug_breakpoint_clear_hits(pmd85debug *d);
/* The execute-breakpoint shortcuts a disassembly view uses. */
int  pmd85debug_breakpoint_toggle(pmd85debug *d, uint16_t addr);
int  pmd85debug_breakpoint_is_set(pmd85debug *d, uint16_t addr);
void pmd85debug_breakpoint_clear_all(pmd85debug *d);
int  pmd85debug_breakpoint_list(pmd85debug *d, uint16_t *out, int max);

/* ---- symbols ----------------------------------------------------------- */
/* Loads "NAME EQU #ADDR"/"NAME EQU $ADDR", z88dk .map ("NAME = $ADDR ;...")
 * or "NAME ADDR" lines on top of what is there. Returns the count, -1 if
 * the file could not be read. */
int  pmd85debug_symbols_load(pmd85debug *d, const char *path);
void pmd85debug_symbols_clear(pmd85debug *d);
int  pmd85debug_symbol_at(pmd85debug *d, uint16_t addr, char *dst, int dstsz);
int  pmd85debug_symbol_find(pmd85debug *d, const char *name, uint16_t *addr_out);
int  pmd85debug_symbol_add(pmd85debug *d, uint16_t addr, const char *name);
int  pmd85debug_symbols_count(pmd85debug *d);
int  pmd85debug_symbols_get(pmd85debug *d, int index, uint16_t *addr, char *name,
                            int namesz);
int  pmd85debug_symbols_filter(pmd85debug *d, const char *text, int *out, int max);

/* ---- the instruction trace (a 65536-entry ring, while engaged) ----------
 * The instructions that ran, each with the registers it started with; the
 * one the machine is stopped before is recorded when it runs. */
typedef struct {
    uint16_t pc;
    uint8_t  len;
    uint8_t  bytes[3];
    uint8_t  a, f, b, c, d, e, h, l;
    uint16_t sp;
    uint64_t tstates;          /* CPU T-states since the machine was built */
    uint32_t frame;
    int32_t  in_frame;         /* T-states into that frame */
} pmd85debug_trace_entry;
void pmd85debug_trace_enable(pmd85debug *d, int on);
int  pmd85debug_trace_enabled(pmd85debug *d);
int  pmd85debug_trace_read(pmd85debug *d, pmd85debug_trace_entry *out, int max); /* newest first */
void pmd85debug_trace_clear(pmd85debug *d);
int  pmd85debug_trace_count(pmd85debug *d);
/* "8C00  JMP  8C03h     A=00 F=02 BC=0000 DE=0000 HL=0000 SP=7FFF" */
int  pmd85debug_trace_format(pmd85debug *d, const pmd85debug_trace_entry *e, char *dst,
                             int dstsz);
enum { PMD85DEBUG_TRACE_ALL = 0, PMD85DEBUG_TRACE_BRANCHES, PMD85DEBUG_TRACE_RANGE,
       PMD85DEBUG_TRACE_IO };
int  pmd85debug_trace_match(const pmd85debug_trace_entry *e, int filter, uint16_t lo,
                            uint16_t hi);

/* ---- the I/O access log -------------------------------------------------
 * Every IN and OUT while engaged and the log is on (it is on by default),
 * newest first, a ring of 4096. */
typedef struct {
    uint64_t tstates;
    uint16_t pc;
    uint8_t  port;
    uint8_t  value;
    uint8_t  out;       /* 1 OUT, 0 IN */
} pmd85debug_io_access;
void pmd85debug_io_log_enable(pmd85debug *d, int on);
int  pmd85debug_io_log_enabled(pmd85debug *d);
int  pmd85debug_io_log(pmd85debug *d, pmd85debug_io_access *out, int max);
void pmd85debug_io_log_clear(pmd85debug *d);
/* "SYS PIO PC", "BECKER data", ... or NULL when nothing answers that port
 * on the machine as configured. Valid while paused. */
const char *pmd85debug_port_name(pmd85debug *d, uint8_t port);

/* ---- the chips: valid while paused (0 and zeroed while running) --------- */
typedef struct {
    int      valid;
    uint8_t  cwr, a, b, c;       /* control word, output latches */
    uint8_t  in_a, in_b, in_c;   /* input buffers */
    uint8_t  inte;               /* bit0 INTE A in, bit1 A out, bit2 B */
} pmd85debug_pio;
typedef struct {
    int      valid;
    uint8_t  cwr;
    uint16_t init, value;
    int      gate, out, counting;
} pmd85debug_pit_counter;
typedef struct {
    int model;                   /* PMD85SESSION_MODEL_* */
    pmd85debug_pio sys;          /* system 8255: keyboard, LEDs, sound */
    pmd85debug_pio gpio;         /* GPIO 8255: joystick, PMD 32 */
    pmd85debug_pio rom;          /* ROM module 8255 */
    pmd85debug_pit_counter pit[3];  /* the timer interface 8253 */
    int      usart_valid;        /* the tape 8251 */
    uint8_t  usart_mode, usart_command, usart_status;
    int      mm256;              /* -1, or the 256K mapper's page register */
    int      mega_page;          /* -1, or the MegaModule's page */
    int      allram, remapped, in_reset;
    int      led;                /* the system PIO's LED bits */
    int      becker;             /* the Becker port is fitted */
    int      becker_link, becker_rx;
    int      mif85, pmd32;
} pmd85debug_io;
int pmd85debug_io_get(pmd85debug *d, pmd85debug_io *out);

/* ---- the prompt ---------------------------------------------------------- */
int pmd85debug_command(pmd85debug *d, const char *command, char *dst, int dstsz);
int pmd85debug_completions(pmd85debug *d, const char *prefix, char *dst, int dstsz);
/* Registers a..l, bc de hl psw sp pc, flags sf zf hf (aux carry) pf cf,
 * [addr] byte, {addr} word (little-endian), symbols, cycles frame tframe,
 * and in a condition value addr port old. 0 and *out, or -1 with a message. */
int pmd85debug_eval(pmd85debug *d, const char *text, long *out, char *err, int errsz);

/* ---- FujiNet: the DriveWire traffic on the Becker port ----------------
 * Recorded from session start whether or not the debugger is engaged. Safe
 * from any thread. */
typedef struct {
    uint64_t serial;
    int      to_host;  /* 1: FujiNet -> PMD with no request before it */
    int      device;   /* the DriveWire opcode (E2 FUJI, D2 READEX...), -1 */
    int      command;  /* the FUJI/NET/... command byte, else -1 */
    int      length;   /* bytes FujiNet sent back, -1 for none */
    int      nbytes;
    uint8_t  data[64];
    char     text[96];
    uint32_t repeat;
} pmd85_fuji_packet;
int  pmd85debug_fujinet_trace(pmd85debug *d, pmd85_fuji_packet *out, int max);
void pmd85debug_fujinet_trace_clear(pmd85debug *d);
int  pmd85debug_fujinet_is_ready(const pmd85_fuji_packet *p);
int  pmd85debug_fujinet_state(pmd85debug *d, char *dst, int dstsz);

#ifdef __cplusplus
}
#endif

#endif /* PMD85DEBUG_H */
