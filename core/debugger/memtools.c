/*
 * memtools -- see core/include/pmd85memtools.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pmd85memtools.h"
#include "debugger_internal.h"
#include "expr.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- range parsing ------------------------------------------------------------ */

static int hexdigit(char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

static long space_mask(int space)
{
    (void)space;
    return 0xFFFFL;
}

static int none_name(void *user, const char *name, long *out)
{
    (void)user;
    (void)name;
    (void)out;
    return 0;
}

static int eval_addr(pmd85debug *d, int space, const char *text, size_t len, long *out)
{
    char buf[128], err[64];
    long value;
    uint16_t found;

    while (len && isspace((unsigned char)text[0])) { text++; len--; }
    while (len && isspace((unsigned char)text[len - 1])) len--;
    if (!len || len >= sizeof buf) return 0;
    memcpy(buf, text, len);
    buf[len] = '\0';
    /* A bare hex token is hex here (memory fields have always taken
     * "C000"), unless it names a symbol. */
    if (len <= 4u) {
        size_t pos;
        for (pos = 0; pos < len && hexdigit(buf[pos]); pos++) ;
        if (pos == len && !(d && pmd85debug_symbol_find(d, buf, &found))) {
            *out = strtol(buf, NULL, 16) & space_mask(space);
            return 1;
        }
    }
    if (d) {
        if (pmd85debug_eval(d, buf, &value, err, sizeof err) != 0)
            return 0;
    } else {
        expr_ctx none = { none_name, NULL, NULL };
        if (expr_eval(buf, &none, &value, err, sizeof err) != 0)
            return 0;
    }
    *out = value & space_mask(space);
    return 1;
}

/* No operator characters: a number or a symbol. */
static int plain(const char *text, size_t len)
{
    size_t pos;
    for (pos = 0; pos < len; pos++)
        if (strchr("+-*/%&|^<>=!~()[]{}", text[pos])) return 0;
    return 1;
}

int pmd85sym_parse_range(pmd85debug *d, int space, const char *text, uint32_t *lo,
                        uint32_t *hi)
{
    const char *dots, *scan;
    size_t len;
    long first, last;

    if (!text || !lo || !hi) return 0;
    while (isspace((unsigned char)*text)) text++;
    len = strlen(text);
    if (!len) return 0;

    dots = strstr(text, "..");
    if (dots) {
        if (!eval_addr(d, space, text, (size_t)(dots - text), &first) ||
            !eval_addr(d, space, dots + 2, strlen(dots + 2), &last))
            return 0;
    } else {
        int split = 0;
        for (scan = text + 1; *scan; scan++) {
            if (*scan != '-') continue;
            if (plain(text, (size_t)(scan - text)) && plain(scan + 1, strlen(scan + 1)) &&
                eval_addr(d, space, text, (size_t)(scan - text), &first) &&
                eval_addr(d, space, scan + 1, strlen(scan + 1), &last)) {
                split = 1;
                break;
            }
        }
        if (!split) {
            if (!eval_addr(d, space, text, len, &first)) return 0;
            last = first;
        }
    }
    if (first > last) { long swap = first; first = last; last = swap; }
    *lo = (uint32_t)first;
    *hi = (uint32_t)last;
    return 1;
}

/* ---- watches ------------------------------------------------------------------ */

static struct {
    pthread_mutex_t mtx;
    char text[PMD85WATCH_MAX][96];
    int space[PMD85WATCH_MAX];
    int count;
    unsigned generation;
} g_watch = { .mtx = PTHREAD_MUTEX_INITIALIZER };

int pmd85watch_count(void)
{
    int count;
    pthread_mutex_lock(&g_watch.mtx);
    count = g_watch.count;
    pthread_mutex_unlock(&g_watch.mtx);
    return count;
}

int pmd85watch_add(const char *text, int space)
{
    int index = -1;

    if (!text) return -1;
    while (isspace((unsigned char)*text)) text++;
    if (!*text) return -1;
    pthread_mutex_lock(&g_watch.mtx);
    if (g_watch.count < PMD85WATCH_MAX) {
        size_t len;
        index = g_watch.count++;
        snprintf(g_watch.text[index], sizeof g_watch.text[index], "%s", text);
        (void)space;
        g_watch.space[index] = PMD85DEBUG_SPACE_CPU;
        len = strlen(g_watch.text[index]);
        while (len && isspace((unsigned char)g_watch.text[index][len - 1]))
            g_watch.text[index][--len] = '\0';
        g_watch.generation++;
    }
    pthread_mutex_unlock(&g_watch.mtx);
    return index;
}

void pmd85watch_remove(int index)
{
    pthread_mutex_lock(&g_watch.mtx);
    if (index >= 0 && index < g_watch.count) {
        int tail = g_watch.count - index - 1;
        memmove(g_watch.text[index], g_watch.text[index + 1],
                (size_t)tail * sizeof g_watch.text[0]);
        memmove(&g_watch.space[index], &g_watch.space[index + 1],
                (size_t)tail * sizeof g_watch.space[0]);
        g_watch.count--;
        g_watch.generation++;
    }
    pthread_mutex_unlock(&g_watch.mtx);
}

void pmd85watch_clear(void)
{
    pthread_mutex_lock(&g_watch.mtx);
    g_watch.count = 0;
    g_watch.generation++;
    pthread_mutex_unlock(&g_watch.mtx);
}

/* Valid until the next change; the windows are single-threaded users. */
const char *pmd85watch_text(int index)
{
    const char *text = NULL;
    pthread_mutex_lock(&g_watch.mtx);
    if (index >= 0 && index < g_watch.count) text = g_watch.text[index];
    pthread_mutex_unlock(&g_watch.mtx);
    return text;
}

int pmd85watch_space(int index)
{
    int space = PMD85DEBUG_SPACE_CPU;
    pthread_mutex_lock(&g_watch.mtx);
    if (index >= 0 && index < g_watch.count) space = g_watch.space[index];
    pthread_mutex_unlock(&g_watch.mtx);
    return space;
}

unsigned pmd85watch_generation(void)
{
    unsigned gen;
    pthread_mutex_lock(&g_watch.mtx);
    gen = g_watch.generation;
    pthread_mutex_unlock(&g_watch.mtx);
    return gen;
}

int pmd85watch_range(pmd85debug *d, int index, uint32_t *lo, uint32_t *hi)
{
    char text[96];
    const char *source = pmd85watch_text(index);

    if (!source) return 0;
    snprintf(text, sizeof text, "%s", source);
    return pmd85sym_parse_range(d, pmd85watch_space(index), text, lo, hi);
}

int pmd85watch_format(pmd85debug *d, int index, char *dst, int dstsz)
{
    char text[96];
    uint32_t lo, hi, addr;
    const char *source = pmd85watch_text(index);
    int space = pmd85watch_space(index), written = 0, pos;
    pmd85debug_stop stop;
    const char *prefix = "";
    const int digits = 4;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!source) return 0;
    snprintf(text, sizeof text, "%s", source);
    if (!pmd85sym_parse_range(d, space, text, &lo, &hi))
        return snprintf(dst, (size_t)dstsz, "%-14s ?", text);
    written = snprintf(dst, (size_t)dstsz, "%-14s %s%s", text, prefix, pmd85hx(d, lo, digits));
    if (written >= dstsz) return dstsz - 1;
    pmd85debug_stop_snapshot(d, &stop);
    if (!stop.paused)
        return written + snprintf(dst + written, (size_t)(dstsz - written), "  -");
    if (lo == hi) {
        uint8_t value = 0;
        pmd85debug_mem_read(d, space, lo, &value, 1);
        return written + snprintf(dst + written, (size_t)(dstsz - written),
                                  "  %s (%u)", pmd85hx(d, value, 2), value);
    }
    written += snprintf(dst + written, (size_t)(dstsz - written), "-%s ", pmd85hx(d, hi, digits));
    for (pos = 0; pos < 8 && lo + (uint32_t)pos <= hi && written < dstsz - 4; pos++) {
        uint8_t value = 0;
        addr = lo + (uint32_t)pos;
        pmd85debug_mem_read(d, space, addr, &value, 1);
        written += snprintf(dst + written, (size_t)(dstsz - written), " %02X", value);
    }
    if (hi - lo >= 8 && written < dstsz - 4)
        written += snprintf(dst + written, (size_t)(dstsz - written), " ...");
    return written < dstsz ? written : dstsz - 1;
}

