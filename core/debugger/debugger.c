/*
 * pmd85debug -- the 8080 debugger engine (see core/include/pmd85debug.h).
 *
 * Shaped after fujinet-go-coco-desktop's cocodebug engine: the emulator
 * thread parks inside the per-instruction hook while the machine is
 * stopped, the reason/PC/serial of a stop are published together under one
 * lock, stepping is a small state machine checked on the way into each
 * instruction. What differs is how accesses are seen: there is no bus tap
 * in GPMD85Emulator's CPU, so the hook decodes the memory and I/O accesses
 * of the instruction about to run (decode_access) and checks them at the
 * next one, when the values written or read are there.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debugger_internal.h"
#include "dwtrace.h"
#include "expr.h"
#include "monitor_symbols.h"
#include "pmd85_host.h"
#include "pmd85_host_internal.h"
#include "session_internal.h"

#define MAX_BP     PMD85DEBUG_MAX_BREAKPOINTS
#define TRACE_N    65536
#define IOLOG_N    4096
#define SPACE_SIZE 0x10000u
#define MAP_MAX    64

/* One memory access the instruction about to run will make. */
typedef struct {
    uint16_t addr;
    uint8_t  rw;      /* PMD85DEBUG_BP_READ / _WRITE bits */
    uint8_t  old;     /* the byte before the instruction */
} mem_access;

struct pmd85debug {
    pmd85session *s;

    pthread_mutex_t mtx;
    pthread_cond_t cv;

    atomic_int engaged;
    int paused;          /* the emulator thread is parked in the hook */
    int stop_all;        /* the session is going away; never park again */
    /* Read without the lock by the hook's fast path (a stale value costs one
     * instruction), written under it. */
    volatile int  pause_request;
    volatile int  step_remaining;
    volatile long transient_bp;     /* one-shot breakpoint, or -1 */
    volatile int  out_active;       /* step out: wait for a RET with SP above out_sp */
    int           out_pending;
    uint16_t      out_sp;
    volatile int  frames_active;    /* run_frames: stop when the frame reaches frame_stop */
    uint64_t      frame_stop;

    /* The breakpoint table, and the maps the hook reads. */
    pmd85debug_breakpoint bp[MAX_BP];
    int nbp, next_id;
    uint8_t map_exec[SPACE_SIZE];   /* nonzero: an enabled exec bp covers it */
    uint8_t map_mem[SPACE_SIZE];    /* PMD85DEBUG_BP_READ / _WRITE bits */
    uint8_t map_io[256];            /* PMD85DEBUG_BP_IN / _OUT bits, mirrors included */

    /* The I/O decode as last seen (at an IN/OUT, or a stop): which register
     * of which device each port number reaches. The PMD's chips decode only
     * some address bits, so a register answers at several ports (mirrors),
     * and a port breakpoint is on the register unless it is exact. */
    pmd85host_port_dev io_devs[PMD85HOST_PORT_DEVS_MAX];
    int n_io_devs;                  /* -1: not seen yet */
    int16_t io_key[256];            /* device * 256 + register bits, -1 none */
    const char *io_name[256];       /* the register's name (static), NULL none */
    atomic_int n_mem;               /* enabled memory read/write breakpoints */
    atomic_int n_io;                /* enabled port breakpoints */

    /* What the previous instruction was decoded to touch (emulator thread). */
    void    *machine_seen;          /* a rebuilt machine drops the pending decode */
    uint16_t pend_pc;
    int      pend_n;
    mem_access pend[2];
    int      pend_io;               /* 1 IN, 2 OUT, 0 none */
    uint8_t  pend_port, pend_out_value;

    /* The access a condition sees as value / addr (port) / old. */
    long acc_value, acc_addr, acc_old;
    int  in_access;

    pmd85debug_regs regs;
    int regs_valid;
    uint64_t stop_serial;
    char reason[160];
    int  reason_addr;
    int  stop_pc;
    atomic_uint generation;

    pmd85debug_region map[MAP_MAX];
    int nmap;

    atomic_int syntax;
    atomic_int hex;

    /* The instruction trace and the I/O log: their own lock, so a window
     * reading them never waits for a frame. */
    atomic_int trace_on;
    atomic_int io_log_on;
    pthread_mutex_t ring_mtx;
    pmd85debug_trace_entry *ring;
    int ring_head, ring_count;
    pmd85debug_io_access *iolog;
    int io_head, io_count;
    uint8_t io_cap[256];      /* the ports the log records (1 each by default) */

    pmd85symtab *symtab;
    void *monsym_machine;     /* the machine the monitor symbols were picked for */
    char  monsym_info[128];   /* what was picked, for the prompt and a window */

    /* The FujiNet tab's DriveWire trace, fed from the Becker port. */
    pthread_mutex_t trace_mtx;
    dwtrace *trace;
};

/* The debugger the Becker port's tap feeds. */
static pmd85debug *g_trace_dbg;

static uint8_t dasm_read(uint16_t addr, void *user);

static void bump(pmd85debug *d)
{
    atomic_fetch_add(&d->generation, 1u);
}

const char *pmd85hx(pmd85debug *d, unsigned v, int digits)
{
    /* a small ring per thread: several in one printf, and the hook and a
     * window may format at the same time */
    static _Thread_local char ring[8][16];
    static _Thread_local int next;
    char *dst = ring[next++ & 7];
    i8080_hex_format(dst, 16, v, digits, d ? (i8080_hex)atomic_load(&d->hex) : I8080_HEX_H);
    return dst;
}

static uint8_t peek(uint16_t addr)
{
    int v = pmd85host_dbg_peek(addr);
    return v < 0 ? 0xFF : (uint8_t)v;
}

static void regs_from_cpu(const pmd85host_cpu *c, pmd85debug_regs *r)
{
    memset(r, 0, sizeof *r);
    r->pc = c->pc;
    r->sp = c->sp;
    r->a = (uint8_t)(c->af >> 8);
    r->f = (uint8_t)c->af;
    r->b = (uint8_t)(c->bc >> 8);
    r->c = (uint8_t)c->bc;
    r->d = (uint8_t)(c->de >> 8);
    r->e = (uint8_t)c->de;
    r->h = (uint8_t)(c->hl >> 8);
    r->l = (uint8_t)c->hl;
    r->iff = c->iff;
    r->halt = c->halt;
}

static void live_regs(pmd85debug_regs *r)
{
    pmd85host_cpu c;
    pmd85host_dbg_cpu(&c);
    regs_from_cpu(&c, r);
}

/* ---- expressions -------------------------------------------------------- */

typedef struct {
    pmd85debug *d;
    pmd85debug_regs regs;
    int have_regs;
    int can_peek;
    int in_access;
    long value, addr, old;
} evalctx;

const char *const pmd85debug_reg_names[] = {
    "pc", "sp", "a", "f", "b", "c", "d", "e", "h", "l", "bc", "de", "hl", "psw", "af",
    "sf", "zf", "hf", "pf", "cf", "cycles", "frame", "tframe", "value", "addr", "port",
    "old", NULL
};

static uint8_t eval_read8(void *user, uint16_t addr)
{
    evalctx *ctx = user;
    return ctx->can_peek ? peek(addr) : 0;
}

static int eval_name(void *user, const char *name, long *out)
{
    evalctx *ctx = user;
    const pmd85debug_regs *r = &ctx->regs;
    uint16_t addr;
    char low[16];
    size_t pos;

    for (pos = 0; name[pos] && pos < sizeof low - 1; pos++)
        low[pos] = (char)tolower((unsigned char)name[pos]);
    low[pos] = '\0';
    if (name[pos])
        low[0] = '\0';

    if (!strcmp(low, "pc"))  { *out = r->pc; return 1; }
    if (!strcmp(low, "sp"))  { *out = r->sp; return 1; }
    if (!strcmp(low, "a"))   { *out = r->a; return 1; }
    if (!strcmp(low, "f"))   { *out = r->f; return 1; }
    if (!strcmp(low, "b"))   { *out = r->b; return 1; }
    if (!strcmp(low, "c"))   { *out = r->c; return 1; }
    if (!strcmp(low, "d"))   { *out = r->d; return 1; }
    if (!strcmp(low, "e"))   { *out = r->e; return 1; }
    if (!strcmp(low, "h"))   { *out = r->h; return 1; }
    if (!strcmp(low, "l"))   { *out = r->l; return 1; }
    if (!strcmp(low, "bc"))  { *out = (r->b << 8) | r->c; return 1; }
    if (!strcmp(low, "de"))  { *out = (r->d << 8) | r->e; return 1; }
    if (!strcmp(low, "hl"))  { *out = (r->h << 8) | r->l; return 1; }
    if (!strcmp(low, "psw") || !strcmp(low, "af")) { *out = (r->a << 8) | r->f; return 1; }
    if (!strcmp(low, "sf"))  { *out = (r->f & PMD85DEBUG_F_S) != 0; return 1; }
    if (!strcmp(low, "zf"))  { *out = (r->f & PMD85DEBUG_F_Z) != 0; return 1; }
    if (!strcmp(low, "hf"))  { *out = (r->f & PMD85DEBUG_F_AC) != 0; return 1; }
    if (!strcmp(low, "pf"))  { *out = (r->f & PMD85DEBUG_F_P) != 0; return 1; }
    if (!strcmp(low, "cf"))  { *out = (r->f & PMD85DEBUG_F_CY) != 0; return 1; }
    if (!strcmp(low, "cycles") || !strcmp(low, "frame") || !strcmp(low, "tframe")) {
        uint64_t ts = 0, fr = 0;
        int inf = 0;
        if (ctx->can_peek)
            pmd85host_dbg_clock(&ts, &fr, &inf);
        *out = !strcmp(low, "cycles") ? (long)ts : !strcmp(low, "frame") ? (long)fr : inf;
        return 1;
    }
    if (ctx->in_access) {
        if (!strcmp(low, "value")) { *out = ctx->value; return 1; }
        if (!strcmp(low, "addr") || !strcmp(low, "port")) { *out = ctx->addr; return 1; }
        if (!strcmp(low, "old"))   { *out = ctx->old; return 1; }
    }
    if (pmd85symtab_lookup_name(ctx->d->symtab, name, &addr)) {
        *out = addr;
        return 1;
    }
    return 0;
}

