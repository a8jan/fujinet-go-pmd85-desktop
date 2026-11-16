/*
 * prompt.c -- the debugger's command line (pmd85debug_command) and its Tab
 * completion. Every command is a thin layer over the public API, so a
 * window's buttons and the prompt can never disagree. After
 * fujinet-go-coco-desktop's prompt, with the 8080's registers and the PMD's
 * I/O (port breakpoints, the I/O log, the chips) in place of the CoCo's.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "compat.h"
#include "debugger_internal.h"
#include "pmd85memtools.h"

typedef struct {
    char *dst;
    int cap, len;
} outbuf;

static void out(outbuf *buf, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (buf->len >= buf->cap - 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(buf->dst + buf->len, (size_t)(buf->cap - buf->len), fmt, ap);
    va_end(ap);
    if (n > 0)
        buf->len += n < buf->cap - buf->len ? n : buf->cap - buf->len - 1;
}

static const char *const commands[] = {
    "help", "step", "over", "out", "run", "stop", "runto", "frame", "break", "bpr",
    "bpw", "bpa", "bpin", "bpout", "bpio", "delete", "enable", "disable", "clear",
    "breaks", "hits", "print", "regs", "set", "mem", "poke", "disasm", "syntax", "hex",
    "trace", "iolog", "watch", "unwatch", "find", "symbols", "label", "labels",
    "map", "io", "ports", NULL,
};

static const char *help_text =
    "step [n]               run n instructions (F7)\n"
    "over                   step over a CALL/RST (F8)\n"
    "out                    run to the routine's return (Shift+F8)\n"
    "run | stop             go / break\n"
    "runto <addr>           run until PC = addr\n"
    "frame [n]              run n frames (20 ms each), stop at the next\n"
    "break <addr>[-<end>] [cond]       execute breakpoint\n"
    "bpr | bpw | bpa <addr>[-<end>] [cond]   read / write / access breakpoint\n"
    "bpin | bpout | bpio <port>[-<end>] [cond]   IN / OUT / either on a port\n"
    "delete <id> | enable <id> | disable <id> | clear | breaks | hits (clear hit counts)\n"
    "print <expr>           e.g. print hl, print [C000h], print {sp}, print pc+3\n"
    "regs                   registers and flags\n"
    "set <reg> <expr>       e.g. set pc 8C00h, set a 0, set hl de, set zf 1\n"
    "mem <addr> [len]       dump memory (side-effect free)\n"
    "poke <addr> <v...>     write RAM\n"
    "disasm [addr] [n]      | syntax intel|z80 (the mnemonics) | hex h|$|0x|#\n"
    "trace [on|off|clear|n] record / show the last n instructions\n"
    "iolog [on|off|clear|n] the last n IN/OUT accesses (on by default)\n"
    "watch [addr|symbol|lo-hi] | unwatch <n|all>   the watch list\n"
    "find [hex] <bytes> | find text <string>   search memory (stopped)\n"
    "symbols [filter] | label <addr> <name> | labels <file>\n"
    "map                    the CPU memory map as mapped now\n"
    "io | ports             the chips (PIOs, timer, USART, modules, Becker); the port map\n"
    "Expressions: C000h $C000 0xC000 #C000 49152 %1010; registers a b c d e h l f\n"
    "bc de hl sp pc psw; flags sf zf hf pf cf; [addr] byte, {addr} word; symbols;\n"
    "cycles frame tframe; in a condition value, old and addr (port); C operators.\n";

static char *next_tok(char **p)
{
    char *s = *p, *t;
    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s) {
        *p = s;
        return NULL;
    }
    t = s;
    while (*s && !isspace((unsigned char)*s))
        s++;
    if (*s)
        *s++ = '\0';
    *p = s;
    return t;
}

static char *rest_of(char *p)
{
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

static int eval(pmd85debug *d, outbuf *buf, const char *text, long *v)
{
    char err[96];
    if (pmd85debug_eval(d, text, v, err, sizeof err) != 0) {
        out(buf, "error: %s\n", err);
        return -1;
    }
    return 0;
}

/* "<addr>" or "<addr>-<end>" */
static int range(pmd85debug *d, outbuf *buf, char *tok, long *first, long *last)
{
    char *dash = strchr(tok + 1, '-');
    if (dash) {
        *dash = '\0';
        if (eval(d, buf, tok, first) || eval(d, buf, dash + 1, last))
            return -1;
        return 0;
    }
    if (eval(d, buf, tok, first))
        return -1;
    *last = *first;
    return 0;
}