/* ---- search ------------------------------------------------------------------- */

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

int pmd85search_parse(int kind, const char *text, uint8_t *pat, int *n,
                     uint8_t *mask, int *flags)
{
    int count = 0;

    if (!text || !pat || !n || !mask || !flags) return 0;
    *mask = 0xFF;
    *flags = 0;
    if (kind == PMD85SEARCH_TEXT) {
        const char *scan;
        for (scan = text; *scan && count < PMD85SEARCH_MAX; scan++)
            pat[count++] = (uint8_t)*scan;
        *flags = PMD85DEBUG_SEARCH_NOCASE;
    } else {
        const char *scan = text;
        while (*scan) {
            int high, low;
            if (isspace((unsigned char)*scan) || *scan == ',') { scan++; continue; }
            if (*scan == '$' || *scan == '#') { scan++; continue; }
            if (scan[0] == '0' && (scan[1] == 'x' || scan[1] == 'X')) { scan += 2; continue; }
            high = hexval(*scan);
            if (high < 0) return 0;
            low = hexval(scan[1]);
            if (low < 0) { low = high; high = 0; scan += 1; }   /* a lone digit is a byte */
            else scan += 2;
            if (count >= PMD85SEARCH_MAX) break;
            pat[count++] = (uint8_t)(high << 4 | low);
        }
    }
    *n = count;
    return count > 0;
}