/* From a UI thread: the stop's snapshot. */
static void ui_context(pmd85debug *d, evalctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->d = d;
    pthread_mutex_lock(&d->mtx);
    if (d->paused && d->regs_valid) {
        ctx->regs = d->regs;
        ctx->have_regs = 1;
        ctx->can_peek = 1;
    }
    pthread_mutex_unlock(&d->mtx);
}

int pmd85debug_eval(pmd85debug *d, const char *text, long *out, char *err, int errsz)
{
    evalctx ctx;
    expr_ctx ex;

    if (!d)
        return -1;
    ui_context(d, &ctx);
    ex.name = eval_name;
    ex.read8 = eval_read8;
    ex.user = &ctx;
    return expr_eval(text, &ex, out, err, errsz);
}

pmd85symtab *pmd85debug_symtab(pmd85debug *d)
{
    return d->symtab;
}

/* Emulator thread, d->mtx held: a condition against the live machine. */
static int cond_true(pmd85debug *d, const pmd85debug_breakpoint *bp)
{
    evalctx ctx;
    expr_ctx ex;
    long v = 0;
    char err[64];

    if (!bp->condition[0])
        return 1;
    memset(&ctx, 0, sizeof(ctx));
    ctx.d = d;
    live_regs(&ctx.regs);
    ctx.have_regs = 1;
    ctx.can_peek = 1;
    ctx.in_access = d->in_access;
    ctx.value = d->acc_value;
    ctx.addr = d->acc_addr;
    ctx.old = d->acc_old;
    ex.name = eval_name;
    ex.read8 = eval_read8;
    ex.user = &ctx;
    if (expr_eval(bp->condition, &ex, &v, err, sizeof(err)) != 0)
        return 1;   /* a condition that no longer evaluates stops, rather than hides */
    return v != 0;
}

/* Whether a port is one a port breakpoint is on: in its range, or (unless
 * it is exact) reaching the same register as a port in its range. */
static int bp_port_hit(const pmd85debug *d, const pmd85debug_breakpoint *bp, unsigned port)
{
    uint32_t q;
    if (port >= bp->start && port <= bp->end)
        return 1;
    if ((bp->type & PMD85DEBUG_BP_EXACT) || d->io_key[port] < 0)
        return 0;
    for (q = bp->start; q <= bp->end && q < 256; q++)
        if (d->io_key[q] == d->io_key[port])
            return 1;
    return 0;
}

/* Rebuild what the hook reads from the table. Caller holds mtx. */
static void rebuild_locked(pmd85debug *d)
{
    int pos, nmem = 0, nio = 0;
    uint32_t addr;

    memset(d->map_exec, 0, sizeof d->map_exec);
    memset(d->map_mem, 0, sizeof d->map_mem);
    memset(d->map_io, 0, sizeof d->map_io);
    for (pos = 0; pos < d->nbp; pos++) {
        const pmd85debug_breakpoint *bp = &d->bp[pos];
        if (!bp->enabled)
            continue;
        if (bp->type & PMD85DEBUG_BP_IO) {
            nio++;
            for (addr = 0; addr < 256; addr++)
                if (bp_port_hit(d, bp, addr))
                    d->map_io[addr] |= (uint8_t)(bp->type & PMD85DEBUG_BP_IO);
            continue;
        }
        if (bp->type & PMD85DEBUG_BP_ACCESS)
            nmem++;
        for (addr = bp->start; addr <= bp->end && addr < SPACE_SIZE; addr++) {
            if (bp->type & PMD85DEBUG_BP_EXEC)
                d->map_exec[addr] = 1;
            d->map_mem[addr] |= (uint8_t)(bp->type & PMD85DEBUG_BP_ACCESS);
        }
    }
    atomic_store(&d->n_mem, nmem);
    atomic_store(&d->n_io, nio);
}

/* Reads the machine's I/O decode; 1 when it changed (the port breakpoints'
 * map is then out of date). Caller holds mtx, on the emulator thread or with
 * the machine parked. */
static int io_decode_refresh_locked(pmd85debug *d)
{
    pmd85host_port_dev devs[PMD85HOST_PORT_DEVS_MAX];
    const int n = pmd85host_port_devs(devs, PMD85HOST_PORT_DEVS_MAX);
    int i, same = n == d->n_io_devs;
    unsigned port;

    for (i = 0; same && i < n; i++)
        same = devs[i].name == d->io_devs[i].name && devs[i].mask == d->io_devs[i].mask &&
               devs[i].adr == d->io_devs[i].adr && devs[i].reg == d->io_devs[i].reg &&
               devs[i].regs == d->io_devs[i].regs;
    if (same)
        return 0;
    for (i = 0; i < n; i++)
        d->io_devs[i] = devs[i];
    d->n_io_devs = n;
    for (port = 0; port < 256; port++) {
        d->io_key[port] = -1;
        d->io_name[port] = NULL;
        for (i = 0; i < n; i++)
            if ((port & devs[i].mask) == devs[i].adr) {
                d->io_key[port] = (int16_t)(i * 256 + (port & devs[i].reg));
                d->io_name[port] = devs[i].regs[port & devs[i].reg];
                break;
            }
    }
    return 1;
}

/* ---- the access decoder --------------------------------------------------
 * What the instruction at c->pc will read and write, from its opcode and
 * the registers. Emulator thread. */
static int cond_met(int y, uint8_t f)
{
    switch (y) {
    case 0: return !(f & PMD85DEBUG_F_Z);
    case 1: return (f & PMD85DEBUG_F_Z) != 0;
    case 2: return !(f & PMD85DEBUG_F_CY);
    case 3: return (f & PMD85DEBUG_F_CY) != 0;
    case 4: return !(f & PMD85DEBUG_F_P);
    case 5: return (f & PMD85DEBUG_F_P) != 0;
    case 6: return !(f & PMD85DEBUG_F_S);
    default: return (f & PMD85DEBUG_F_S) != 0;
    }
}

static void add_acc(pmd85debug *d, uint16_t addr, int rw)
{
    if (d->pend_n < 2) {
        d->pend[d->pend_n].addr = addr;
        d->pend[d->pend_n].rw = (uint8_t)rw;
        d->pend[d->pend_n].old = peek(addr);
        d->pend_n++;
    }
}

static void add_word(pmd85debug *d, uint16_t addr, int rw)
{
    add_acc(d, addr, rw);
    add_acc(d, (uint16_t)(addr + 1), rw);
}

static void decode_access(pmd85debug *d, const pmd85host_cpu *c, int want_mem)
{
    const uint8_t op = peek(c->pc);
    const uint8_t f = (uint8_t)c->af;
    const uint16_t a16 = (uint16_t)(peek((uint16_t)(c->pc + 1)) |
                                    (peek((uint16_t)(c->pc + 2)) << 8));
    const int R = PMD85DEBUG_BP_READ, W = PMD85DEBUG_BP_WRITE;

    d->pend_n = 0;
    d->pend_io = 0;
    d->pend_pc = c->pc;
    if (c->inta)
        return;   /* an interrupt is taken instead */

    if (op == 0xD3) {
        d->pend_io = 2;
        d->pend_port = peek((uint16_t)(c->pc + 1));
        d->pend_out_value = (uint8_t)(c->af >> 8);
        return;
    }
    if (op == 0xDB) {
        d->pend_io = 1;
        d->pend_port = peek((uint16_t)(c->pc + 1));
        return;
    }
    if (!want_mem)
        return;

    switch (op) {
    case 0x02: add_acc(d, c->bc, W); return;
    case 0x12: add_acc(d, c->de, W); return;
    case 0x0A: add_acc(d, c->bc, R); return;
    case 0x1A: add_acc(d, c->de, R); return;
    case 0x22: add_word(d, a16, W); return;
    case 0x2A: add_word(d, a16, R); return;
    case 0x32: add_acc(d, a16, W); return;
    case 0x3A: add_acc(d, a16, R); return;
    case 0x34: case 0x35: add_acc(d, c->hl, R | W); return;
    case 0x36: add_acc(d, c->hl, W); return;
    case 0xE3: add_word(d, c->sp, R | W); return;   /* XTHL */
    default: break;
    }
    if (op >= 0x40 && op <= 0x7F && op != 0x76) {
        if ((op & 7) == 6) add_acc(d, c->hl, R);          /* MOV r,M */
        else if (((op >> 3) & 7) == 6) add_acc(d, c->hl, W);  /* MOV M,r */
        return;
    }
    if (op >= 0x80 && op <= 0xBF) {
        if ((op & 7) == 6) add_acc(d, c->hl, R);          /* ALU M */
        return;
    }
    if (op >= 0xC0) {
        const int y = (op >> 3) & 7, z = op & 7;
        switch (z) {
        case 0:   /* Rcc */
            if (cond_met(y, f)) add_word(d, c->sp, R);
            return;
        case 1:   /* POP, RET (C9 and its alias D9) */
            if (!(y & 1) || y == 1 || y == 3) add_word(d, c->sp, R);
            return;
        case 4:   /* Ccc */
            if (cond_met(y, f)) add_word(d, (uint16_t)(c->sp - 2), W);
            return;
        case 5:   /* PUSH; CALL and its aliases */
            add_word(d, (uint16_t)(c->sp - 2), W);
            return;
        case 7:   /* RST */
            add_word(d, (uint16_t)(c->sp - 2), W);
            return;
        default:
            return;
        }
    }
}

