/*
 * pmd85symtab -- name<->address table for the debugger's disassembly view.
 *
 * Two sources merged into one flat table: a built-in list, and whatever a
 * user loads on top from their own assembled program.
 *
 * The built-in part is the running monitor's symbols, swapped in by the
 * engine (pmd85symtab_set_builtin) from core/debugger/monitor_symbols.c.
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
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYM_NAME_MAX 32

typedef struct {
    uint16_t addr;
    char name[SYM_NAME_MAX];
} sym_entry;

struct pmd85symtab {
    /* the monitor's table is swapped in on the emulator thread when the
     * machine stops, while a window may be reading: one lock for all */
    pthread_mutex_t mtx;
    sym_entry *entries;
    int count;
    int cap;
    int builtin_count; /* entries[0..builtin_count) survive clear_user */
};

static void lock(const pmd85symtab *t) { pthread_mutex_lock(&((pmd85symtab *)t)->mtx); }
static void unlock(const pmd85symtab *t) { pthread_mutex_unlock(&((pmd85symtab *)t)->mtx); }

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
    if (!t) return NULL;
    pthread_mutex_init(&t->mtx, NULL);
    return t;
}

void pmd85symtab_destroy(pmd85symtab *t)
{
    if (!t) return;
    pthread_mutex_destroy(&t->mtx);
    free(t->entries);
    free(t);
}

void pmd85symtab_clear_user(pmd85symtab *t)
{
    if (!t) return;
    lock(t);
    t->count = t->builtin_count;
    unlock(t);
}

int pmd85symtab_set_builtin(pmd85symtab *t, const uint16_t *addrs,
                            const char *const *names, int n)
{
    sym_entry *user;
    int nuser, i;

    if (!t) return 0;
    lock(t);
    nuser = t->count - t->builtin_count;
    user = nuser > 0 ? malloc((size_t)nuser * sizeof *user) : NULL;
    if (nuser > 0 && !user) {
        unlock(t);
        return 0;
    }
    if (nuser > 0)
        memcpy(user, t->entries + t->builtin_count, (size_t)nuser * sizeof *user);
    t->count = 0;
    for (i = 0; i < n; i++)
        append(t, addrs[i], names[i]);
    t->builtin_count = t->count;
    for (i = 0; i < nuser; i++)
        append(t, user[i].addr, user[i].name);
    free(user);
    unlock(t);
    return 1;
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
    int ok;
    if (!t || !name)
        return 0;
    lock(t);
    ok = append(t, addr, name);
    unlock(t);
    return ok;
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
        if (parse_line(line, name, &addr)) {
            lock(t);
            loaded += append(t, addr, name);
            unlock(t);
        }
    }
    fclose(f);
    return loaded;
}

int pmd85symtab_lookup_addr(const pmd85symtab *t, uint16_t addr, char *dst,
                           int dstsz)
{
    int i;
    if (!t) return 0;
    lock(t);
    for (i = t->count - 1; i >= 0; i--) {
        if (t->entries[i].addr == addr) {
            snprintf(dst, (size_t)dstsz, "%s", t->entries[i].name);
            unlock(t);
            return 1;
        }
    }
    unlock(t);
    return 0;
}

int pmd85symtab_lookup_name(const pmd85symtab *t, const char *name,
                           uint16_t *addr_out)
{
    int i;
    if (!t) return 0;
    lock(t);
    for (i = t->count - 1; i >= 0; i--) {
        if (strcmp(t->entries[i].name, name) == 0) {
            *addr_out = t->entries[i].addr;
            unlock(t);
            return 1;
        }
    }
    unlock(t);
    return 0;
}

int pmd85symtab_count(const pmd85symtab *t)
{
    int n;
    if (!t) return 0;
    lock(t);
    n = t->count;
    unlock(t);
    return n;
}

int pmd85symtab_get(const pmd85symtab *t, int index, uint16_t *addr, char *name,
                   int namesz, int *builtin)
{
    if (!t || index < 0)
        return 0;
    lock(t);
    if (index >= t->count) {
        unlock(t);
        return 0;
    }
    if (addr)
        *addr = t->entries[index].addr;
    if (name && namesz > 0)
        snprintf(name, (size_t)namesz, "%s", t->entries[index].name);
    if (builtin)
        *builtin = index < t->builtin_count;
    unlock(t);
    return 1;
}
