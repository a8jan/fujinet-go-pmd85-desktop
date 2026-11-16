/*
 * pmd85symtab -- name<->address table for the debugger's disassembly view.
 *
 * Not installed; shared only between debugger.c and symtab.c. The public
 * surface a frontend actually sees is the pmd85debug_symbol_* wrappers in
 * pmd85debug.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PMD85_SYMTAB_H
#define PMD85_SYMTAB_H

#include <stdint.h>

typedef struct pmd85symtab pmd85symtab;

/* Seeds the built-in table (see symtab.c) and returns an empty-of-user-
 * symbols table ready to use. Never NULL except on out-of-memory. */
pmd85symtab *pmd85symtab_create(void);
void pmd85symtab_destroy(pmd85symtab *t);

/* Loads a symbol file on top of whatever is already there (built-in table
 * plus anything already loaded). Accepts LWASM's `--symbol-dump` format
 * ("NAME EQU $ADDR" per line, verified against actual lwasm output) and a
 * plain "NAME ADDR" fallback (hex with an optional $ or 0x prefix, or plain
 * decimal). ';' starts a comment; blank lines are skipped. Returns the
 * number of symbols added, or -1 if the file could not be opened. */
int pmd85symtab_load(pmd85symtab *t, const char *path);

/* Adds one symbol directly, e.g. from a linker map file. Counts as a user
 * symbol, so pmd85symtab_clear_user() drops it too. Returns 1 on success, 0 on
 * out-of-memory or a NULL table/name. */
int pmd85symtab_add(pmd85symtab *t, uint16_t addr, const char *name);

/* Drops everything pmd85symtab_load added; the built-in table is untouched. */
void pmd85symtab_clear_user(pmd85symtab *t);

/* Exact-address lookup. Returns 1 and copies the name (NUL-terminated,
 * truncated to dstsz) when addr has one, 0 otherwise. Symbols loaded later
 * win ties over earlier ones (a loaded file can shadow a built-in name at
 * the same address), so this scans newest-first. */
int pmd85symtab_lookup_addr(const pmd85symtab *t, uint16_t addr, char *dst,
                           int dstsz);

/* Name -> address, case-sensitive exact match (the same as lwasm's own
 * symbols). Returns 1 and fills *addr_out on a hit. */
int pmd85symtab_lookup_name(const pmd85symtab *t, const char *name,
                           uint16_t *addr_out);

/* Table walk, for a Symbols tab: entries 0..count-1 in table order (the
 * built-ins first, then what was loaded or added). Index order can change
 * when symbols are added or cleared. */
int pmd85symtab_count(const pmd85symtab *t);
/* Returns 1 and fills the outputs (name truncated to namesz), 0 past the end.
 * *builtin is 1 for an entry pmd85symtab_clear_user keeps. */
int pmd85symtab_get(const pmd85symtab *t, int index, uint16_t *addr, char *name,
                   int namesz, int *builtin);

#endif /* PMD85_SYMTAB_H */