/* ---- the I/O log and the trace ring ------------------------------------- */

static void io_log(pmd85debug *d, uint16_t pc, uint8_t port, uint8_t value, int out)
{
    pmd85debug_io_access e;
    uint64_t ts = 0;

    pmd85host_dbg_clock(&ts, NULL, NULL);
    e.tstates = ts;
    e.pc = pc;
    e.port = port;
    e.value = value;
    e.out = (uint8_t)(out ? 1 : 0);
    pthread_mutex_lock(&d->ring_mtx);
    if (!d->io_cap[port]) {
        pthread_mutex_unlock(&d->ring_mtx);
        return;
    }
    if (!d->iolog)
        d->iolog = calloc(IOLOG_N, sizeof *d->iolog);
    if (d->iolog) {
        d->iolog[d->io_head] = e;
        d->io_head = (d->io_head + 1) % IOLOG_N;
        if (d->io_count < IOLOG_N)
            d->io_count++;
    }
    pthread_mutex_unlock(&d->ring_mtx);
}

static void trace_record(pmd85debug *d, const pmd85host_cpu *c)
{
    pmd85debug_trace_entry e;
    uint64_t ts = 0, fr = 0;
    int inf = 0, pos;

    memset(&e, 0, sizeof e);
    e.pc = c->pc;
    e.len = (uint8_t)i8080_length(peek(c->pc));
    for (pos = 0; pos < e.len; pos++)
        e.bytes[pos] = peek((uint16_t)(c->pc + pos));
    e.a = (uint8_t)(c->af >> 8);
    e.f = (uint8_t)c->af;
    e.b = (uint8_t)(c->bc >> 8);
    e.c = (uint8_t)c->bc;
    e.d = (uint8_t)(c->de >> 8);
    e.e = (uint8_t)c->de;
    e.h = (uint8_t)(c->hl >> 8);
    e.l = (uint8_t)c->hl;
    e.sp = c->sp;
    pmd85host_dbg_clock(&ts, &fr, &inf);
    e.tstates = ts;
    e.frame = (uint32_t)fr;
    e.in_frame = inf;

    pthread_mutex_lock(&d->ring_mtx);
    if (d->ring) {
        d->ring[d->ring_head] = e;
        d->ring_head = (d->ring_head + 1) % TRACE_N;
        if (d->ring_count < TRACE_N)
            d->ring_count++;
    }
    pthread_mutex_unlock(&d->ring_mtx);
}

/* ---- the monitor's symbols -------------------------------------------------
 * Every table in monitor_symbols.c was verified against one ROM image; the
 * running monitor may be that one, a variant (monit2A differs from monit2
 * only in HEADIN; monit1u from monit1 in two places) or something else. A
 * routine's symbol is enabled only where the bytes at it match, and a table
 * is used only when most of its routines do. Emulator thread (parked). */
static int monsym_matches(const pmd85_monsym *m)
{
    int k;
    for (k = 0; k < m->nchk && peek((uint16_t)(m->addr + k)) == m->chk[k]; k++) ;
    return k == m->nchk;
}

static void select_monitor_symbols(pmd85debug *d)
{
    int best = -1, best_hits = 0, best_code = 0, t, i, n = 0, nvar = 0;
    uint16_t *addrs;
    const char **names;
    const pmd85_montable *tab;

    for (t = 0; t < pmd85_monitor_table_count; t++) {
        int code = 0, hits = 0;
        tab = &pmd85_monitor_tables[t];
        for (i = 0; i < tab->count; i++) {
            if (!tab->syms[i].nchk)
                continue;
            code++;
            hits += monsym_matches(&tab->syms[i]);
        }
        if (hits * 2 > code && hits > best_hits) {
            best = t;
            best_hits = hits;
            best_code = code;
        }
    }
    if (best < 0) {
        pmd85symtab_set_builtin(d->symtab, NULL, NULL, 0);
        snprintf(d->monsym_info, sizeof d->monsym_info,
                 "monitor symbols: none (no listing matches this monitor)");
        return;
    }
    tab = &pmd85_monitor_tables[best];
    addrs = malloc((size_t)tab->count * sizeof *addrs);
    names = malloc((size_t)tab->count * sizeof *names);
    if (!addrs || !names) {
        free(addrs);
        free(names);
        return;
    }
    for (i = 0; i < tab->count; i++) {
        if (!monsym_matches(&tab->syms[i]))
            continue;
        nvar += !tab->syms[i].nchk;
        addrs[n] = tab->syms[i].addr;
        names[n++] = tab->syms[i].name;
    }
    pmd85symtab_set_builtin(d->symtab, addrs, names, n);
    snprintf(d->monsym_info, sizeof d->monsym_info,
             "monitor symbols: %s listing, %d of %d routines match, %d variables",
             tab->rom, best_hits, best_code, nvar);
    free(addrs);
    free(names);
}

int pmd85debug_monitor_symbols_info(pmd85debug *d, char *dst, int dstsz)
{
    int n;
    if (!d || !dst || dstsz <= 0)
        return 0;
    pthread_mutex_lock(&d->mtx);
    n = snprintf(dst, (size_t)dstsz, "%s",
                 d->monsym_info[0] ? d->monsym_info : "monitor symbols: not yet (stop once)");
    pthread_mutex_unlock(&d->mtx);
    return n < dstsz ? n : dstsz - 1;
}

/* ---- parking -------------------------------------------------------------
 * Caller holds d->mtx on the emulator thread, inside the hook. */
static void park_locked(pmd85debug *d, const pmd85host_cpu *c, const char *reason)
{
    if (d->machine_seen != d->monsym_machine) {
        select_monitor_symbols(d);
        d->monsym_machine = d->machine_seen;
    }
    if (io_decode_refresh_locked(d))
        rebuild_locked(d);
    regs_from_cpu(c, &d->regs);
    d->regs_valid = 1;
    snprintf(d->reason, sizeof(d->reason), "%s", reason);
    d->reason_addr = c->pc;
    d->stop_pc = c->pc;
    d->stop_serial++;
    d->nmap = 0;
    d->paused = 1;
    bump(d);
    while (d->paused && !d->stop_all)
        pthread_cond_wait(&d->cv, &d->mtx);
    d->paused = 0;
    d->regs_valid = 0;
    bump(d);
}

static void clear_motion_locked(pmd85debug *d)
{
    d->pause_request = 0;
    d->step_remaining = 0;
    d->transient_bp = -1;
    d->out_active = 0;
    d->frames_active = 0;
}

/* The previous instruction's accesses, now that it has run: the I/O log,
 * and whether a read/write/port breakpoint fired. Returns 1 with the reason
 * in text. Caller holds mtx. */
static int check_pending_locked(pmd85debug *d, const pmd85host_cpu *c, char *text,
                                size_t textsz)
{
    int i, pos, hit = 0;

    if (d->pend_io && io_decode_refresh_locked(d))
        rebuild_locked(d);      /* the machine was reconfigured: new mirrors */
    if (d->pend_io) {
        const int out = d->pend_io == 2;
        const uint8_t value = out ? d->pend_out_value : (uint8_t)(c->af >> 8);
        const int bit = out ? PMD85DEBUG_BP_OUT : PMD85DEBUG_BP_IN;

        if (atomic_load_explicit(&d->io_log_on, memory_order_relaxed))
            io_log(d, d->pend_pc, d->pend_port, value, out);
        if (d->map_io[d->pend_port] & bit) {
            d->acc_addr = d->pend_port;
            d->acc_value = value;
            d->acc_old = value;
            d->in_access = 1;
            for (pos = 0; pos < d->nbp && !hit; pos++) {
                pmd85debug_breakpoint *bp = &d->bp[pos];
                if (bp->enabled && (bp->type & bit) && bp_port_hit(d, bp, d->pend_port) &&
                    cond_true(d, bp)) {
                    const char *name = pmd85host_port_name(d->pend_port);
                    bp->hits++;
                    hit = 1;
                    snprintf(text, textsz, "%s %s%s%s%s = %s at %s",
                             out ? "OUT" : "IN", pmd85hx(d, d->pend_port, 2),
                             name ? " (" : "", name ? name : "", name ? ")" : "",
                             pmd85hx(d, value, 2), pmd85hx(d, d->pend_pc, 4));
                }
            }
            d->in_access = 0;
        }
    }
    for (i = 0; i < d->pend_n && !hit; i++) {
        const mem_access *a = &d->pend[i];
        if (!(d->map_mem[a->addr] & a->rw))
            continue;
        d->acc_addr = a->addr;
        d->acc_value = peek(a->addr);
        d->acc_old = a->old;
        d->in_access = 1;
        for (pos = 0; pos < d->nbp && !hit; pos++) {
            pmd85debug_breakpoint *bp = &d->bp[pos];
            const int kinds = bp->type & a->rw & PMD85DEBUG_BP_ACCESS;
            if (bp->enabled && kinds && a->addr >= bp->start && a->addr <= bp->end &&
                cond_true(d, bp)) {
                bp->hits++;
                hit = 1;
                if (kinds & PMD85DEBUG_BP_WRITE)
                    snprintf(text, textsz, "wrote %s = %s (was %s) at %s",
                             pmd85hx(d, a->addr, 4), pmd85hx(d, (unsigned)d->acc_value, 2),
                             pmd85hx(d, a->old, 2), pmd85hx(d, d->pend_pc, 4));
                else
                    snprintf(text, textsz, "read %s = %s at %s", pmd85hx(d, a->addr, 4),
                             pmd85hx(d, (unsigned)d->acc_value, 2),
                             pmd85hx(d, d->pend_pc, 4));
            }
        }
        d->in_access = 0;
    }
    d->pend_n = 0;
    d->pend_io = 0;
    return hit;
}