/* ---- breakpoint description ----------------------------------------------------- */

int pmd85bp_describe(pmd85debug *d, const pmd85debug_breakpoint *bp, char *dst, int dstsz)
{
    char kind[48] = "", label[48] = "";
    int written;
    const int io = bp && (bp->type & PMD85DEBUG_BP_IO);

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    if (!bp) return 0;
    if (io) {
        snprintf(kind, sizeof kind, "%s",
                 (bp->type & PMD85DEBUG_BP_IO) == PMD85DEBUG_BP_IO ? "In/Out"
                 : (bp->type & PMD85DEBUG_BP_IN) ? "In" : "Out");
        if (d) {
            const char *name = bp->start == bp->end ? pmd85debug_port_name(d, (uint8_t)bp->start) : NULL;
            /* the mirrors it covers too, when the range has any */
            uint8_t hit[256];
            int covered = 0;
            uint32_t port;
            memset(hit, 0, sizeof hit);
            if (!(bp->type & PMD85DEBUG_BP_EXACT))
                for (port = bp->start; port <= bp->end && port < 256; port++)
                    pmd85debug_port_mirrors(d, (uint8_t)port, hit);
            for (port = 0; port < 256; port++)
                covered += hit[port];
            snprintf(label, sizeof label, "%s%s%s", name ? name : "",
                     name && ((bp->type & PMD85DEBUG_BP_EXACT) || covered > (int)(bp->end - bp->start + 1))
                         ? ", " : "",
                     (bp->type & PMD85DEBUG_BP_EXACT) ? "exact"
                     : covered > (int)(bp->end - bp->start + 1) ? "and mirrors" : "");
        }
        if (bp->start == bp->end)
            written = snprintf(dst, (size_t)dstsz, "%s  port %s", kind, pmd85hx(d, bp->start, 2));
        else
            written = snprintf(dst, (size_t)dstsz, "%s  ports %s-%s", kind, pmd85hx(d, bp->start, 2),
                               pmd85hx(d, bp->end, 2));
    } else {
        if (bp->type & PMD85DEBUG_BP_EXEC)
            strncat(kind, "Exec", sizeof kind - strlen(kind) - 1);
        if ((bp->type & PMD85DEBUG_BP_ACCESS) == PMD85DEBUG_BP_ACCESS) {
            if (kind[0]) strncat(kind, "/", sizeof kind - strlen(kind) - 1);
            strncat(kind, "Access", sizeof kind - strlen(kind) - 1);
        } else if (bp->type & PMD85DEBUG_BP_READ) {
            if (kind[0]) strncat(kind, "/", sizeof kind - strlen(kind) - 1);
            strncat(kind, "Read", sizeof kind - strlen(kind) - 1);
        } else if (bp->type & PMD85DEBUG_BP_WRITE) {
            if (kind[0]) strncat(kind, "/", sizeof kind - strlen(kind) - 1);
            strncat(kind, "Write", sizeof kind - strlen(kind) - 1);
        }
        if (d)
            pmd85debug_symbol_at(d, (uint16_t)bp->start, label, sizeof label);
        if (bp->start == bp->end)
            written = snprintf(dst, (size_t)dstsz, "%s  %s", kind, pmd85hx(d, bp->start, 4));
        else
            written = snprintf(dst, (size_t)dstsz, "%s  %s-%s", kind, pmd85hx(d, bp->start, 4),
                               pmd85hx(d, bp->end, 4));
    }
    if (written < dstsz && label[0])
        written += snprintf(dst + written, (size_t)(dstsz - written), " (%s)", label);
    if (written < dstsz && bp->condition[0])
        written += snprintf(dst + written, (size_t)(dstsz - written), "  if %s", bp->condition);
    return written < dstsz ? written : dstsz - 1;
}

/* ---- ports ------------------------------------------------------------------------ */