static void show_regs(pmd85debug *d, outbuf *buf)
{
    static const char names[] = "SZ-H-P-C";
    pmd85debug_regs r;
    char flags[9];
    int bit;

    if (!pmd85debug_regs_get(d, &r)) {
        out(buf, "the machine is running (stop first)\n");
        return;
    }
    for (bit = 0; bit < 8; bit++)
        flags[bit] = names[bit] == '-' ? '-' : (r.f & (0x80 >> bit)) ? names[bit] : '.';
    flags[8] = '\0';
    out(buf, "PC=%04X  SP=%04X  A=%02X F=%02X %s  %s%s\n", r.pc, r.sp, r.a, r.f, flags,
        r.iff ? "EI" : "DI", r.halt ? "  HALT" : "");
    out(buf, "BC=%02X%02X  DE=%02X%02X  HL=%02X%02X\n", r.b, r.c, r.d, r.e, r.h, r.l);
}

static void dump(outbuf *buf, uint32_t addr, const uint8_t *bytes, int n)
{
    int row, col;

    for (row = 0; row < n; row += 16) {
        out(buf, "%04X ", addr + (uint32_t)row);
        for (col = 0; col < 16; col++)
            out(buf, col < n - row ? " %02X" : "   ", bytes[row + col]);
        out(buf, "  ");
        for (col = 0; col < 16 && row + col < n; col++)
            out(buf, "%c", (bytes[row + col] >= 0x20 && bytes[row + col] < 0x7f)
                               ? bytes[row + col] : '.');
        out(buf, "\n");
    }
}

static int reg_by_name(const char *name)
{
    static const struct { const char *name; int reg; } map[] = {
        { "pc", PMD85DEBUG_REG_PC }, { "sp", PMD85DEBUG_REG_SP },
        { "a", PMD85DEBUG_REG_A }, { "f", PMD85DEBUG_REG_F }, { "b", PMD85DEBUG_REG_B },
        { "c", PMD85DEBUG_REG_C }, { "d", PMD85DEBUG_REG_D }, { "e", PMD85DEBUG_REG_E },
        { "h", PMD85DEBUG_REG_H }, { "l", PMD85DEBUG_REG_L },
        { "bc", PMD85DEBUG_REG_BC }, { "de", PMD85DEBUG_REG_DE },
        { "hl", PMD85DEBUG_REG_HL }, { "psw", PMD85DEBUG_REG_PSW },
        { "af", PMD85DEBUG_REG_PSW },
    };
    unsigned pos;
    for (pos = 0; pos < sizeof map / sizeof map[0]; pos++)
        if (!strcasecmp(map[pos].name, name))
            return map[pos].reg;
    return -1;
}

static int flag_by_name(const char *name)
{
    static const struct { const char *name; unsigned mask; } map[] = {
        { "sf", PMD85DEBUG_F_S }, { "zf", PMD85DEBUG_F_Z }, { "hf", PMD85DEBUG_F_AC },
        { "pf", PMD85DEBUG_F_P }, { "cf", PMD85DEBUG_F_CY },
    };
    unsigned pos;
    for (pos = 0; pos < sizeof map / sizeof map[0]; pos++)
        if (!strcasecmp(map[pos].name, name))
            return (int)map[pos].mask;
    return 0;
}

static void list_breaks(pmd85debug *d, outbuf *buf)
{
    pmd85debug_breakpoint bps[PMD85DEBUG_MAX_BREAKPOINTS];
    int count = pmd85debug_breakpoints_list(d, bps, PMD85DEBUG_MAX_BREAKPOINTS), pos;

    if (!count) {
        out(buf, "no breakpoints\n");
        return;
    }
    for (pos = 0; pos < count; pos++) {
        char text[200];
        pmd85bp_describe(d, &bps[pos], text, sizeof text);
        out(buf, "#%d %s%s  hits %u\n", bps[pos].id, text,
            bps[pos].enabled ? "" : " (disabled)", bps[pos].hits);
    }
}