/* ---- the hook -------------------------------------------------------------
 * Emulator thread, before every instruction while engaged. */
static void instruction_hook(void *ctx, uint16_t pc_arg)
{
    pmd85debug *d = ctx;
    pmd85host_cpu c;
    void *machine;
    int stop = 0, want_mem, want_io;
    const char *reason = "stopped";
    char text[160];

    (void)pc_arg;
    if (d->stop_all || !atomic_load_explicit(&d->engaged, memory_order_relaxed))
        return;
    if (!pmd85host_dbg_cpu(&c))
        return;

    machine = pmd85host_machine_ptr();
    if (machine != d->machine_seen) {
        /* a rebuilt machine: what was decoded belonged to the old one */
        d->machine_seen = machine;
        d->pend_n = 0;
        d->pend_io = 0;
    }

    if (!(d->pend_n || d->pend_io || d->pause_request || d->step_remaining > 0 ||
          d->out_active || d->transient_bp >= 0 || d->frames_active ||
          d->map_exec[c.pc]))
        goto decode;

    pthread_mutex_lock(&d->mtx);

    if ((d->pend_n || d->pend_io) && check_pending_locked(d, &c, text, sizeof text)) {
        clear_motion_locked(d);
        stop = 1;
        reason = text;
    }
    if (!stop && d->pause_request) {
        d->pause_request = 0;
        stop = 1;
        snprintf(text, sizeof text, "stopped at %s", pmd85hx(d, c.pc, 4));
        reason = text;
    } else if (!stop && d->step_remaining > 0) {
        if (--d->step_remaining == 0) {
            stop = 1;
            snprintf(text, sizeof text, "step at %s", pmd85hx(d, c.pc, 4));
            reason = text;
        }
    }
    if (!stop && d->out_active) {
        if (d->out_pending) {
            d->out_pending = 0;
            if (c.sp > d->out_sp) {
                d->out_active = 0;
                stop = 1;
                snprintf(text, sizeof text, "returned to %s", pmd85hx(d, c.pc, 4));
                reason = text;
            }
        }
    }
    if (!stop && d->frames_active) {
        uint64_t fr = 0;
        pmd85host_dbg_clock(NULL, &fr, NULL);
        if (fr >= d->frame_stop) {
            d->frames_active = 0;
            stop = 1;
            snprintf(text, sizeof text, "frame %llu at %s", (unsigned long long)fr,
                     pmd85hx(d, c.pc, 4));
            reason = text;
        }
    }
    if (d->transient_bp >= 0 && (uint16_t)d->transient_bp == c.pc) {
        /* spent whether or not it is what stopped us */
        d->transient_bp = -1;
        if (!stop) {
            clear_motion_locked(d);
            stop = 1;
            snprintf(text, sizeof text, "returned to %s", pmd85hx(d, c.pc, 4));
            reason = text;
        }
    }
    if (!stop && d->map_exec[c.pc]) {
        int pos;
        d->acc_addr = c.pc;
        d->acc_value = peek(c.pc);
        d->acc_old = d->acc_value;
        d->in_access = 1;
        for (pos = 0; pos < d->nbp; pos++) {
            pmd85debug_breakpoint *bp = &d->bp[pos];
            if (bp->enabled && (bp->type & PMD85DEBUG_BP_EXEC) && c.pc >= bp->start &&
                c.pc <= bp->end && cond_true(d, bp)) {
                bp->hits++;
                clear_motion_locked(d);
                stop = 1;
                snprintf(text, sizeof text, "breakpoint: exec %s", pmd85hx(d, c.pc, 4));
                reason = text;
                break;
            }
        }
        d->in_access = 0;
    }

    if (stop) {
        park_locked(d, &c, reason);
        /* the user may have changed registers or memory while parked */
        pmd85host_dbg_cpu(&c);
    }
    /* step out: is the instruction about to run return-class? */
    if (d->out_active) {
        uint8_t op = peek(c.pc);
        d->out_pending = op == 0xC9 || op == 0xD9 || (op & 0xC7) == 0xC0;
    }
    pthread_mutex_unlock(&d->mtx);

decode:
    /* The trace is what ran: recorded on the way out, with any edit made
     * while stopped (PC, registers) already in. */
    if (atomic_load_explicit(&d->trace_on, memory_order_relaxed))
        trace_record(d, &c);
    /* again, after any park: breakpoints added while stopped count from
     * the very next instruction */
    want_mem = atomic_load_explicit(&d->n_mem, memory_order_relaxed) > 0;
    want_io = atomic_load_explicit(&d->n_io, memory_order_relaxed) > 0 ||
              atomic_load_explicit(&d->io_log_on, memory_order_relaxed);
    if (want_mem || want_io)
        decode_access(d, &c, want_mem);
}

/* ---- lifecycle ------------------------------------------------------------ */

pmd85debug *pmd85debug_create(pmd85session *s)
{
    pmd85debug *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->s = s;
    pthread_mutex_init(&d->mtx, NULL);
    pthread_cond_init(&d->cv, NULL);
    pthread_mutex_init(&d->ring_mtx, NULL);
    pthread_mutex_init(&d->trace_mtx, NULL);
    d->transient_bp = -1;
    d->next_id = 1;
    d->n_io_devs = -1;
    memset(d->io_key, 0xFF, sizeof d->io_key);      /* -1: nothing answers */
    atomic_store(&d->io_log_on, 1);
    memset(d->io_cap, 1, sizeof d->io_cap);
    {
        /* the display choices persist with the session's settings */
        int syn = pmd85session_get_int(s, "debugger_syntax", I8080_SYNTAX_INTEL);
        int hex = pmd85session_get_int(s, "debugger_hex", I8080_HEX_H);
        atomic_store(&d->syntax, syn == I8080_SYNTAX_Z80 ? syn : I8080_SYNTAX_INTEL);
        atomic_store(&d->hex, hex >= 0 && hex < I8080_HEX_COUNT ? hex : I8080_HEX_H);
    }
    d->symtab = pmd85symtab_create();
    d->trace = dwtrace_create();
    return d;
}

void pmd85debug_destroy(pmd85debug *d)
{
    if (!d)
        return;
    if (g_trace_dbg == d) {
        pmd85host_set_becker_trace(NULL);
        g_trace_dbg = NULL;
    }
    pmd85host_set_instr_hook(NULL, NULL);
    pmd85symtab_destroy(d->symtab);
    dwtrace_destroy(d->trace);
    free(d->ring);
    free(d->iolog);
    pthread_mutex_destroy(&d->mtx);
    pthread_cond_destroy(&d->cv);
    pthread_mutex_destroy(&d->ring_mtx);
    pthread_mutex_destroy(&d->trace_mtx);
    free(d);
}

void pmd85debug_resume_for_stop(pmd85debug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    d->stop_all = 1;
    d->paused = 0;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
}

/* A session is starting (UI thread, before the emulator thread): clear what
 * the stop path or the old machine left behind. */
static void session_begin(pmd85debug *d)
{
    pthread_mutex_lock(&d->mtx);
    d->stop_all = 0;
    d->paused = 0;
    d->regs_valid = 0;
    clear_motion_locked(d);
    d->pend_n = 0;
    d->pend_io = 0;
    d->machine_seen = NULL;
    d->monsym_machine = NULL;
    d->nmap = 0;
    pthread_mutex_unlock(&d->mtx);
    bump(d);
}

static void becker_trace(int dir, const uint8_t *buf, size_t len)
{
    pmd85debug *d = g_trace_dbg;
    if (!d)
        return;
    pthread_mutex_lock(&d->trace_mtx);
    dwtrace_feed(d->trace, dir, buf, len);
    pthread_mutex_unlock(&d->trace_mtx);
}

void pmd85debug_trace_install(pmd85debug *d)
{
    if (!d)
        return;
    session_begin(d);
    g_trace_dbg = d;
    pmd85host_set_becker_trace(becker_trace);
    if (atomic_load(&d->engaged))
        pmd85host_set_instr_hook(instruction_hook, d);
}

/* ---- engage / pause / step ------------------------------------------------ */

void pmd85debug_set_engaged(pmd85debug *d, int engaged)
{
    if (!d)
        return;
    engaged = engaged ? 1 : 0;
    if (!engaged)
        pmd85debug_resume(d);
    atomic_store(&d->engaged, engaged);
    /* the hook goes away entirely when released: full speed again */
    pmd85host_set_instr_hook(engaged ? instruction_hook : NULL, engaged ? d : NULL);
    bump(d);
}

int pmd85debug_is_engaged(const pmd85debug *d)
{
    return d ? atomic_load(&((pmd85debug *)d)->engaged) : 0;
}

void pmd85debug_pause(pmd85debug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    if (!d->paused) {
        clear_motion_locked(d);
        d->pause_request = 1;
    }
    pthread_mutex_unlock(&d->mtx);
}

void pmd85debug_resume(pmd85debug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    clear_motion_locked(d);
    d->paused = 0;
    pthread_cond_broadcast(&d->cv);
    pthread_mutex_unlock(&d->mtx);
    bump(d);
}

