/*
 * pmd85debug engine. (Milestone 1: lifecycle only.)
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>

#include "pmd85debug.h"
#include "session_internal.h"

struct pmd85debug {
    pmd85session *s;
};

pmd85debug *pmd85debug_create(pmd85session *s)
{
    pmd85debug *d = calloc(1, sizeof(*d));
    if (d) d->s = s;
    return d;
}

void pmd85debug_destroy(pmd85debug *d) { free(d); }
void pmd85debug_resume_for_stop(pmd85debug *d) { (void)d; }
void pmd85debug_trace_install(pmd85debug *d) { (void)d; }
