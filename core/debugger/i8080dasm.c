/*
 * i8080dasm -- see core/include/i8080dasm.h.
 *
 * Decoded from the opcode's fields (xx yyy zzz) rather than a 256-entry
 * table, so both spellings come from one decoder and cannot drift apart.
 * core/tests/dasm_test.c checks every opcode against GPMD85Emulator's own
 * mnemonic tables.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "i8080dasm.h"

#include <stdio.h>
#include <string.h>

int i8080_length(uint8_t op)
{
    switch (op) {
    /* MVI r,d8 */
    case 0x06: case 0x0E: case 0x16: case 0x1E:
    case 0x26: case 0x2E: case 0x36: case 0x3E:
    /* immediate ALU, OUT, IN */
    case 0xC6: case 0xCE: case 0xD6: case 0xDE:
    case 0xE6: case 0xEE: case 0xF6: case 0xFE:
    case 0xD3: case 0xDB:
        return 2;
    /* LXI, SHLD/LHLD, STA/LDA */
    case 0x01: case 0x11: case 0x21: case 0x31:
    case 0x22: case 0x2A: case 0x32: case 0x3A:
    /* JMP/Jcc, CALL/Ccc (and the aliases) */
    case 0xC2: case 0xC3: case 0xC4: case 0xCA: case 0xCB: case 0xCC: case 0xCD:
    case 0xD2: case 0xD4: case 0xDA: case 0xDC: case 0xDD:
    case 0xE2: case 0xE4: case 0xEA: case 0xEC: case 0xED:
    case 0xF2: case 0xF4: case 0xFA: case 0xFC: case 0xFD:
        return 3;
    default:
        return 1;
    }
}

/* register / pair / condition / ALU names, Intel then Z80 */
static const char *const k_r[2][8] = {
    { "B", "C", "D", "E", "H", "L", "M", "A" },
    { "b", "c", "d", "e", "h", "l", "(hl)", "a" },
};
static const char *const k_rp[2][4] = {
    { "B", "D", "H", "SP" },
    { "bc", "de", "hl", "sp" },
};
static const char *const k_rp2[2][4] = {
    { "B", "D", "H", "PSW" },
    { "bc", "de", "hl", "af" },
};
static const char *const k_cc[2][8] = {
    { "NZ", "Z", "NC", "C", "PO", "PE", "P", "M" },
    { "nz", "z", "nc", "c", "po", "pe", "p", "m" },
};
static const char *const k_alu[8] = { "ADD", "ADC", "SUB", "SBB", "ANA", "XRA", "ORA", "CMP" };
static const char *const k_alui[8] = { "ADI", "ACI", "SUI", "SBI", "ANI", "XRI", "ORI", "CPI" };
/* Z80: the mnemonic and whether it names the accumulator ("add  a,b") */
static const char *const k_zalu[8] = { "add  a,", "adc  a,", "sub  ", "sbc  a,",
                                       "and  ", "xor  ", "or   ", "cp   " };

/* Mnemonic padded to 5 columns, then the operands. */
static void emit(char *dst, size_t sz, const char *mn, const char *ops)
{
    if (ops && *ops)
        snprintf(dst, sz, "%-5s%s", mn, ops);
    else
        snprintf(dst, sz, "%s", mn);
}

int i8080_hex_format(char *dst, int dstsz, unsigned v, int digits, i8080_hex style)
{
    static const char *const fmt[I8080_HEX_COUNT] = { "%0*Xh", "$%0*X", "0x%0*X", "#%0*X" };
    int n;
    if (!dst || dstsz <= 0)
        return 0;
    n = snprintf(dst, (size_t)dstsz, fmt[(unsigned)style < I8080_HEX_COUNT ? style : 0],
                 digits, v);
    return n < dstsz ? n : dstsz - 1;
}