static void add_bp(pmd85debug *d, outbuf *buf, int type, char *args)
{
    char *tok = next_tok(&args);
    long first, last;
    int id;

    if (!tok) {
        out(buf, (type & PMD85DEBUG_BP_IO) ? "usage: <port>[-<end>] [condition]\n"
                                           : "usage: <addr>[-<end>] [condition]\n");
        return;
    }
    if (range(d, buf, tok, &first, &last))
        return;
    id = pmd85debug_breakpoint_add(d, type, (uint32_t)first, (uint32_t)last, rest_of(args));
    if (id < 0)
        out(buf, "error: not set (table full, bad range, or the condition does not parse)\n");
    else
        out(buf, "breakpoint #%d\n", id);
}

static void wait_stopped(pmd85debug *d, uint64_t was)
{
    int tries;
    pmd85debug_stop stop;
    for (tries = 0; tries < 2000; tries++) {
        pmd85debug_stop_snapshot(d, &stop);
        if (stop.paused && stop.serial != was)
            return;
        pmd85_sleep_ms(1);
    }
}

static void show_pio(outbuf *buf, const char *name, const pmd85debug_pio *p)
{
    if (!p->valid)
        return;
    out(buf, "%-8s CWR %02X  PA %02X PB %02X PC %02X  in %02X %02X %02X  INTE %X\n", name,
        p->cwr, p->a, p->b, p->c, p->in_a, p->in_b, p->in_c, p->inte);
}

static void show_io(pmd85debug *d, outbuf *buf)
{
    pmd85debug_io io;
    int i;

    if (!pmd85debug_io_get(d, &io)) {
        out(buf, "the machine is running (stop first)\n");
        return;
    }
    out(buf, "%s  memory: %s%s%s  LEDs %02X\n", pmd85session_model_name(io.model),
        io.allram ? "ALLRAM " : "", io.remapped ? "remapped " : "",
        io.in_reset ? "(reset map)" : "", io.led);
    show_pio(buf, "SYS PIO", &io.sys);
    show_pio(buf, "GPIO", &io.gpio);
    show_pio(buf, "ROM PIO", &io.rom);
    for (i = 0; i < 3; i++)
        if (io.pit[i].valid)
            out(buf, "PIT CT%d  CWR %02X  init %04X  count %04X  gate %d out %d%s\n", i,
                io.pit[i].cwr, io.pit[i].init, io.pit[i].value, io.pit[i].gate,
                io.pit[i].out, io.pit[i].counting ? "  counting" : "");
    if (io.usart_valid)
        out(buf, "USART    mode %02X  command %02X  status %02X\n", io.usart_mode,
            io.usart_command, io.usart_status);
    if (io.mm256 >= 0)
        out(buf, "MM256    page %02X\n", io.mm256);
    if (io.mega_page >= 0)
        out(buf, "MEGA     page %d\n", io.mega_page);
    if (io.becker)
        out(buf, "BECKER   link %s, %d byte(s) waiting\n", io.becker_link ? "up" : "down",
            io.becker_rx);
    if (io.mif85 || io.pmd32)
        out(buf, "%s%s\n", io.mif85 ? "MIF 85 fitted  " : "", io.pmd32 ? "PMD 32 fitted" : "");
}

static void show_ports(pmd85debug *d, outbuf *buf)
{
    const char *last = NULL;
    int port, first = -1;

    if (!pmd85debug_is_paused(d)) {
        out(buf, "the machine is running (stop first)\n");
        return;
    }
    /* runs of ports answered by the same thing (the 8255s decode with masks,
     * so each register shows up at several addresses) */
    for (port = 0; port <= 256; port++) {
        const char *name = port < 256 ? pmd85debug_port_name(d, (uint8_t)port) : NULL;
        if (port < 256 && name == last && name)
            continue;
        if (last && first == port - 1)
            out(buf, "  %-9s %s\n", pmd85hx(d, (unsigned)first, 2), last);
        else if (last) {
            char span[24];
            snprintf(span, sizeof span, "%s-%s", pmd85hx(d, (unsigned)first, 2),
                     pmd85hx(d, (unsigned)(port - 1), 2));
            out(buf, "  %-9s %s\n", span, last);
        }
        last = name;
        first = port;
    }
}

