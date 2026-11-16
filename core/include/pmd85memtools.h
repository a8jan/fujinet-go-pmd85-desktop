/*
 * Watches, range parsing, memory-search queries and breakpoint descriptions,
 * kept in the core so the debugger windows only lay them out. Every call
 * that takes an address knows the two spaces of pmd85debug.h: the CPU's 64K
 * and (CoCo 3) physical RAM.
 *
 * A watch is text the user typed (an address, a symbol, an expression, or a
 * range lo-hi / lo..hi) shown with its live value. It is stored as text and
 * resolved every time it is shown, so loading symbols later makes a symbol
 * start resolving.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PMD85MEMTOOLS_H
#define PMD85MEMTOOLS_H

#include <stdint.h>

#include "pmd85debug.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "lo..hi", "lo-hi" (plain numbers or symbols on both sides), or one
 * expression (an address, symbol, symbol+3, [ptr]). A bare token of hex
 * digits ("0400", up to 4) is hex unless it
 * names a symbol; "$0400" and expressions work as in the console. With d
 * NULL only numbers parse. Returns 1 and fills lo and hi (inclusive,
 * lo <= hi, clamped to the space), 0 if the text means nothing. */
int pmd85sym_parse_range(pmd85debug *d, int space, const char *text, uint32_t *lo,
                        uint32_t *hi);

/* ---- watches ---- */
#define PMD85WATCH_MAX 64
/* The process-wide list, like the symbol tables: it outlives the window. */
int  pmd85watch_count(void);
/* Returns the new index, or -1 if empty / full. A watch that does not resolve
 * yet is kept (it shows "?"). `space` is PMD85DEBUG_SPACE_CPU. */
int  pmd85watch_add(const char *text, int space);
void pmd85watch_remove(int index);
void pmd85watch_clear(void);
const char *pmd85watch_text(int index);
int  pmd85watch_space(int index);
unsigned pmd85watch_generation(void);
/* "score  $0400  $42 (66)". While the machine runs the value is "-"; a range
 * shows its first 8 bytes. Returns the length. */
int  pmd85watch_format(pmd85debug *d, int index, char *dst, int dstsz);
/* The range a watch covers (for "Show in Memory"). 1 if it resolves. */
int  pmd85watch_range(pmd85debug *d, int index, uint32_t *lo, uint32_t *hi);

/* ---- memory search ---- */
enum { PMD85SEARCH_HEX = 0, PMD85SEARCH_TEXT };
#define PMD85SEARCH_MAX 32
/* Hex: byte pairs, optional "$" / "0x" / spaces. Text: one byte per
 * character, case-insensitive. Fills pat, n, mask (0xFF) and flags for
 * pmd85debug_search. Returns 1, or 0 for empty / malformed text. */
int pmd85search_parse(int kind, const char *text, uint8_t *pat, int *n,
                     uint8_t *mask, int *flags);

/* ---- breakpoints ---- */
/* "Write  #C000 (SCREEN)  if value != old", "Out  port #3F (BECKER data)". */
int pmd85bp_describe(pmd85debug *d, const pmd85debug_breakpoint *bp, char *dst,
                    int dstsz);

/* ---- memory map ---- */
/* The name of the region of `map` (from pmd85debug_memory_map) holding addr
 * ("RAM", "ROM", "VRAM"), for labelling Memory rows. Fetch the map once
 * per refresh and look rows up in it. Returns the length, 0 and "" when no
 * region covers addr. */
int pmd85mem_region_name(const pmd85debug_region *map, int count, uint16_t addr,
                        char *dst, int dstsz);
/* "CPU": the Memory tab's space label. */
const char *pmd85mem_space_name(int space);

#ifdef __cplusplus
}
#endif

#endif /* PMD85MEMTOOLS_H */
