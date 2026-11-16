/*
 * i8080dasm -- Intel 8080 (MHB8080A) disassembler for the PMD 85 debugger.
 *
 * One decoder, two spellings: Intel's own mnemonics (MOV, MVI, LXI, JNZ ...)
 * or the Z80 equivalents for the same opcodes (ld, jp nz ...), as
 * GPMD85Emulator's debugger offers. Hex in any of four styles (i8080_hex).
 * The 12 undocumented opcode aliases (08h.. NOP, CBh JMP, D9h RET, DDh/EDh/
 * FDh CALL) decode as what the MHB8080A executes, flagged I8080_UNDOC.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef I8080DASM_H
#define I8080DASM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What an instruction does to the flow of control, and what it touches. */
#define I8080_JUMP     0x0001  /* JMP/Jcc/PCHL */
#define I8080_CALL     0x0002  /* CALL/Ccc/RST */
#define I8080_RET      0x0004  /* RET/Rcc */
#define I8080_COND     0x0008  /* conditional (Jcc/Ccc/Rcc) */
#define I8080_RST      0x0010
#define I8080_HALT     0x0020
#define I8080_INDIRECT 0x0040  /* PCHL: the target is HL, not in the bytes */
#define I8080_UNDOC    0x0080  /* an undocumented alias */
#define I8080_IN       0x0100  /* IN port */
#define I8080_OUT      0x0200  /* OUT port */
#define I8080_BRANCH   0x0400  /* `target` is meaningful (JUMP/CALL, not PCHL) */

typedef enum { I8080_SYNTAX_INTEL = 0, I8080_SYNTAX_Z80 = 1 } i8080_syntax;

/* How hex numbers are written: C000h (the default), $C000, 0xC000, #C000
 * (GPMD85Emulator's own). */
typedef enum { I8080_HEX_H = 0, I8080_HEX_DOLLAR, I8080_HEX_0X, I8080_HEX_HASH,
               I8080_HEX_COUNT } i8080_hex;

/* v as `digits` hex digits in that style. Returns the length. */
int i8080_hex_format(char *dst, int dstsz, unsigned v, int digits, i8080_hex style);

typedef struct {
    uint16_t addr;
    uint8_t  len;          /* 1..3 */
    uint8_t  bytes[3];
    char     text[32];     /* "JMP  8C00h", "MVI  A,FFh", "ld   a,(C000h)" */
    unsigned flags;        /* I8080_* */
    uint16_t target;       /* with I8080_BRANCH: where it goes */
    int      port;         /* with I8080_IN/OUT: the port, else -1 */
} i8080_line;

typedef uint8_t (*i8080_read_fn)(uint16_t addr, void *user);

/* The length of the instruction that starts with this opcode. */
int i8080_length(uint8_t opcode);

/* Decode one instruction at addr. Returns its length. */
int i8080_dasm_one(uint16_t addr, i8080_read_fn read, void *user,
                   i8080_syntax syntax, i8080_hex hex, i8080_line *out);

#ifdef __cplusplus
}
#endif

#endif /* I8080DASM_H */