int pmd85debug_command(pmd85debug *d, const char *command, char *dst, int dstsz)
{
    char line[512];
    char *p = line, *cmd;
    outbuf buf = { dst, dstsz, 0 };
    long v, w;

    if (!d || !dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    snprintf(line, sizeof line, "%s", command ? command : "");
    cmd = next_tok(&p);
    if (!cmd)
        return 0;
    for (char *ch = cmd; *ch; ch++)
        *ch = (char)tolower((unsigned char)*ch);

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        out(&buf, "%s", help_text);
    } else if (!strcmp(cmd, "step") || !strcmp(cmd, "s")) {
        char *tok = next_tok(&p);
        v = 1;
        if (tok && eval(d, &buf, tok, &v))
            return buf.len;
        if (!pmd85debug_is_paused(d)) {
            out(&buf, "the machine is running (stop first)\n");
        } else {
            long count;
            for (count = 0; count < v; count++) {
                uint64_t was = pmd85debug_stop_serial(d);
                pmd85debug_step(d);
                wait_stopped(d, was);
            }
        }
    } else if (!strcmp(cmd, "over") || !strcmp(cmd, "o")) {
        pmd85debug_step_over(d);
    } else if (!strcmp(cmd, "out")) {
        pmd85debug_step_out(d);
    } else if (!strcmp(cmd, "run") || !strcmp(cmd, "go") || !strcmp(cmd, "g")) {
        pmd85debug_resume(d);
    } else if (!strcmp(cmd, "stop")) {
        pmd85debug_pause(d);
    } else if (!strcmp(cmd, "runto")) {
        char *tok = next_tok(&p);
        if (!tok || eval(d, &buf, tok, &v))
            return buf.len;
        pmd85debug_run_to(d, (uint16_t)v);
    } else if (!strcmp(cmd, "frame")) {
        char *tok = next_tok(&p);
        v = 1;
        if (tok && eval(d, &buf, tok, &v))
            return buf.len;
        pmd85debug_run_frames(d, (int)v);
    } else if (!strcmp(cmd, "break") || !strcmp(cmd, "bp")) {
        add_bp(d, &buf, PMD85DEBUG_BP_EXEC, p);
    } else if (!strcmp(cmd, "bpr")) {
        add_bp(d, &buf, PMD85DEBUG_BP_READ, p);
    } else if (!strcmp(cmd, "bpw")) {
        add_bp(d, &buf, PMD85DEBUG_BP_WRITE, p);
    } else if (!strcmp(cmd, "bpa")) {
        add_bp(d, &buf, PMD85DEBUG_BP_ACCESS, p);
    } else if (!strcmp(cmd, "bpin")) {
        add_bp(d, &buf, PMD85DEBUG_BP_IN, p);
    } else if (!strcmp(cmd, "bpout")) {
        add_bp(d, &buf, PMD85DEBUG_BP_OUT, p);
    } else if (!strcmp(cmd, "bpio")) {
        add_bp(d, &buf, PMD85DEBUG_BP_IO, p);
    } else if (!strcmp(cmd, "delete") || !strcmp(cmd, "enable") || !strcmp(cmd, "disable")) {
        char *tok = next_tok(&p);
        if (!tok) {
            out(&buf, "usage: %s <id>\n", cmd);
        } else {
            long id = strtol(tok[0] == '#' ? tok + 1 : tok, NULL, 10);
            if (!strcmp(cmd, "delete"))
                pmd85debug_breakpoint_remove(d, (int)id);
            else
                pmd85debug_breakpoint_enable(d, (int)id, !strcmp(cmd, "enable"));
        }
    } else if (!strcmp(cmd, "clear")) {
        pmd85debug_breakpoint_clear_all(d);
    } else if (!strcmp(cmd, "breaks") || !strcmp(cmd, "bl")) {
        list_breaks(d, &buf);
    } else if (!strcmp(cmd, "hits")) {
        pmd85debug_breakpoint_clear_hits(d);
    } else if (!strcmp(cmd, "print") || !strcmp(cmd, "p")) {
        if (eval(d, &buf, rest_of(p), &v) == 0) {
            char bin[17];
            int bit;
            for (bit = 0; bit < 16; bit++)
                bin[bit] = (char)('0' + ((v >> (15 - bit)) & 1));
            bin[16] = '\0';
            out(&buf, "%s  %ld  %%%s\n", pmd85hx(d, (unsigned)v & 0xffff, 4), v, bin);
        }
    } else if (!strcmp(cmd, "regs") || !strcmp(cmd, "r")) {
        show_regs(d, &buf);
    } else if (!strcmp(cmd, "set")) {
        char *name = next_tok(&p);
        int reg = name ? reg_by_name(name) : -1;
        int flag = name ? flag_by_name(name) : 0;
        if (reg < 0 && !flag) {
            out(&buf, "usage: set <reg|flag> <expr>\n");
        } else if (eval(d, &buf, rest_of(p), &v) == 0) {
            if (!pmd85debug_is_paused(d))
                out(&buf, "the machine is running (stop first)\n");
            else if (flag ? !pmd85debug_flag_set(d, (unsigned)flag, v != 0)
                          : !pmd85debug_regs_set(d, reg, (unsigned)v))
                out(&buf, "could not set it\n");
            else
                show_regs(d, &buf);
        }
    } else if (!strcmp(cmd, "mem") || !strcmp(cmd, "m")) {
        char *tok = next_tok(&p), *len = next_tok(&p);
        uint8_t bytes[256];
        int got;
        w = 64;
        if (!tok || eval(d, &buf, tok, &v) || (len && eval(d, &buf, len, &w)))
            return buf.len;
        if (w > 256)
            w = 256;
        got = pmd85debug_mem_read(d, PMD85DEBUG_SPACE_CPU, (uint32_t)v & 0xFFFF, bytes, (int)w);
        if (got <= 0)
            out(&buf, "the machine is running (stop first)\n");
        else
            dump(&buf, (uint32_t)v & 0xFFFF, bytes, got);
    } else if (!strcmp(cmd, "poke")) {
        char *tok = next_tok(&p), *item;
        int count = 0;
        if (!tok || eval(d, &buf, tok, &v))
            return buf.len;
        while ((item = next_tok(&p)) != NULL) {
            uint8_t byte;
            if (eval(d, &buf, item, &w))
                return buf.len;
            byte = (uint8_t)w;
            if (pmd85debug_mem_write(d, PMD85DEBUG_SPACE_CPU, (uint32_t)(v + count) & 0xFFFF,
                                     &byte, 1) != 1) {
                out(&buf, "stopped at +%d: running, or ROM there\n", count);
                return buf.len;
            }
            count++;
        }
        out(&buf, "%d byte%s at %s\n", count, count == 1 ? "" : "s",
            pmd85hx(d, (unsigned)v & 0xFFFF, 4));
    } else if (!strcmp(cmd, "disasm") || !strcmp(cmd, "d")) {
        char *tok = next_tok(&p), *num = next_tok(&p);
        pmd85debug_line lines[64];
        pmd85debug_regs r;
        int pcl, count, pos;
        if (!pmd85debug_regs_get(d, &r)) {
            out(&buf, "the machine is running (stop first)\n");
            return buf.len;
        }
        v = r.pc;
        w = 16;
        if ((tok && eval(d, &buf, tok, &v)) || (num && eval(d, &buf, num, &w)))
            return buf.len;
        if (w > 64)
            w = 64;
        count = pmd85debug_disassemble_ex(d, (uint16_t)v, lines, (int)w, &pcl);
        for (pos = 0; pos < count; pos++) {
            if (lines[pos].label[0])
                out(&buf, "%s:\n", lines[pos].label);
            out(&buf, "%c%c%04X  %-9s %-20s%s%s\n", lines[pos].is_pc ? '>' : ' ',
                lines[pos].has_breakpoint ? '*' : lines[pos].bp_disabled ? 'o' : ' ',
                lines[pos].address, lines[pos].bytes, lines[pos].disasm,
                lines[pos].comment[0] ? "; " : "", lines[pos].comment);
        }
    } else if (!strcmp(cmd, "syntax")) {
        char *tok = next_tok(&p);
        if (tok && !strcasecmp(tok, "z80"))
            pmd85debug_set_syntax(d, I8080_SYNTAX_Z80);
        else if (tok && (!strcasecmp(tok, "intel") || !strcmp(tok, "8080")))
            pmd85debug_set_syntax(d, I8080_SYNTAX_INTEL);
        else if (tok)
            out(&buf, "syntax intel|z80\n");
        out(&buf, "mnemonics: %s\n",
            pmd85debug_syntax(d) == I8080_SYNTAX_Z80 ? "Z80" : "Intel 8080");
    } else if (!strcmp(cmd, "hex")) {
        static const char *const styles[] = { "h", "$", "0x", "#" };
        static const char *const shown[] = { "C000h", "$C000", "0xC000", "#C000" };
        char *tok = next_tok(&p);
        if (tok) {
            int i, found = -1;
            for (i = 0; i < I8080_HEX_COUNT; i++)
                if (!strcasecmp(tok, styles[i]) || !strcasecmp(tok, shown[i]))
                    found = i;
            if (found < 0)
                out(&buf, "hex h|$|0x|#\n");
            else
                pmd85debug_set_hex_style(d, (i8080_hex)found);
        }
        out(&buf, "hex numbers: %s\n", shown[pmd85debug_hex_style(d)]);
    } else if (!strcmp(cmd, "trace")) {
        char *tok = next_tok(&p);
        if (tok && !strcmp(tok, "on")) {
            pmd85debug_trace_enable(d, 1);
        } else if (tok && !strcmp(tok, "off")) {
            pmd85debug_trace_enable(d, 0);
        } else if (tok && !strcmp(tok, "clear")) {
            pmd85debug_trace_clear(d);
        } else {
            pmd85debug_trace_entry ring[64];
            int count, pos;
            w = 16;
            if (tok && eval(d, &buf, tok, &w))
                return buf.len;
            if (w > 64)
                w = 64;
            count = pmd85debug_trace_read(d, ring, (int)w);
            if (!count)
                out(&buf, "trace is %s and empty\n",
                    pmd85debug_trace_enabled(d) ? "on" : "off");
            for (pos = count - 1; pos >= 0; pos--) {
                char text[256];
                pmd85debug_trace_format(d, &ring[pos], text, sizeof text);
                out(&buf, "%5u:%5d  %s\n", ring[pos].frame, ring[pos].in_frame, text);
            }
        }
    } else if (!strcmp(cmd, "iolog")) {
        char *tok = next_tok(&p);
        if (tok && !strcmp(tok, "on")) {
            pmd85debug_io_log_enable(d, 1);
        } else if (tok && !strcmp(tok, "off")) {
            pmd85debug_io_log_enable(d, 0);
        } else if (tok && !strcmp(tok, "clear")) {
            pmd85debug_io_log_clear(d);
        } else {
            pmd85debug_io_access log[64];
            int count, pos;
            w = 16;
            if (tok && eval(d, &buf, tok, &w))
                return buf.len;
            if (w > 64)
                w = 64;
            count = pmd85debug_io_log(d, log, (int)w);
            if (!count)
                out(&buf, "the I/O log is %s and empty\n",
                    pmd85debug_io_log_enabled(d) ? "on" : "off");
            for (pos = count - 1; pos >= 0; pos--) {
                const char *name = pmd85debug_port_name(d, log[pos].port);
                out(&buf, "%12llu  %04X  %-3s %s = %s  %s\n",
                    (unsigned long long)log[pos].tstates, log[pos].pc,
                    log[pos].out ? "OUT" : "IN", pmd85hx(d, log[pos].port, 2),
                    pmd85hx(d, log[pos].value, 2), name ? name : "");
            }
        }
    } else if (!strcmp(cmd, "watch")) {
        char *text = rest_of(p);
        int pos;
        if (*text && pmd85watch_add(text, PMD85DEBUG_SPACE_CPU) < 0)
            out(&buf, "no room for another watch\n");
        if (!pmd85watch_count())
            out(&buf, "no watches (watch <addr|symbol|lo-hi>)\n");
        else
            for (pos = 0; pos < pmd85watch_count(); pos++) {
                char text_line[160];
                pmd85watch_format(d, pos, text_line, sizeof text_line);
                out(&buf, "%2d  %s\n", pos, text_line);
            }
    } else if (!strcmp(cmd, "unwatch")) {
        char *tok = next_tok(&p);
        if (tok && !strcmp(tok, "all"))
            pmd85watch_clear();
        else if (tok && eval(d, &buf, tok, &v))
            return buf.len;
        else if (tok)
            pmd85watch_remove((int)v);
        else
            out(&buf, "unwatch <n|all>\n");
    } else if (!strcmp(cmd, "find")) {
        char *tok = next_tok(&p);
        int kind = PMD85SEARCH_HEX, count = 0, flags = 0, found, pos;
        uint8_t pat[PMD85SEARCH_MAX], mask;
        uint32_t hits[32];
        char joined[160];
        if (tok && !strcmp(tok, "text")) {
            kind = PMD85SEARCH_TEXT;
            tok = rest_of(p);
        } else if (tok && !strcmp(tok, "hex")) {
            tok = rest_of(p);
        } else if (tok) {
            snprintf(joined, sizeof joined, "%s %s", tok, rest_of(p));
            tok = joined;
        }
        if (!tok || !*tok || !pmd85search_parse(kind, tok, pat, &count, &mask, &flags)) {
            out(&buf, "find [hex] <bytes> | find text <string>\n");
            return buf.len;
        }
        found = pmd85debug_search(d, PMD85DEBUG_SPACE_CPU, pat, count, mask, flags, 0,
                                  0xFFFF, hits, 32);
        if (found < 0)
            out(&buf, "stop the machine first\n");
        else if (!found)
            out(&buf, "not found\n");
        else
            for (pos = 0; pos < found; pos++)
                out(&buf, "%s\n", pmd85hx(d, hits[pos], 4));
    } else if (!strcmp(cmd, "symbols") || !strcmp(cmd, "sym")) {
        char *filter = rest_of(p);
        int picks[400], count, pos;
        count = pmd85debug_symbols_filter(d, filter, picks, 400);
        for (pos = 0; pos < count; pos++) {
            uint16_t addr;
            char name[48];
            if (pmd85debug_symbols_get(d, picks[pos], &addr, name, sizeof name))
                out(&buf, "%04X  %s\n", addr, name);
        }
        if (!count)
            out(&buf, "no symbols match\n");
    } else if (!strcmp(cmd, "label")) {
        char *tok = next_tok(&p), *name = next_tok(&p);
        if (!tok || !name)
            out(&buf, "usage: label <addr> <name>\n");
        else if (eval(d, &buf, tok, &v) == 0)
            pmd85debug_symbol_add(d, (uint16_t)v, name);
    } else if (!strcmp(cmd, "labels")) {
        char *file = rest_of(p);
        int loaded = *file ? pmd85debug_symbols_load(d, file) : -1;
        if (loaded < 0)
            out(&buf, "could not read '%s'\n", file);
        else
            out(&buf, "%d symbol(s) loaded\n", loaded);
    } else if (!strcmp(cmd, "map")) {
        pmd85debug_region map[64];
        int count = pmd85debug_memory_map(d, map, 64), pos;
        for (pos = 0; pos < count; pos++)
            out(&buf, "%s-%s  %s%s\n", pmd85hx(d, map[pos].lo, 4), pmd85hx(d, map[pos].hi, 4),
                map[pos].name,
                map[pos].writable || map[pos].kind == PMD85DEBUG_REGION_UNMAPPED
                    ? "" : " (read only)");
        if (!count)
            out(&buf, "no map yet (stop the machine once)\n");
    } else if (!strcmp(cmd, "io")) {
        show_io(d, &buf);
    } else if (!strcmp(cmd, "ports")) {
        show_ports(d, &buf);
    } else {
        out(&buf, "unknown command '%s' (try help)\n", cmd);
    }
    return buf.len;
}