int i8080_dasm_one(uint16_t addr, i8080_read_fn read, void *user,
                   i8080_syntax syntax, i8080_hex hex, i8080_line *o)
{
    const int z = syntax == I8080_SYNTAX_Z80 ? 1 : 0;
    uint8_t op;
    int len, x, y, zz, p, q;
    char ops[24], b8[12], w16[12];
    uint16_t word;
    char *t;

    memset(o, 0, sizeof *o);
    o->addr = addr;
    o->port = -1;
    op = read(addr, user);
    len = i8080_length(op);
    o->len = (uint8_t)len;
    o->bytes[0] = op;
    if (len > 1) o->bytes[1] = read((uint16_t)(addr + 1), user);
    if (len > 2) o->bytes[2] = read((uint16_t)(addr + 2), user);
    word = (uint16_t)(o->bytes[1] | (o->bytes[2] << 8));
    i8080_hex_format(b8, sizeof b8, o->bytes[1], 2, hex);
    i8080_hex_format(w16, sizeof w16, word, 4, hex);

    x = op >> 6;
    y = (op >> 3) & 7;
    zz = op & 7;
    p = y >> 1;
    q = y & 1;
    t = o->text;
    ops[0] = '\0';

    if (x == 1) {
        if (op == 0x76) {
            emit(t, sizeof o->text, z ? "halt" : "HLT", NULL);
            o->flags |= I8080_HALT;
        } else {
            snprintf(ops, sizeof ops, "%s,%s", k_r[z][y], k_r[z][zz]);
            emit(t, sizeof o->text, z ? "ld" : "MOV", ops);
        }
        return len;
    }
    if (x == 2) {
        if (z) {
            snprintf(t, sizeof o->text, "%s%s", k_zalu[y], k_r[1][zz]);
        } else {
            emit(t, sizeof o->text, k_alu[y], k_r[0][zz]);
        }
        return len;
    }

    if (x == 0) {
        switch (zz) {
        case 0:
            emit(t, sizeof o->text, z ? "nop" : "NOP", NULL);
            if (y)
                o->flags |= I8080_UNDOC;
            break;
        case 1:
            if (q == 0) {
                snprintf(ops, sizeof ops, "%s,%s", k_rp[z][p], w16);
                emit(t, sizeof o->text, z ? "ld" : "LXI", ops);
            } else {
                snprintf(ops, sizeof ops, z ? "hl,%s" : "%s", k_rp[z][p]);
                emit(t, sizeof o->text, z ? "add" : "DAD", ops);
            }
            break;
        case 2:
            switch (y) {
            case 0: case 2:   /* STAX B/D */
                if (z) snprintf(ops, sizeof ops, "(%s),a", k_rp[1][p]);
                else snprintf(ops, sizeof ops, "%s", k_rp[0][p]);
                emit(t, sizeof o->text, z ? "ld" : "STAX", ops);
                break;
            case 1: case 3:   /* LDAX B/D */
                if (z) snprintf(ops, sizeof ops, "a,(%s)", k_rp[1][p]);
                else snprintf(ops, sizeof ops, "%s", k_rp[0][p]);
                emit(t, sizeof o->text, z ? "ld" : "LDAX", ops);
                break;
            case 4:
                if (z) snprintf(ops, sizeof ops, "(%s),hl", w16);
                emit(t, sizeof o->text, z ? "ld" : "SHLD", z ? ops : w16);
                break;
            case 5:
                if (z) snprintf(ops, sizeof ops, "hl,(%s)", w16);
                emit(t, sizeof o->text, z ? "ld" : "LHLD", z ? ops : w16);
                break;
            case 6:
                if (z) snprintf(ops, sizeof ops, "(%s),a", w16);
                emit(t, sizeof o->text, z ? "ld" : "STA", z ? ops : w16);
                break;
            default:
                if (z) snprintf(ops, sizeof ops, "a,(%s)", w16);
                emit(t, sizeof o->text, z ? "ld" : "LDA", z ? ops : w16);
                break;
            }
            break;
        case 3:
            emit(t, sizeof o->text, z ? (q ? "dec" : "inc") : (q ? "DCX" : "INX"), k_rp[z][p]);
            break;
        case 4:
            emit(t, sizeof o->text, z ? "inc" : "INR", k_r[z][y]);
            break;
        case 5:
            emit(t, sizeof o->text, z ? "dec" : "DCR", k_r[z][y]);
            break;
        case 6:
            snprintf(ops, sizeof ops, "%s,%s", k_r[z][y], b8);
            emit(t, sizeof o->text, z ? "ld" : "MVI", ops);
            break;
        default: {
            static const char *const intel[8] = { "RLC", "RRC", "RAL", "RAR",
                                                  "DAA", "CMA", "STC", "CMC" };
            static const char *const zilog[8] = { "rlca", "rrca", "rla", "rra",
                                                  "daa", "cpl", "scf", "ccf" };
            emit(t, sizeof o->text, z ? zilog[y] : intel[y], NULL);
            break;
        }
        }
        return len;
    }

    /* x == 3 */
    switch (zz) {
    case 0:   /* Rcc */
        if (z) emit(t, sizeof o->text, "ret", k_cc[1][y]);
        else snprintf(t, sizeof o->text, "R%s", k_cc[0][y]);
        o->flags |= I8080_RET | I8080_COND;
        break;
    case 1:
        if (q == 0) {
            emit(t, sizeof o->text, z ? "pop" : "POP", k_rp2[z][p]);
        } else {
            switch (p) {
            case 0:
                emit(t, sizeof o->text, z ? "ret" : "RET", NULL);
                o->flags |= I8080_RET;
                break;
            case 1:
                emit(t, sizeof o->text, z ? "ret" : "RET", NULL);
                o->flags |= I8080_RET | I8080_UNDOC;
                break;
            case 2:
                emit(t, sizeof o->text, z ? "jp" : "PCHL", z ? "(hl)" : NULL);
                o->flags |= I8080_JUMP | I8080_INDIRECT;
                break;
            default:
                emit(t, sizeof o->text, z ? "ld" : "SPHL", z ? "sp,hl" : NULL);
                break;
            }
        }
        break;
    case 2:   /* Jcc */
        if (z) {
            snprintf(ops, sizeof ops, "%s,%s", k_cc[1][y], w16);
            emit(t, sizeof o->text, "jp", ops);
        } else {
            char mn[8];
            snprintf(mn, sizeof mn, "J%s", k_cc[0][y]);
            emit(t, sizeof o->text, mn, w16);
        }
        o->flags |= I8080_JUMP | I8080_COND | I8080_BRANCH;
        o->target = word;
        break;
    case 3:
        switch (y) {
        case 0: case 1:
            emit(t, sizeof o->text, z ? "jp" : "JMP", w16);
            o->flags |= I8080_JUMP | I8080_BRANCH | (y ? I8080_UNDOC : 0);
            o->target = word;
            break;
        case 2:
            if (z) snprintf(ops, sizeof ops, "(%s),a", b8);
            emit(t, sizeof o->text, z ? "out" : "OUT", z ? ops : b8);
            o->flags |= I8080_OUT;
            o->port = o->bytes[1];
            break;
        case 3:
            if (z) snprintf(ops, sizeof ops, "a,(%s)", b8);
            emit(t, sizeof o->text, z ? "in" : "IN", z ? ops : b8);
            o->flags |= I8080_IN;
            o->port = o->bytes[1];
            break;
        case 4:
            emit(t, sizeof o->text, z ? "ex" : "XTHL", z ? "(sp),hl" : NULL);
            break;
        case 5:
            emit(t, sizeof o->text, z ? "ex" : "XCHG", z ? "de,hl" : NULL);
            break;
        case 6:
            emit(t, sizeof o->text, z ? "di" : "DI", NULL);
            break;
        default:
            emit(t, sizeof o->text, z ? "ei" : "EI", NULL);
            break;
        }
        break;
    case 4:   /* Ccc */
        if (z) {
            snprintf(ops, sizeof ops, "%s,%s", k_cc[1][y], w16);
            emit(t, sizeof o->text, "call", ops);
        } else {
            char mn[8];
            snprintf(mn, sizeof mn, "C%s", k_cc[0][y]);
            emit(t, sizeof o->text, mn, w16);
        }
        o->flags |= I8080_CALL | I8080_COND | I8080_BRANCH;
        o->target = word;
        break;
    case 5:
        if (q == 0) {
            emit(t, sizeof o->text, z ? "push" : "PUSH", k_rp2[z][p]);
        } else {
            /* CDh CALL; DDh, EDh, FDh are its undocumented aliases */
            emit(t, sizeof o->text, z ? "call" : "CALL", w16);
            o->flags |= I8080_CALL | I8080_BRANCH | (p ? I8080_UNDOC : 0);
            o->target = word;
        }
        break;
    case 6:
        if (z) snprintf(t, sizeof o->text, "%s%s", k_zalu[y], b8);
        else emit(t, sizeof o->text, k_alui[y], b8);
        break;
    default: {   /* RST n */
        char n[12];
        if (z) i8080_hex_format(n, sizeof n, (unsigned)(y * 8), 2, hex);
        else snprintf(n, sizeof n, "%d", y);
        emit(t, sizeof o->text, z ? "rst" : "RST", n);
        o->flags |= I8080_CALL | I8080_RST | I8080_BRANCH;
        o->target = (uint16_t)(y * 8);
        break;
    }
    }
    return len;
}