int pmd85debug_is_paused(const pmd85debug *d)
{
    pmd85debug *mut = (pmd85debug *)d;
    int paused;

    if (!d)
        return 0;
    pthread_mutex_lock(&mut->mtx);
    paused = mut->paused;
    pthread_mutex_unlock(&mut->mtx);
    return paused;
}

/* Let the parked thread go with the motion state already set. Caller holds
 * mtx. */
static void release_locked(pmd85debug *d)
{
    d->paused = 0;
    pthread_cond_broadcast(&d->cv);
}

void pmd85debug_step(pmd85debug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    clear_motion_locked(d);
    d->step_remaining = 1;
    release_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

void pmd85debug_step_over(pmd85debug *d)
{
    i8080_line line;
    uint16_t pc;

    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    if (!d->paused || !d->regs_valid) {
        pthread_mutex_unlock(&d->mtx);
        pmd85debug_step(d);
        return;
    }
    pc = d->regs.pc;
    i8080_dasm_one(pc, dasm_read, NULL, I8080_SYNTAX_INTEL, I8080_HEX_H, &line);
    pthread_mutex_unlock(&d->mtx);
    if (!(line.flags & I8080_CALL)) {
        pmd85debug_step(d);
        return;
    }
    pthread_mutex_lock(&d->mtx);
    clear_motion_locked(d);
    d->transient_bp = (uint16_t)(pc + line.len);
    release_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

void pmd85debug_step_out(pmd85debug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    if (!d->paused || !d->regs_valid) {
        pthread_mutex_unlock(&d->mtx);
        pmd85debug_step(d);
        return;
    }
    /* Depth, not a guess at the return address: run until a return-class
     * instruction has executed and SP is above where it is now. A nested
     * call's RET comes back to this SP or lower, so it does not stop us. */
    clear_motion_locked(d);
    d->out_sp = d->regs.sp;
    d->out_active = 1;
    {
        uint8_t op = peek(d->regs.pc);
        d->out_pending = op == 0xC9 || op == 0xD9 || (op & 0xC7) == 0xC0;
    }
    release_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

void pmd85debug_run_to(pmd85debug *d, uint16_t addr)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    clear_motion_locked(d);
    d->transient_bp = addr;
    release_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

void pmd85debug_run_frames(pmd85debug *d, int n)
{
    uint64_t fr = 0;

    if (!d || n < 1)
        return;
    pthread_mutex_lock(&d->mtx);
    pmd85host_dbg_clock(NULL, &fr, NULL);   /* parked or not: a frame number */
    clear_motion_locked(d);
    d->frame_stop = fr + (uint64_t)n;
    d->frames_active = 1;
    release_locked(d);
    pthread_mutex_unlock(&d->mtx);
}

uint64_t pmd85debug_stop_serial(const pmd85debug *d)
{
    pmd85debug *mut = (pmd85debug *)d;
    uint64_t serial;

    if (!d)
        return 0;
    pthread_mutex_lock(&mut->mtx);
    serial = mut->stop_serial;
    pthread_mutex_unlock(&mut->mtx);
    return serial;
}

void pmd85debug_stop_snapshot(pmd85debug *d, pmd85debug_stop *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    out->pc = out->reason_addr = -1;
    if (!d) {
        snprintf(out->reason, sizeof out->reason, "running");
        return;
    }
    pthread_mutex_lock(&d->mtx);
    out->paused = d->paused;
    out->serial = d->stop_serial;
    if (d->paused) {
        out->pc = d->stop_pc;
        out->reason_addr = d->reason_addr;
        snprintf(out->reason, sizeof out->reason, "%s", d->reason);
    } else {
        snprintf(out->reason, sizeof out->reason, "running");
    }
    pthread_mutex_unlock(&d->mtx);
}

int pmd85debug_stop_reason(pmd85debug *d, char *dst, int dstsz, int *address)
{
    pmd85debug_stop st;
    pmd85debug_stop_snapshot(d, &st);
    if (address)
        *address = st.reason_addr;
    if (!dst || dstsz <= 0)
        return 0;
    return snprintf(dst, (size_t)dstsz, "%s", st.reason);
}

unsigned pmd85debug_generation(pmd85debug *d)
{
    return d ? atomic_load(&d->generation) : 0;
}

/* ---- registers ----------------------------------------------------------- */

int pmd85debug_regs_get(pmd85debug *d, pmd85debug_regs *out)
{
    int ok;
    if (!d || !out)
        return 0;
    pthread_mutex_lock(&d->mtx);
    ok = d->paused && d->regs_valid;
    if (ok)
        *out = d->regs;
    pthread_mutex_unlock(&d->mtx);
    return ok;
}

static int push_regs_locked(pmd85debug *d)
{
    pmd85host_cpu c;
    const pmd85debug_regs *r = &d->regs;

    if (!pmd85host_dbg_cpu(&c))
        return 0;
    c.pc = r->pc;
    c.sp = r->sp;
    c.af = (uint16_t)(r->a << 8 | r->f);
    c.bc = (uint16_t)(r->b << 8 | r->c);
    c.de = (uint16_t)(r->d << 8 | r->e);
    c.hl = (uint16_t)(r->h << 8 | r->l);
    return pmd85host_dbg_cpu_set(&c);
}

int pmd85debug_regs_set(pmd85debug *d, int reg, unsigned v)
{
    pmd85debug_regs *r;
    int ok = 1;

    if (!d)
        return 0;
    pthread_mutex_lock(&d->mtx);
    if (!d->paused || !d->regs_valid) {
        pthread_mutex_unlock(&d->mtx);
        return 0;
    }
    r = &d->regs;
    switch (reg) {
    case PMD85DEBUG_REG_PC:  r->pc = (uint16_t)v; break;
    case PMD85DEBUG_REG_SP:  r->sp = (uint16_t)v; break;
    case PMD85DEBUG_REG_A:   r->a = (uint8_t)v; break;
    case PMD85DEBUG_REG_F:   r->f = (uint8_t)v; break;
    case PMD85DEBUG_REG_B:   r->b = (uint8_t)v; break;
    case PMD85DEBUG_REG_C:   r->c = (uint8_t)v; break;
    case PMD85DEBUG_REG_D:   r->d = (uint8_t)v; break;
    case PMD85DEBUG_REG_E:   r->e = (uint8_t)v; break;
    case PMD85DEBUG_REG_H:   r->h = (uint8_t)v; break;
    case PMD85DEBUG_REG_L:   r->l = (uint8_t)v; break;
    case PMD85DEBUG_REG_BC:  r->b = (uint8_t)(v >> 8); r->c = (uint8_t)v; break;
    case PMD85DEBUG_REG_DE:  r->d = (uint8_t)(v >> 8); r->e = (uint8_t)v; break;
    case PMD85DEBUG_REG_HL:  r->h = (uint8_t)(v >> 8); r->l = (uint8_t)v; break;
    case PMD85DEBUG_REG_PSW: r->a = (uint8_t)(v >> 8); r->f = (uint8_t)v; break;
    default: ok = 0; break;
    }
    if (ok) {
        /* the 8080 keeps bit 1 set and bits 3 and 5 clear in F */
        r->f = (uint8_t)((r->f & 0xD7) | 0x02);
        ok = push_regs_locked(d);
        d->stop_pc = d->regs.pc;
    }
    pthread_mutex_unlock(&d->mtx);
    if (ok)
        bump(d);
    return ok;
}

int pmd85debug_flag_set(pmd85debug *d, unsigned mask, int on)
{
    pmd85debug_regs r;
    if (!pmd85debug_regs_get(d, &r))
        return 0;
    return pmd85debug_regs_set(d, PMD85DEBUG_REG_F,
                               on ? (r.f | mask) : (r.f & ~mask));
}

/* ---- memory ------------------------------------------------------------- */

uint32_t pmd85debug_space_size(pmd85debug *d, int space)
{
    (void)d;
    return space == PMD85DEBUG_SPACE_CPU ? SPACE_SIZE : 0;
}

static int parked(pmd85debug *d)
{
    int p;
    pthread_mutex_lock(&d->mtx);
    p = d->paused;
    pthread_mutex_unlock(&d->mtx);
    return p;
}

int pmd85debug_mem_read(pmd85debug *d, int space, uint32_t addr, uint8_t *dst, int n)
{
    int i;
    if (!d || !dst || n <= 0 || space != PMD85DEBUG_SPACE_CPU || addr >= SPACE_SIZE ||
        !parked(d))
        return 0;
    if (addr + (uint32_t)n > SPACE_SIZE)
        n = (int)(SPACE_SIZE - addr);
    for (i = 0; i < n; i++)
        dst[i] = peek((uint16_t)(addr + (uint32_t)i));
    return n;
}

int pmd85debug_mem_write(pmd85debug *d, int space, uint32_t addr, const uint8_t *src, int n)
{
    int i, stored = 0;
    if (!d || !src || n <= 0 || space != PMD85DEBUG_SPACE_CPU || addr >= SPACE_SIZE ||
        !parked(d))
        return 0;
    for (i = 0; i < n && addr + (uint32_t)i < SPACE_SIZE; i++)
        stored += pmd85host_dbg_poke((uint16_t)(addr + (uint32_t)i), src[i]);
    if (stored)
        bump(d);
    return stored;
}

int pmd85debug_read(pmd85debug *d, uint16_t addr, uint8_t *dst, int n)
{
    return pmd85debug_mem_read(d, PMD85DEBUG_SPACE_CPU, addr, dst, n);
}

static void region_kind(int st, int *kind, int *writable, char *name, size_t namesz)
{
    const int r = st & PMD85HOST_MEM_R, w = st & PMD85HOST_MEM_W;
    *writable = w != 0;
    if (st & PMD85HOST_MEM_ROM) {
        *kind = PMD85DEBUG_REGION_ROM;
        *writable = 0;   /* a poke would not show: it lands in the RAM below */
        snprintf(name, namesz, "%s", w ? "ROM (writes go to RAM below)" : "ROM");
    } else if (r && w) {
        *kind = (st & PMD85HOST_MEM_VRAM) ? PMD85DEBUG_REGION_VRAM : PMD85DEBUG_REGION_RAM;
        snprintf(name, namesz, "%s", (st & PMD85HOST_MEM_VRAM) ? "VRAM" : "RAM");
    } else if (r) {
        *kind = PMD85DEBUG_REGION_ROM;
        snprintf(name, namesz, "ROM");
    } else if (w) {
        *kind = PMD85DEBUG_REGION_WRITE_ONLY;
        snprintf(name, namesz, "RAM under ROM (write only)");
    } else {
        *kind = PMD85DEBUG_REGION_UNMAPPED;
        snprintf(name, namesz, "unmapped");
    }
}

int pmd85debug_memory_map(pmd85debug *d, pmd85debug_region *out, int max)
{
    int n;
    if (!d || !out || max <= 0)
        return 0;
    pthread_mutex_lock(&d->mtx);
    if (d->paused && !d->nmap) {
        /* in 256-byte steps: the memory models map in 1K blocks and more;
         * the VRAM-aside bit (per 64-byte line) is left out of the kind */
        uint32_t a;
        for (a = 0; a < SPACE_SIZE; a += 256) {
            int st = pmd85host_dbg_mem_state((uint16_t)a), kind, wr;
            char name[32];
            region_kind(st, &kind, &wr, name, sizeof name);
            if (d->nmap && d->map[d->nmap - 1].kind == kind &&
                d->map[d->nmap - 1].writable == wr) {
                d->map[d->nmap - 1].hi = (uint16_t)(a + 255);
                continue;
            }
            if (d->nmap >= MAP_MAX)
                break;
            d->map[d->nmap].lo = (uint16_t)a;
            d->map[d->nmap].hi = (uint16_t)(a + 255);
            d->map[d->nmap].kind = kind;
            d->map[d->nmap].writable = wr;
            snprintf(d->map[d->nmap].name, sizeof d->map[d->nmap].name, "%s", name);
            d->nmap++;
        }
    }
    n = d->nmap < max ? d->nmap : max;
    memcpy(out, d->map, (size_t)n * sizeof *out);
    pthread_mutex_unlock(&d->mtx);
    return n;
}

static unsigned fold(unsigned ch, int nocase)
{
    return nocase ? (unsigned)tolower((int)ch) : ch;
}

static int match_at(const uint8_t *mem, uint32_t at, const uint8_t *pat, int patlen,
                    uint8_t mask, int nocase)
{
    int i;
    for (i = 0; i < patlen; i++) {
        uint32_t a = at + (uint32_t)i;
        if (a >= SPACE_SIZE)
            return 0;
        if ((fold(mem[a], nocase) & mask) != (fold(pat[i], nocase) & mask))
            return 0;
    }
    return 1;
}

static uint8_t *snapshot_mem(pmd85debug *d)
{
    uint8_t *mem = malloc(SPACE_SIZE);
    if (mem && pmd85debug_mem_read(d, PMD85DEBUG_SPACE_CPU, 0, mem, (int)SPACE_SIZE) !=
               (int)SPACE_SIZE) {
        free(mem);
        return NULL;
    }
    return mem;
}

int pmd85debug_search(pmd85debug *d, int space, const uint8_t *pat, int patlen,
                      uint8_t mask, int flags, uint32_t from, uint32_t to,
                      uint32_t *out, int max)
{
    uint8_t *mem;
    uint32_t a;
    int n = 0;

    if (!d || !pat || patlen <= 0 || space != PMD85DEBUG_SPACE_CPU)
        return -1;
    mem = snapshot_mem(d);
    if (!mem)
        return -1;
    if (to >= SPACE_SIZE)
        to = SPACE_SIZE - 1;
    for (a = from; a <= to && n < max; a++)
        if (match_at(mem, a, pat, patlen, mask, flags & PMD85DEBUG_SEARCH_NOCASE))
            out[n++] = a;
    free(mem);
    return n;
}

int pmd85debug_search_next(pmd85debug *d, int space, const uint8_t *pat, int patlen,
                           uint8_t mask, int flags, uint32_t from, int backward,
                           uint32_t *found)
{
    uint8_t *mem;
    uint32_t i;

    if (!d || !pat || patlen <= 0 || space != PMD85DEBUG_SPACE_CPU)
        return -1;
    mem = snapshot_mem(d);
    if (!mem)
        return -1;
    for (i = 1; i <= SPACE_SIZE; i++) {
        uint32_t a = backward ? (from - i) & 0xFFFF : (from + i) & 0xFFFF;
        if (match_at(mem, a, pat, patlen, mask, flags & PMD85DEBUG_SEARCH_NOCASE)) {
            *found = a;
            free(mem);
            return 1;
        }
    }
    free(mem);
    return 0;
}

/* ---- disassembly ---------------------------------------------------------- */

static uint8_t dasm_read(uint16_t addr, void *user)
{
    (void)user;
    return peek(addr);
}

void pmd85debug_set_syntax(pmd85debug *d, i8080_syntax syntax)
{
    if (!d)
        return;
    syntax = syntax == I8080_SYNTAX_Z80 ? I8080_SYNTAX_Z80 : I8080_SYNTAX_INTEL;
    atomic_store(&d->syntax, (int)syntax);
    pmd85session_set_int(d->s, "debugger_syntax", (int)syntax);
    bump(d);
}

void pmd85debug_set_hex_style(pmd85debug *d, i8080_hex style)
{
    if (!d)
        return;
    if ((unsigned)style >= I8080_HEX_COUNT)
        style = I8080_HEX_H;
    atomic_store(&d->hex, (int)style);
    pmd85session_set_int(d->s, "debugger_hex", (int)style);
    bump(d);
}

i8080_hex pmd85debug_hex_style(pmd85debug *d)
{
    return d ? (i8080_hex)atomic_load(&d->hex) : I8080_HEX_H;
}

int pmd85debug_format_hex(pmd85debug *d, unsigned value, int digits, char *dst, int dstsz)
{
    return i8080_hex_format(dst, dstsz, value, digits, pmd85debug_hex_style(d));
}

i8080_syntax pmd85debug_syntax(pmd85debug *d)
{
    return d ? (i8080_syntax)atomic_load(&d->syntax) : I8080_SYNTAX_INTEL;
}

/* The engine's own decode, always Intel and against live memory (parked). */
static int decode_at(pmd85debug *d, uint16_t addr, i8080_line *l, i8080_syntax syn)
{
    return i8080_dasm_one(addr, dasm_read, NULL, syn,
                          d ? (i8080_hex)atomic_load(&d->hex) : I8080_HEX_H, l);
}

int pmd85debug_disassemble_ex(pmd85debug *d, uint16_t addr, pmd85debug_line *out,
                              int max, int *pc_line)
{
    pmd85debug_regs r;
    i8080_syntax syn;
    int n = 0, i;

    if (pc_line)
        *pc_line = -1;
    if (!d || !out || max <= 0 || !pmd85debug_regs_get(d, &r))
        return 0;
    syn = pmd85debug_syntax(d);
    for (n = 0; n < max; n++) {
        pmd85debug_line *o = &out[n];
        i8080_line l;
        int pos;

        memset(o, 0, sizeof *o);
        decode_at(d, addr, &l, syn);
        o->address = addr;
        o->is_pc = addr == r.pc;
        if (o->is_pc && pc_line)
            *pc_line = n;
        o->length = l.len;
        for (pos = 0, i = 0; i < l.len; i++)
            pos += snprintf(o->bytes + pos, sizeof o->bytes - (size_t)pos, i ? " %02X" : "%02X",
                            l.bytes[i]);
        snprintf(o->disasm, sizeof o->disasm, "%s", l.text);
        o->flags = l.flags;
        o->target = l.target;
        o->port = l.port;
        pmd85symtab_lookup_addr(d->symtab, addr, o->label, sizeof o->label);
        if (l.flags & I8080_BRANCH) {
            pmd85symtab_lookup_addr(d->symtab, l.target, o->comment, sizeof o->comment);
        } else if (l.flags & (I8080_IN | I8080_OUT)) {
            const char *name = pmd85host_port_name((uint8_t)l.port);
            if (name)
                snprintf(o->comment, sizeof o->comment, "%s", name);
        } else if (l.len == 3) {
            /* LXI/LDA/STA/SHLD/LHLD: a data address that has a name */
            pmd85symtab_lookup_addr(d->symtab, (uint16_t)(l.bytes[1] | l.bytes[2] << 8),
                                    o->comment, sizeof o->comment);
        }
        pthread_mutex_lock(&d->mtx);
        if (d->map_exec[addr]) {
            o->has_breakpoint = 1;
        } else {
            int b;
            for (b = 0; b < d->nbp; b++)
                if ((d->bp[b].type & PMD85DEBUG_BP_EXEC) && !d->bp[b].enabled &&
                    addr >= d->bp[b].start && addr <= d->bp[b].end)
                    o->bp_disabled = 1;
        }
        pthread_mutex_unlock(&d->mtx);
        addr = (uint16_t)(addr + l.len);
    }
    return n;
}

int pmd85debug_row_address(pmd85debug *d, uint16_t addr, int rows)
{
    i8080_line l;
    pmd85debug_regs r;

    if (!d || !pmd85debug_regs_get(d, &r))
        return 0;
    if (rows >= 0) {
        while (rows-- > 0) {
            decode_at(d, addr, &l, I8080_SYNTAX_INTEL);
            addr = (uint16_t)(addr + l.len);
        }
        return addr;
    }
    {
        /* Backwards: decode forward from a few candidate starts and keep
         * the one whose instruction boundaries land exactly on addr. */
        const int back = -rows;
        uint16_t starts[512];
        int best = -1, slack;

        for (slack = 0; slack < 3 && best < 0; slack++) {
            uint16_t a = (uint16_t)(addr - back * 3 - slack);
            int n = 0;
            while (n < 512) {
                uint16_t dist = (uint16_t)(addr - a);
                if (dist == 0 || dist > (uint16_t)(back * 3 + 3))
                    break;
                starts[n++] = a;
                decode_at(d, a, &l, I8080_SYNTAX_INTEL);
                a = (uint16_t)(a + l.len);
            }
            if (a == addr && n >= back)
                best = starts[n - back];
        }
        return best >= 0 ? best : (uint16_t)(addr + rows);
    }
}

/* ---- breakpoints ---------------------------------------------------------- */

int pmd85debug_breakpoint_add(pmd85debug *d, int type, uint32_t start, uint32_t end,
                              const char *condition)
{
    pmd85debug_breakpoint *bp;
    char err[96];
    const int io = type & PMD85DEBUG_BP_IO;
    const int mem = type & (PMD85DEBUG_BP_EXEC | PMD85DEBUG_BP_ACCESS);
    const int exact = type & PMD85DEBUG_BP_EXACT;
    int id;

    if (!d || (!io && !mem) || (io && mem) || (exact && !io))
        return -1;
    if (condition && *condition && expr_eval(condition, NULL, NULL, err, sizeof(err)) != 0)
        return -1;
    if (end < start) {
        uint32_t swap = start;
        start = end;
        end = swap;
    }
    if (end >= (io ? 256u : SPACE_SIZE))
        return -1;
    pthread_mutex_lock(&d->mtx);
    if (d->nbp >= MAX_BP) {
        pthread_mutex_unlock(&d->mtx);
        return -1;
    }
    bp = &d->bp[d->nbp++];
    memset(bp, 0, sizeof(*bp));
    bp->id = id = d->next_id++;
    bp->type = io ? io | exact : mem;
    bp->start = start;
    bp->end = end;
    bp->enabled = 1;
    snprintf(bp->condition, sizeof(bp->condition), "%s", condition ? condition : "");
    rebuild_locked(d);
    pthread_mutex_unlock(&d->mtx);
    bump(d);
    return id;
}

void pmd85debug_breakpoint_remove(pmd85debug *d, int id)
{
    int pos;
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    for (pos = 0; pos < d->nbp; pos++)
        if (d->bp[pos].id == id) {
            memmove(&d->bp[pos], &d->bp[pos + 1],
                    (size_t)(d->nbp - pos - 1) * sizeof(d->bp[0]));
            d->nbp--;
            break;
        }
    rebuild_locked(d);
    pthread_mutex_unlock(&d->mtx);
    bump(d);
}

void pmd85debug_breakpoint_enable(pmd85debug *d, int id, int enabled)
{
    int pos;
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    for (pos = 0; pos < d->nbp; pos++)
        if (d->bp[pos].id == id)
            d->bp[pos].enabled = enabled ? 1 : 0;
    rebuild_locked(d);
    pthread_mutex_unlock(&d->mtx);
    bump(d);
}

int pmd85debug_breakpoints_list(pmd85debug *d, pmd85debug_breakpoint *out, int max)
{
    int n;
    if (!d || !out)
        return 0;
    pthread_mutex_lock(&d->mtx);
    n = d->nbp < max ? d->nbp : max;
    memcpy(out, d->bp, (size_t)n * sizeof(*out));
    pthread_mutex_unlock(&d->mtx);
    return n;
}

void pmd85debug_breakpoint_clear_hits(pmd85debug *d)
{
    int pos;
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    for (pos = 0; pos < d->nbp; pos++)
        d->bp[pos].hits = 0;
    pthread_mutex_unlock(&d->mtx);
    bump(d);
}

/* The plain exec breakpoint at exactly addr, or -1. Caller holds mtx. */
static int find_plain_exec_locked(pmd85debug *d, uint16_t addr)
{
    int pos;
    for (pos = 0; pos < d->nbp; pos++)
        if (d->bp[pos].type == PMD85DEBUG_BP_EXEC && d->bp[pos].start == addr &&
            d->bp[pos].end == addr)
            return pos;
    return -1;
}

int pmd85debug_breakpoint_toggle(pmd85debug *d, uint16_t addr)
{
    int pos, id;
    if (!d)
        return -1;
    pthread_mutex_lock(&d->mtx);
    pos = find_plain_exec_locked(d, addr);
    id = pos >= 0 ? d->bp[pos].id : -1;
    pthread_mutex_unlock(&d->mtx);
    if (id >= 0) {
        pmd85debug_breakpoint_remove(d, id);
        return 0;
    }
    return pmd85debug_breakpoint_add(d, PMD85DEBUG_BP_EXEC, addr, addr, NULL) >= 0 ? 1 : -1;
}

int pmd85debug_breakpoint_is_set(pmd85debug *d, uint16_t addr)
{
    int set;
    if (!d)
        return 0;
    pthread_mutex_lock(&d->mtx);
    set = d->map_exec[addr] != 0;
    pthread_mutex_unlock(&d->mtx);
    return set;
}

void pmd85debug_breakpoint_clear_all(pmd85debug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->mtx);
    d->nbp = 0;
    rebuild_locked(d);
    pthread_mutex_unlock(&d->mtx);
    bump(d);
}

static int cmp_u16(const void *a, const void *b)
{
    return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b;
}

int pmd85debug_breakpoint_list(pmd85debug *d, uint16_t *out, int max)
{
    int pos, n = 0;
    if (!d || !out)
        return 0;
    pthread_mutex_lock(&d->mtx);
    for (pos = 0; pos < d->nbp && n < max; pos++)
        if ((d->bp[pos].type & PMD85DEBUG_BP_EXEC) && d->bp[pos].enabled)
            out[n++] = (uint16_t)d->bp[pos].start;
    pthread_mutex_unlock(&d->mtx);
    qsort(out, (size_t)n, sizeof *out, cmp_u16);
    return n;
}

/* ---- symbols ---------------------------------------------------------------- */

int pmd85debug_symbols_load(pmd85debug *d, const char *path)
{
    int n = d ? pmd85symtab_load(d->symtab, path) : -1;
    if (n > 0)
        bump(d);
    return n;
}

void pmd85debug_symbols_clear(pmd85debug *d)
{
    if (!d)
        return;
    pmd85symtab_clear_user(d->symtab);
    bump(d);
}

int pmd85debug_symbol_at(pmd85debug *d, uint16_t addr, char *dst, int dstsz)
{
    return d ? pmd85symtab_lookup_addr(d->symtab, addr, dst, dstsz) : 0;
}

int pmd85debug_symbol_find(pmd85debug *d, const char *name, uint16_t *addr_out)
{
    return d ? pmd85symtab_lookup_name(d->symtab, name, addr_out) : 0;
}

int pmd85debug_symbol_add(pmd85debug *d, uint16_t addr, const char *name)
{
    int ok = d ? pmd85symtab_add(d->symtab, addr, name) : 0;
    if (ok)
        bump(d);
    return ok;
}

int pmd85debug_symbols_count(pmd85debug *d)
{
    return d ? pmd85symtab_count(d->symtab) : 0;
}

int pmd85debug_symbols_get(pmd85debug *d, int index, uint16_t *addr, char *name, int namesz)
{
    return d ? pmd85symtab_get(d->symtab, index, addr, name, namesz, NULL) : 0;
}

typedef struct {
    int index;
    uint16_t addr;
    char name[48];
} sym_pick;

static int sym_pick_cmp(const void *left, const void *right)
{
    const sym_pick *a = left, *b = right;
    if (a->addr != b->addr)
        return (int)a->addr - (int)b->addr;
    return strcmp(a->name, b->name);
}

static int contains_nocase(const char *hay, const char *needle)
{
    size_t n = strlen(needle), i;
    if (!n)
        return 1;
    for (; *hay; hay++) {
        for (i = 0; i < n && hay[i] && tolower((unsigned char)hay[i]) ==
                                           tolower((unsigned char)needle[i]); i++) ;
        if (i == n)
            return 1;
    }
    return 0;
}

int pmd85debug_symbols_filter(pmd85debug *d, const char *text, int *out, int max)
{
    int count, pos, n = 0;
    sym_pick *picks;

    if (!d || !out || max <= 0)
        return 0;
    count = pmd85symtab_count(d->symtab);
    picks = calloc((size_t)(count ? count : 1), sizeof *picks);
    if (!picks)
        return 0;
    for (pos = 0; pos < count; pos++) {
        sym_pick p;
        p.index = pos;
        if (pmd85symtab_get(d->symtab, pos, &p.addr, p.name, sizeof p.name, NULL) &&
            contains_nocase(p.name, text ? text : ""))
            picks[n++] = p;
    }
    qsort(picks, (size_t)n, sizeof *picks, sym_pick_cmp);
    if (n > max)
        n = max;
    for (pos = 0; pos < n; pos++)
        out[pos] = picks[pos].index;
    free(picks);
    return n;
}

/* ---- the trace ------------------------------------------------------------- */

void pmd85debug_trace_enable(pmd85debug *d, int on)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->ring_mtx);
    if (on && !d->ring)
        d->ring = calloc(TRACE_N, sizeof *d->ring);
    pthread_mutex_unlock(&d->ring_mtx);
    atomic_store(&d->trace_on, on && d->ring ? 1 : 0);
    bump(d);
}

