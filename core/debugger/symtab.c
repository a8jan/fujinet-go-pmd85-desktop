/*
 * pmd85symtab -- name<->address table for the debugger's disassembly view.
 *
 * Two sources merged into one flat table: a built-in list, and whatever a
 * user loads on top from their own assembled program.
 *
 * The built-in list is empty for now on purpose. The PMD 85 monitors'
 * entry points differ between the models (the 2A's ROM-module transfer at
 * 8C00h is not the 1's), GPMD85Emulator ships no names, and a wrong address
 * in a debugger is worse than a missing one; per-model tables can come from
 * a ROM disassembly when one is in hand.
 *
 * User symbols load as "NAME EQU #ADDR" / "NAME EQU $ADDR" (assembler
 * listings and symbol dumps), z88dk's .map ("NAME = $ADDR ; ..."), or a
 * plain "NAME ADDR". Numbers: #/$/0x prefix or an h suffix for hex, else
 * decimal.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "symtab.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYM_NAME_MAX 32

typedef struct {
    uint16_t addr;
    char name[SYM_NAME_MAX];
} sym_entry;

struct pmd85symtab {
    sym_entry *entries;
    int count;
    int cap;
    int builtin_count; /* entries[0..builtin_count) survive clear_user */
};

/* none yet (see the top of the file); the sentinel keeps the array legal */
static const struct { uint16_t addr; const char *name; } k_builtin[] = {
    { 0, NULL },
};

static int ensure_cap(pmd85symtab *t, int need)
{
    if (need <= t->cap)
        return 1;
    int newcap = t->cap ? t->cap * 2 : 16;
    sym_entry *e;
    while (newcap < need) newcap *= 2;
    e = realloc(t->entries, (size_t)newcap * sizeof(*e));
    if (!e) return 0;
    t->entries = e;
    t->cap = newcap;
    return 1;
}

static int append(pmd85symtab *t, uint16_t addr, const char *name)
{
    if (!ensure_cap(t, t->count + 1))
        return 0;
    t->entries[t->count].addr = addr;
    snprintf(t->entries[t->count].name, SYM_NAME_MAX, "%s", name);
    t->count++;
    return 1;
}

pmd85symtab *pmd85symtab_create(void)
{
    pmd85symtab *t = calloc(1, sizeof(*t));
    size_t i;
    if (!t) return NULL;
    for (i = 0; i < sizeof(k_builtin) / sizeof(k_builtin[0]); i++)
        if (k_builtin[i].name)
            append(t, k_builtin[i].addr, k_builtin[i].name);
    t->builtin_count = t->count;
    return t;
}

void pmd85symtab_destroy(pmd85symtab *t)
{
    if (!t) return;
    free(t->entries);
    free(t);
}

void pmd85symtab_clear_user(pmd85symtab *t)
{
    if (!t) return;
    t->count = t->builtin_count;
}

/* Parses one line into (name, addr). Accepts "NAME EQU $ADDR" (lwasm's
 * --symbol-dump) and "NAME ADDR" (hex with $/0x, or decimal). Returns 1 on
 * a successful parse. */
static int parse_line(const char *line, char *name_out, uint16_t *addr_out)
{
    char name[SYM_NAME_MAX];
    char rest[128];
    const char *p = line;
    int n;

    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == ';' || *p == '\n' || *p == '\r')
        return 0;

    if (sscanf(p, "%31s%n", name, &n) != 1)
        return 0;
    p += n;
    while (*p == ' ' || *p == '\t') p++;

    /* Optional "EQU" keyword between the name and the address. */
    if ((p[0] == 'E' || p[0] == 'e') && (p[1] == 'Q' || p[1] == 'q') &&
        (p[2] == 'U' || p[2] == 'u') && (p[3] == '\0' || isspace((unsigned char)p[3]))) {
        p += 3;
        while (*p == ' ' || *p == '\t') p++;
    } else if (*p == '=') {   /* z88dk .map */
        p++;
        while (*p == ' ' || *p == '\t') p++;
    }

    if (sscanf(p, "%127s", rest) != 1)
        return 0;

    {
        const char *num = rest;
        int base = 10;
        char *end;
        unsigned long v;

        size_t len = strlen(rest);

        if (num[0] == '$' || num[0] == '#') { num++; base = 16; }
        else if (num[0] == '0' && (num[1] == 'x' || num[1] == 'X')) { num += 2; base = 16; }
        else if (len > 1 && (rest[len - 1] == 'h' || rest[len - 1] == 'H')) {
            rest[len - 1] = '\0';
            base = 16;
        }

        v = strtoul(num, &end, base);
        if (end == num || *end != '\0')
            return 0;
        if (v > 0xFFFF)
            return 0;
        *addr_out = (uint16_t)v;
    }
    snprintf(name_out, SYM_NAME_MAX, "%s", name);
    return 1;
}

int pmd85symtab_add(pmd85symtab *t, uint16_t addr, const char *name)
{
    if (!t || !name)
        return 0;
    return append(t, addr, name);
}

int pmd85symtab_load(pmd85symtab *t, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    int loaded = 0;

    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        char name[SYM_NAME_MAX];
        uint16_t addr;
        if (parse_line(line, name, &addr) && append(t, addr, name))
            loaded++;
    }
    fclose(f);
    return loaded;
}

int pmd85symtab_lookup_addr(const pmd85symtab *t, uint16_t addr, char *dst,
                           int dstsz)
{
    int i;
    if (!t) return 0;
    for (i = t->count - 1; i >= 0; i--) {
        if (t->entries[i].addr == addr) {
            snprintf(dst, (size_t)dstsz, "%s", t->entries[i].name);
            return 1;
        }
    }
    return 0;
}

int pmd85symtab_lookup_name(const pmd85symtab *t, const char *name,
                           uint16_t *addr_out)
{
    int i;
    if (!t) return 0;
    for (i = t->count - 1; i >= 0; i--) {
        if (strcmp(t->entries[i].name, name) == 0) {
            *addr_out = t->entries[i].addr;
            return 1;
        }
    }
    return 0;
}

int pmd85symtab_count(const pmd85symtab *t)
{
    return t ? t->count : 0;
}

int pmd85symtab_get(const pmd85symtab *t, int index, uint16_t *addr, char *name,
                   int namesz, int *builtin)
{
    if (!t || index < 0 || index >= t->count)
        return 0;
    if (addr)
        *addr = t->entries[index].addr;
    if (name && namesz > 0)
        snprintf(name, (size_t)namesz, "%s", t->entries[index].name);
    if (builtin)
        *builtin = index < t->builtin_count;
    return 1;
}
