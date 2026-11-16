/*
 * dasm_test: every opcode, in both spellings, against GPMD85Emulator's own
 * mnemonic tables (src/Debugger.cpp of the pinned checkout, parsed here),
 * plus the lengths, flags and branch targets the debugger relies on.
 *
 * argv[1] is the GPMD85Emulator source directory. Exit 77 (skip) without it.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i8080dasm.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); \
    fputc('\n', stderr); failures++; } } while (0)

/* Collect the 256 string literals after `name` in the C source. */
static int load_table(const char *src, const char *name, char out[256][24])
{
    const char *p = strstr(src, name);
    int n = 0;
    if (!p) return 0;
    p = strchr(p, '{');
    while (p && n < 256) {
        const char *q = strchr(p, '"'), *e;
        if (!q) break;
        e = strchr(q + 1, '"');
        if (!e || e - q - 1 >= 24) break;
        memcpy(out[n], q + 1, (size_t)(e - q - 1));
        out[n][e - q - 1] = '\0';
        n++;
        p = e + 1;
    }
    return n;
}

/* Upstream's template with this test's operand bytes (34h 12h) filled in. */
static void expand(const char *tpl, uint8_t op, char *dst, size_t sz, int *undoc)
{
    size_t j = 0;
    *undoc = tpl[0] == '!';
    for (tpl++; *tpl && j + 6 < sz; tpl++) {
        switch (*tpl) {
        case '%': j += (size_t)snprintf(dst + j, sz - j, "#%02X", 0x34); break;
        case '@': j += (size_t)snprintf(dst + j, sz - j, "#%02X", op & 0x38); break;
        case '&': case '*': j += (size_t)snprintf(dst + j, sz - j, "#%04X", 0x1234); break;
        default: dst[j++] = *tpl; break;
        }
    }
    dst[j] = '\0';
}

static uint8_t mem[4];
static uint8_t rd(uint16_t a, void *u) { (void)u; return mem[a & 3]; }

int main(int argc, char **argv)
{
    static char t8080[256][24], tz80[256][24];
    char path[1024], *src;
    FILE *f;
    long size;
    i8080_line l;

    if (argc < 2) return 77;
    snprintf(path, sizeof path, "%s/src/Debugger.cpp", argv[1]);
    f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "no %s\n", path); return 77; }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    src = calloc(1, (size_t)size + 1);
    if (fread(src, 1, (size_t)size, f) != (size_t)size) return 77;
    fclose(f);
    CHECK(load_table(src, "instr8080[256][11] = {", t8080) == 256, "8080 table");
    CHECK(load_table(src, "instrZ80[256][14] = {", tz80) == 256, "Z80 table");
    if (failures) return 1;

    for (int op = 0; op < 256; op++) {
        char want[40];
        int undoc;
        mem[0] = (uint8_t)op; mem[1] = 0x34; mem[2] = 0x12;
        for (int syn = 0; syn < 2; syn++) {
            i8080_dasm_one(0, rd, NULL, syn, I8080_HEX_HASH, &l);
            expand(syn ? tz80[op] : t8080[op], (uint8_t)op, want, sizeof want, &undoc);
            /* upstream spells the 8080's RST with a digit and the Z80's
             * with the vector; both agree with ours */
            CHECK(!strcmp(l.text, want), "%02X %s: got '%s', upstream '%s'", op,
                  syn ? "z80" : "8080", l.text, want);
            CHECK(!!(l.flags & I8080_UNDOC) == undoc, "%02X undocumented flag", op);
        }
        /* length: what the operands in upstream's template imply */
        {
            int want_len = strchr(t8080[op], '&') || strchr(t8080[op], '*') ? 3
                         : strchr(t8080[op], '%') ? 2 : 1;
            CHECK(i8080_length((uint8_t)op) == want_len, "%02X length %d, want %d", op,
                  i8080_length((uint8_t)op), want_len);
        }
    }

    /* flow details the debugger's stepping uses */
    mem[0] = 0xCD; i8080_dasm_one(0, rd, NULL, 0, I8080_HEX_HASH, &l);
    CHECK((l.flags & I8080_CALL) && (l.flags & I8080_BRANCH) && l.target == 0x1234, "CALL");
    mem[0] = 0xC2; i8080_dasm_one(0, rd, NULL, 0, I8080_HEX_HASH, &l);
    CHECK((l.flags & (I8080_JUMP | I8080_COND)) == (I8080_JUMP | I8080_COND), "JNZ");
    mem[0] = 0xC8; i8080_dasm_one(0, rd, NULL, 0, I8080_HEX_HASH, &l);
    CHECK((l.flags & (I8080_RET | I8080_COND)) == (I8080_RET | I8080_COND), "RZ");
    mem[0] = 0xEF; i8080_dasm_one(0, rd, NULL, 0, I8080_HEX_HASH, &l);
    CHECK((l.flags & I8080_RST) && l.target == 0x28, "RST 5");
    mem[0] = 0xE9; i8080_dasm_one(0, rd, NULL, 0, I8080_HEX_HASH, &l);
    CHECK((l.flags & I8080_INDIRECT) && !(l.flags & I8080_BRANCH), "PCHL");
    mem[0] = 0xD3; i8080_dasm_one(0, rd, NULL, 0, I8080_HEX_HASH, &l);
    CHECK((l.flags & I8080_OUT) && l.port == 0x34, "OUT");
    mem[0] = 0xDB; i8080_dasm_one(0, rd, NULL, 0, I8080_HEX_HASH, &l);
    CHECK((l.flags & I8080_IN) && l.port == 0x34, "IN");
    mem[0] = 0x76; i8080_dasm_one(0, rd, NULL, 0, I8080_HEX_HASH, &l);
    CHECK(l.flags & I8080_HALT, "HLT");

    free(src);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    /* the other hex styles */
    mem[0] = 0x21; mem[1] = 0x00; mem[2] = 0xC0;
    i8080_dasm_one(0, rd, NULL, 0, I8080_HEX_H, &l);
    CHECK(!strcmp(l.text, "LXI  H,C000h"), "h style: %s", l.text);
    i8080_dasm_one(0, rd, NULL, 0, I8080_HEX_DOLLAR, &l);
    CHECK(!strcmp(l.text, "LXI  H,$C000"), "$ style: %s", l.text);
    i8080_dasm_one(0, rd, NULL, 1, I8080_HEX_0X, &l);
    CHECK(!strcmp(l.text, "ld   hl,0xC000"), "0x style: %s", l.text);

    printf("dasm: ok (256 opcodes x 2 syntaxes)\n");
    return 0;
}