int pmd85debug_completions(pmd85debug *d, const char *prefix, char *dst, int dstsz)
{
    outbuf buf = { dst, dstsz, 0 };
    const char *word;
    size_t len;
    int count = 0, pos;
    const int first_word = !strchr(prefix ? prefix : "", ' ');

    if (!d || !dst || dstsz <= 0)
        return 0;
    dst[0] = '\0';
    if (!prefix)
        prefix = "";
    word = strrchr(prefix, ' ');
    word = word ? word + 1 : prefix;
    len = strlen(word);

    if (first_word) {
        for (pos = 0; commands[pos]; pos++)
            if (!strncmp(commands[pos], word, len)) {
                out(&buf, "%s\n", commands[pos]);
                count++;
            }
        return count;
    }
    for (pos = 0; pmd85debug_reg_names[pos]; pos++)
        if (len && !strncmp(pmd85debug_reg_names[pos], word, len)) {
            out(&buf, "%s\n", pmd85debug_reg_names[pos]);
            count++;
        }
    {
        pmd85symtab *table = pmd85debug_symtab(d);
        char name[48];
        for (pos = 0; count < 200 && pmd85symtab_get(table, pos, NULL, name, sizeof name, NULL);
             pos++)
            if (len && !strncasecmp(name, word, len)) {
                out(&buf, "%s\n", name);
                count++;
            }
    }
    return count;
}