/* A port number as the filter takes it: 3E, 3Eh, $3E, 0x3E, #3E. */
static int port_number(const char *text, int len, int *out)
{
    char buf[16], *end;
    long v;
    if (len <= 0 || len >= (int)sizeof buf) return 0;
    memcpy(buf, text, (size_t)len);
    buf[len] = '\0';
    text = buf;
    if (*text == '$' || *text == '#') text++;
    else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    if ((buf[len - 1] == 'h' || buf[len - 1] == 'H') && buf + len - 1 > text) buf[len - 1] = '\0';
    if (!*text) return 0;
    v = strtol(text, &end, 16);
    if (*end || v < 0 || v > 255) return 0;
    *out = (int)v;
    return 1;
}

int pmd85portfilter_parse(pmd85debug *d, const char *text, pmd85portfilter *f)
{
    char buf[64];
    const char *dash;
    int lo, hi, exact = 0, port, any = 0;
    size_t len;

    if (!f) return 0;
    memset(f->hit, 0, sizeof f->hit);
    while (text && isspace((unsigned char)*text)) text++;
    snprintf(buf, sizeof buf, "%s", text ? text : "");
    len = strlen(buf);
    while (len && isspace((unsigned char)buf[len - 1])) buf[--len] = '\0';
    if (!len) {
        memset(f->hit, 1, sizeof f->hit);
        return 1;
    }
    text = buf;
    if (*text == '=') {
        exact = 1;
        text++;
        while (isspace((unsigned char)*text)) text++;
    }
    dash = strchr(text + 1, '-');
    if (dash ? port_number(text, (int)(dash - text), &lo) && port_number(dash + 1, (int)strlen(dash + 1), &hi)
             : port_number(text, (int)strlen(text), &lo)) {
        if (!dash) hi = lo;
        if (hi < lo) { port = lo; lo = hi; hi = port; }
        for (port = lo; port <= hi; port++) {
            if (exact || !d) f->hit[port] = 1;
            else pmd85debug_port_mirrors(d, (uint8_t)port, f->hit);
        }
        return 1;
    }
    if (exact || !d) return 0;
    /* the start of a name: "sys pio" is every system PIO register */
    for (port = 0; port < 256; port++) {
        const char *name = pmd85debug_port_name(d, (uint8_t)port);
        if (name && !strncasecmp(name, text, strlen(text))) {
            f->hit[port] = 1;
            any = 1;
        }
    }
    return any;
}

int pmd85ports_describe(pmd85debug *d, char *dst, int dstsz)
{
    pmd85debug_port_device devs[16];
    int n, i, len = 0;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    n = pmd85debug_port_devices(d, devs, 16);
    if (n <= 0)
        return snprintf(dst, (size_t)dstsz, "(not seen yet: stop the machine once)\n") < dstsz ? (int)strlen(dst)
                                                                                         : dstsz - 1;
    for (i = 0; i < n && len < dstsz - 1; i++) {
        char span[24], rule[48];
        const int each = devs[i].registers ? devs[i].ports / devs[i].registers : 0;
        if (devs[i].first == devs[i].last)
            snprintf(span, sizeof span, "%s", pmd85hx(d, devs[i].first, 2));
        else
            snprintf(span, sizeof span, "%s-%s", pmd85hx(d, devs[i].first, 2), pmd85hx(d, devs[i].last, 2));
        snprintf(rule, sizeof rule, "port & %s = %s", pmd85hx(d, devs[i].mask, 2), pmd85hx(d, devs[i].adr, 2));
        len += snprintf(dst + len, (size_t)(dstsz - len), "  %-10s %-10s %-20s %d register%s", devs[i].name,
                        span, rule, devs[i].registers, devs[i].registers == 1 ? "" : "s");
        if (len < dstsz - 1 && each > 1)
            len += snprintf(dst + len, (size_t)(dstsz - len), ", %s at %d ports",
                            devs[i].registers == 1 ? "answering" : "each", each);
        if (len < dstsz - 1)
            len += snprintf(dst + len, (size_t)(dstsz - len), "\n");
    }
    return len < dstsz ? len : dstsz - 1;
}

/* ---- memory map labels ---------------------------------------------------------- */

int pmd85mem_region_name(const pmd85debug_region *map, int count, uint16_t addr,
                        char *dst, int dstsz)
{
    int pos;

    if (!dst || dstsz <= 0) return 0;
    dst[0] = '\0';
    for (pos = 0; map && pos < count; pos++)
        if (addr >= map[pos].lo && addr <= map[pos].hi)
            return snprintf(dst, (size_t)dstsz, "%s", map[pos].name);
    return 0;
}

const char *pmd85mem_space_name(int space)
{
    (void)space;
    return "CPU";
}