int pmd85debug_trace_enabled(pmd85debug *d)
{
    return d ? atomic_load(&d->trace_on) : 0;
}

void pmd85debug_trace_clear(pmd85debug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->ring_mtx);
    d->ring_head = d->ring_count = 0;
    pthread_mutex_unlock(&d->ring_mtx);
    bump(d);
}

int pmd85debug_trace_count(pmd85debug *d)
{
    int n;
    if (!d)
        return 0;
    pthread_mutex_lock(&d->ring_mtx);
    n = d->ring_count;
    pthread_mutex_unlock(&d->ring_mtx);
    return n;
}

int pmd85debug_trace_read(pmd85debug *d, pmd85debug_trace_entry *out, int max)
{
    int n = 0;
    if (!d || !out)
        return 0;
    pthread_mutex_lock(&d->ring_mtx);
    while (n < max && n < d->ring_count) {
        int idx = (d->ring_head - 1 - n + TRACE_N) % TRACE_N;
        out[n++] = d->ring[idx];
    }
    pthread_mutex_unlock(&d->ring_mtx);
    return n;
}

static uint8_t entry_read(uint16_t addr, void *user)
{
    const pmd85debug_trace_entry *e = user;
    uint16_t off = (uint16_t)(addr - e->pc);
    return off < 3 ? e->bytes[off] : 0;
}

