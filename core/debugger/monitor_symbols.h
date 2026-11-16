/*
 * The PMD 85 monitors' symbols (core/debugger/monitor_symbols.c, generated
 * by tools/gpmd85/extract-monitor-symbols.py).
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PMD85_MONITOR_SYMBOLS_H
#define PMD85_MONITOR_SYMBOLS_H

#include <stdint.h>

typedef struct {
    uint16_t    addr;
    const char *name;
    uint8_t     nchk;     /* 0 for a work variable */
    uint8_t     chk[3];   /* the ROM bytes at addr */
} pmd85_monsym;

typedef struct {
    const char         *rom;    /* the ROM image it was verified against */
    const pmd85_monsym *syms;
    int                 count;
} pmd85_montable;

extern const pmd85_montable pmd85_monitor_tables[];
extern const int pmd85_monitor_table_count;

#endif /* PMD85_MONITOR_SYMBOLS_H */