int pmd85debug_trace_format(pmd85debug *d, const pmd85debug_trace_entry *e, char *dst,
                            int dstsz)
{
    i8080_line l;
    int n;
    if (!e || !dst || dstsz <= 0)
        return 0;
    i8080_dasm_one(e->pc, entry_read, (void *)e, pmd85debug_syntax(d),
                   d ? (i8080_hex)atomic_load(&d->hex) : I8080_HEX_H, &l);
    n = snprintf(dst, (size_t)dstsz,
                 "%04X  %-18s A=%02X F=%02X BC=%02X%02X DE=%02X%02X HL=%02X%02X SP=%04X",
                 e->pc, l.text, e->a, e->f, e->b, e->c, e->d, e->e, e->h, e->l, e->sp);
    return n < dstsz ? n : dstsz - 1;
}

int pmd85debug_trace_match(const pmd85debug_trace_entry *e, int filter, uint16_t lo,
                           uint16_t hi)
{
    i8080_line l;
    if (!e)
        return 0;
    switch (filter) {
    case PMD85DEBUG_TRACE_BRANCHES:
        i8080_dasm_one(e->pc, entry_read, (void *)e, I8080_SYNTAX_INTEL, I8080_HEX_H, &l);
        return (l.flags & (I8080_JUMP | I8080_CALL | I8080_RET)) != 0;
    case PMD85DEBUG_TRACE_IO:
        return e->bytes[0] == 0xD3 || e->bytes[0] == 0xDB;
    case PMD85DEBUG_TRACE_RANGE:
        return e->pc >= lo && e->pc <= hi;
    default:
        return 1;
    }
}

/* ---- the I/O log and the chips ------------------------------------------- */

void pmd85debug_io_log_enable(pmd85debug *d, int on)
{
    if (!d)
        return;
    atomic_store(&d->io_log_on, on ? 1 : 0);
    bump(d);
}

int pmd85debug_io_log_enabled(pmd85debug *d)
{
    return d ? atomic_load(&d->io_log_on) : 0;
}

int pmd85debug_io_log(pmd85debug *d, pmd85debug_io_access *out, int max)
{
    int n = 0;
    if (!d || !out)
        return 0;
    pthread_mutex_lock(&d->ring_mtx);
    while (d->iolog && n < max && n < d->io_count) {
        int idx = (d->io_head - 1 - n + IOLOG_N) % IOLOG_N;
        out[n++] = d->iolog[idx];
    }
    pthread_mutex_unlock(&d->ring_mtx);
    return n;
}

void pmd85debug_io_log_set_ports(pmd85debug *d, const uint8_t *ports)
{
    int index;
    if (!d)
        return;
    pthread_mutex_lock(&d->ring_mtx);
    for (index = 0; index < 256; index++)
        d->io_cap[index] = (uint8_t)(ports ? ports[index] != 0 : 1);
    pthread_mutex_unlock(&d->ring_mtx);
}

void pmd85debug_io_log_clear(pmd85debug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->ring_mtx);
    d->io_head = d->io_count = 0;
    pthread_mutex_unlock(&d->ring_mtx);
    bump(d);
}

/* From the decode the engine last saw (at an IN/OUT, or a stop), so it
 * answers while the machine runs too. */
const char *pmd85debug_port_name(pmd85debug *d, uint8_t port)
{
    const char *name;
    if (!d)
        return NULL;
    pthread_mutex_lock(&d->mtx);
    name = d->io_name[port];
    pthread_mutex_unlock(&d->mtx);
    return name;
}

int pmd85debug_port_devices(pmd85debug *d, pmd85debug_port_device *out, int max)
{
    int i, n = 0;
    unsigned port;
    if (!d || !out || max <= 0)
        return 0;
    pthread_mutex_lock(&d->mtx);
    for (i = 0; i < d->n_io_devs && n < max; i++, n++) {
        const pmd85host_port_dev *dev = &d->io_devs[i];
        pmd85debug_port_device *o = &out[n];
        memset(o, 0, sizeof *o);
        snprintf(o->name, sizeof o->name, "%s", dev->name);
        o->mask = dev->mask;
        o->adr = dev->adr;
        /* as the PMD's own documentation lists them: the bits nothing
         * decodes set (the system PIO is F4h-F7h, BASIC writes 86h) */
        o->first = (uint8_t)(dev->adr | (~dev->mask & ~dev->reg & 0xFF));
        o->last = (uint8_t)(o->first | dev->reg);
        o->registers = 1;
        for (port = 0; port < 256; port++)
            if ((port & dev->reg) == port && port)
                o->registers++;
        for (port = 0; port < 256; port++)
            if (d->io_key[port] >= 0 && d->io_key[port] / 256 == i)
                o->ports++;
    }
    pthread_mutex_unlock(&d->mtx);
    return n;
}

int pmd85debug_port_mirrors(pmd85debug *d, uint8_t port, uint8_t hit[256])
{
    int n = 0;
    unsigned other;
    if (!d || !hit)
        return 0;
    pthread_mutex_lock(&d->mtx);
    for (other = 0; other < 256; other++)
        if (other == port || (d->io_key[port] >= 0 && d->io_key[other] == d->io_key[port])) {
            hit[other] = 1;
            n++;
        }
    pthread_mutex_unlock(&d->mtx);
    return n;
}

int pmd85debug_io_get(pmd85debug *d, pmd85debug_io *out)
{
    if (!out)
        return 0;
    memset(out, 0, sizeof *out);
    if (!d || !parked(d))
        return 0;
    return pmd85host_dbg_io(out);
}

/* ---- FujiNet --------------------------------------------------------------- */

int pmd85debug_fujinet_trace(pmd85debug *d, pmd85_fuji_packet *out, int max)
{
    int n;
    if (!d)
        return 0;
    pthread_mutex_lock(&d->trace_mtx);
    n = dwtrace_get(d->trace, out, max);
    pthread_mutex_unlock(&d->trace_mtx);
    return n;
}

void pmd85debug_fujinet_trace_clear(pmd85debug *d)
{
    if (!d)
        return;
    pthread_mutex_lock(&d->trace_mtx);
    dwtrace_clear(d->trace);
    pthread_mutex_unlock(&d->trace_mtx);
}

int pmd85debug_fujinet_is_ready(const pmd85_fuji_packet *p)
{
    return p && p->device == 0xE2 && p->command == 0x00;
}

int pmd85debug_fujinet_state(pmd85debug *d, char *dst, int dstsz)
{
    unsigned long long out, in, ex;
    char pending[96];
    int n;

    if (!d || !dst || dstsz <= 0)
        return 0;
    pthread_mutex_lock(&d->trace_mtx);
    out = dwtrace_bytes(d->trace, 1);
    in = dwtrace_bytes(d->trace, 0);
    ex = dwtrace_exchanges(d->trace);
    dwtrace_pending(d->trace, pending, sizeof(pending));
    pthread_mutex_unlock(&d->trace_mtx);

    n = snprintf(dst, (size_t)dstsz,
                 "Becker port     %s\n"
                 "FujiNet         %s\n"
                 "Link            %s\n"
                 "Unread          %d B\n"
                 "\n"
                 "PMD \xe2\x86\x92 FujiNet   %llu B\n"
                 "FujiNet \xe2\x86\x92 PMD   %llu B\n"
                 "Exchanges       %llu\n"
                 "ROM downloads   %d\n"
                 "\n"
                 "In progress     %s\n",
                 pmd85session_becker_endpoint(d->s),
                 d->s->fujinet_external ? "external" :
                 d->s->fujinet_running ? "listening" : "off",
                 pmd85host_becker_link_up() ? "connected" : "not connected",
                 pmd85host_becker_rx_pending(), out, in, ex,
                 pmd85host_becker_rom_loads(NULL), pending);
    return n < dstsz ? n : dstsz - 1;
}
