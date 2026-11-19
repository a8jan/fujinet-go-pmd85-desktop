/*
 * debug_test -- the debugger engine, against a real running machine.
 *
 * The engine parks the emulator thread inside the host's instruction hook,
 * so nothing about it can be tested without a machine actually executing.
 * This starts a session on a PMD 85-2A at its bare monitor (no FujiNet, no
 * audio), engages the debugger, pokes a small program into RAM and drives
 * it the way a frontend does: pause, step, step over/out, run to, frames,
 * execute / read / write / IN / OUT breakpoints with conditions, register
 * and memory edits, the trace, the I/O log, the prompt, and a session stop
 * while paused.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pmd85debug.h"
#include "pmd85memtools.h"
#include "pmd85session.h"
#include "test_util.h"

static int failures;

static void ok(const char *what, int cond)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond)
        failures++;
}

static void nap_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int wait_stop(pmd85debug *d, uint64_t was)
{
    pmd85debug_stop st;
    for (int i = 0; i < 300; i++) {
        pmd85debug_stop_snapshot(d, &st);
        if (st.paused && st.serial != was)
            return 1;
        nap_ms(10);
    }
    return 0;
}

static void reason(pmd85debug *d, char *dst, size_t sz)
{
    pmd85debug_stop st;
    pmd85debug_stop_snapshot(d, &st);
    snprintf(dst, sz, "%s", st.reason);
}

static int pc_of(pmd85debug *d)
{
    pmd85debug_regs r;
    return pmd85debug_regs_get(d, &r) ? r.pc : -1;
}

/* Resume and wait for the next stop. */
static int go(pmd85debug *d)
{
    uint64_t was = pmd85debug_stop_serial(d);
    pmd85debug_resume(d);
    return wait_stop(d, was);
}

static char out[16384];
static const char *cmd(pmd85debug *d, const char *c)
{
    pmd85debug_command(d, c, out, sizeof out);
    return out;
}

/* The test program at #0100 (stack at #7F00):
 *   0100 21 00 02   LXI  H,#0200
 *   0103 36 41      MVI  M,#41
 *   0105 7E         MOV  A,M
 *   0106 D3 20      OUT  #20
 *   0108 DB 20      IN   #20
 *   010A CD 10 01   CALL #0110
 *   010D C3 0D 01   JMP  #010D
 *   0110 00         NOP
 *   0111 C9         RET                                                   */
static const uint8_t prog[] = {
    0x21, 0x00, 0x02, 0x36, 0x41, 0x7E, 0xD3, 0x20, 0xDB, 0x20,
    0xCD, 0x10, 0x01, 0xC3, 0x0D, 0x01, 0x00, 0xC9,
};

static void restart_prog(pmd85debug *d)
{
    pmd85debug_regs_set(d, PMD85DEBUG_REG_PC, 0x0100);
    pmd85debug_regs_set(d, PMD85DEBUG_REG_SP, 0x7F00);
}

int main(void)
{
    char dir[1024], cfg[1100], data[1100], text[200];
    pmd85session_paths paths;
    pmd85session_start_opts opts;
    pmd85session *s;
    pmd85debug *d;
    pmd85debug_regs r;
    uint64_t was;
    long v;
    int id;

    if (test_mkdtemp(dir, sizeof dir, "pmd85-debug") != 0)
        return 77;
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";
    s = pmd85session_new(&paths);
    if (!s)
        return 77;
    pmd85session_default_opts(s, &opts);
    opts.model = PMD85SESSION_MODEL_2A;
    opts.module = PMD85SESSION_MODULE_NONE;
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    if (pmd85session_start(s, &opts) != 0) {
        fprintf(stderr, "start: %s\n", pmd85session_last_error(s));
        return 77;
    }
    nap_ms(1500);   /* to the monitor prompt */

    d = pmd85session_debugger(s);
    ok("a debugger", d != NULL);
    pmd85debug_set_engaged(d, 1);
    ok("engaged", pmd85debug_is_engaged(d));

    /* ---- pause ---- */
    was = pmd85debug_stop_serial(d);
    pmd85debug_pause(d);
    ok("pause stops", wait_stop(d, was));
    reason(d, text, sizeof text);
    ok("reason 'stopped at ....h'", !strncmp(text, "stopped at ", 11) && strlen(text) == 16 &&
                                    text[15] == 'h');
    ok("registers while paused", pmd85debug_regs_get(d, &r));
    pmd85debug_monitor_symbols_info(d, text, sizeof text);
    ok("2A: the 85-2 listing's symbols, all but HEADIN",
       strstr(text, "monit2.rom listing, 129 of 130 routines") != NULL);
    {
        uint16_t a = 0;
        char name[32] = "";
        /* the 85-2 listing names 8C00h both TRANSFER and ROMIN */
        ok("ROMIN is 8C00h", pmd85debug_symbol_find(d, "ROMIN", &a) && a == 0x8C00 &&
                             pmd85debug_symbol_at(d, 0x8C00, name, sizeof name) &&
                             (!strcmp(name, "ROMIN") || !strcmp(name, "TRANSFER")));
    }
    ok("the monitor runs from ROM (#8000-#8FFF)", r.pc >= 0x8000 && r.pc <= 0x8FFF);
    {
        pmd85debug_region map[64];
        int n = pmd85debug_memory_map(d, map, 64), rom = 0, ram = 0, vram = 0;
        for (int i = 0; i < n; i++) {
            rom |= map[i].kind == PMD85DEBUG_REGION_ROM && map[i].lo <= 0x8000 &&
                   map[i].hi >= 0x8000;
            ram |= map[i].kind == PMD85DEBUG_REGION_RAM && map[i].lo == 0x0000;
            vram |= map[i].kind == PMD85DEBUG_REGION_VRAM && map[i].hi == 0xFFFF;
        }
        ok("memory map: RAM at 0, ROM at #8000, VRAM at the top", rom && ram && vram);
        if (!(rom && ram && vram))
            for (int i = 0; i < n; i++)
                printf("     #%04X-#%04X %s\n", map[i].lo, map[i].hi, map[i].name);
    }

    /* ---- memory and registers ---- */
    ok("poke the program", pmd85debug_mem_write(d, PMD85DEBUG_SPACE_CPU, 0x0100, prog,
                                                (int)sizeof prog) == (int)sizeof prog);
    {
        uint8_t back[sizeof prog];
        ok("read it back", pmd85debug_mem_read(d, PMD85DEBUG_SPACE_CPU, 0x0100, back,
                                               (int)sizeof back) == (int)sizeof back &&
                               !memcmp(back, prog, sizeof prog));
        uint8_t rom = 0, x = 0x55;
        pmd85debug_mem_read(d, PMD85DEBUG_SPACE_CPU, 0x8000, &rom, 1);
        ok("ROM is not written", pmd85debug_mem_write(d, PMD85DEBUG_SPACE_CPU, 0x8000, &x, 1) == 0);
    }
    restart_prog(d);
    ok("PC set", pc_of(d) == 0x0100);
    ok("eval {#0100}: a little-endian word", pmd85debug_eval(d, "{#0100}", &v, text, 64) == 0 &&
                                              v == 0x0021);
    ok("eval [#0101] + 1", pmd85debug_eval(d, "[$0101]+1", &v, text, 64) == 0 && v == 1);

    /* ---- disassembly ---- */
    {
        pmd85debug_line l[4];
        int pcl, n = pmd85debug_disassemble_ex(d, 0x0100, l, 4, &pcl);
        ok("disassembly", n == 4 && pcl == 0 && !strcmp(l[0].disasm, "LXI  H,0200h") &&
                          !strcmp(l[3].disasm, "OUT  20h") && l[3].port == 0x20);
        ok("row back from #0105 is #0103", pmd85debug_row_address(d, 0x0105, -1) == 0x0103);
    }

    /* ---- step ---- */
    was = pmd85debug_stop_serial(d);
    pmd85debug_step(d);
    ok("step", wait_stop(d, was) && pc_of(d) == 0x0103);

    /* ---- write breakpoint: stops after the MVI M ---- */
    id = pmd85debug_breakpoint_add(d, PMD85DEBUG_BP_WRITE, 0x0200, 0x0200, NULL);
    ok("bpw set", id > 0);
    ok("write breakpoint stops", go(d));
    reason(d, text, sizeof text);
    ok("  after the write, at #0105", pc_of(d) == 0x0105);
    ok("  'wrote #0200 = #41 (was ..) at #0103'",
       !strncmp(text, "wrote 0200h = 41h (was ", 23) && strstr(text, "at 0103h"));
    pmd85debug_breakpoint_remove(d, id);

    /* ---- read breakpoint with a condition ---- */
    id = pmd85debug_breakpoint_add(d, PMD85DEBUG_BP_READ, 0x01FF, 0x0201, "value == #41");
    ok("bpr set", id > 0);
    ok("read breakpoint stops", go(d));
    reason(d, text, sizeof text);
    ok("  'read #0200 = #41 at #0105'", !strcmp(text, "read 0200h = 41h at 0105h") &&
                                        pc_of(d) == 0x0106);
    pmd85debug_breakpoint_remove(d, id);

    /* ---- port breakpoints ---- */
    id = pmd85debug_breakpoint_add(d, PMD85DEBUG_BP_OUT, 0x20, 0x20, NULL);
    ok("bpout set", id > 0);
    ok("OUT breakpoint stops", go(d));
    reason(d, text, sizeof text);
    ok("  'OUT #20 = #41 at #0106'", !strcmp(text, "OUT 20h = 41h at 0106h") &&
                                     pc_of(d) == 0x0108);
    pmd85debug_breakpoint_remove(d, id);
    id = pmd85debug_breakpoint_add(d, PMD85DEBUG_BP_IN, 0x1F, 0x21, NULL);
    ok("IN breakpoint stops", go(d));
    reason(d, text, sizeof text);
    ok("  'IN #20 = .. at #0108'", !strncmp(text, "IN 20h = ", 9) &&
                                   strstr(text, "at 0108h") && pc_of(d) == 0x010A);
    pmd85debug_breakpoint_remove(d, id);
    ok("a memory + port breakpoint is refused",
       pmd85debug_breakpoint_add(d, PMD85DEBUG_BP_IN | PMD85DEBUG_BP_READ, 0, 0, NULL) < 0);
    ok("a port past #FF is refused",
       pmd85debug_breakpoint_add(d, PMD85DEBUG_BP_OUT, 0x20, 0x100, NULL) < 0);

    /* ---- mirrors: the system PIO decodes A7, A3, A2, so its port B is F5h
     * and 85h (and six more) ---- */
    {
        static const uint8_t in85[] = { 0xDB, 0x85, 0x00, 0x00 };   /* IN 85h; NOP; NOP */
        pmd85debug_breakpoint bp[2];
        char desc[200];
        pmd85debug_mem_write(d, PMD85DEBUG_SPACE_CPU, 0x0120, in85, (int)sizeof in85);
        pmd85debug_regs_set(d, PMD85DEBUG_REG_PC, 0x0120);
        id = pmd85debug_breakpoint_add(d, PMD85DEBUG_BP_IN, 0xF5, 0xF5, NULL);
        ok("bpin F5h stops on IN 85h, a mirror", go(d) && pc_of(d) == 0x0122);
        reason(d, text, sizeof text);
        ok("  'IN 85h (SYS PIO PB) = ..'", !strncmp(text, "IN 85h (SYS PIO PB) = ", 22));
        pmd85debug_breakpoints_list(d, bp, 2);
        pmd85bp_describe(d, &bp[0], desc, sizeof desc);
        ok("  described with its mirrors", strstr(desc, "port F5h (SYS PIO PB, and mirrors)") != NULL);
        pmd85debug_breakpoint_remove(d, id);
        pmd85debug_regs_set(d, PMD85DEBUG_REG_PC, 0x0120);
        id = pmd85debug_breakpoint_add(d, PMD85DEBUG_BP_IN | PMD85DEBUG_BP_EXACT, 0xF5, 0xF5, NULL);
        pmd85debug_breakpoints_list(d, bp, 2);
        pmd85bp_describe(d, &bp[0], desc, sizeof desc);
        ok("  an exact one is described so", strstr(desc, "(SYS PIO PB, exact)") != NULL);
        was = pmd85debug_stop_serial(d);
        pmd85debug_run_to(d, 0x0123);
        ok("an exact bpin F5h lets IN 85h by", wait_stop(d, was) && pc_of(d) == 0x0123);
        pmd85debug_breakpoint_remove(d, id);
        ok("an exact memory breakpoint is refused",
           pmd85debug_breakpoint_add(d, PMD85DEBUG_BP_READ | PMD85DEBUG_BP_EXACT, 0, 0, NULL) < 0);
        ok("port 86h is named as its register", pmd85debug_port_name(d, 0x86) &&
                                                !strcmp(pmd85debug_port_name(d, 0x86), "SYS PIO PC"));
        pmd85debug_regs_set(d, PMD85DEBUG_REG_PC, 0x010A);   /* back at the CALL, for what follows */
    }
    {
        pmd85portfilter f;
        char t[2048];
        ok("filter F4-F7 takes the mirrors", pmd85portfilter_parse(d, "F4-F7", &f) && f.hit[0x86] &&
                                             f.hit[0xF6] && f.hit[0xA4] && !f.hit[0x88]);
        ok("filter =86 is that port only", pmd85portfilter_parse(d, "=86", &f) && f.hit[0x86] && !f.hit[0xF6]);
        ok("filter by name", pmd85portfilter_parse(d, "sys pio", &f) && f.hit[0x84] && f.hit[0xF7] &&
                             !f.hit[0x4C]);
        ok("filter: nothing by that name", !pmd85portfilter_parse(d, "zzz", &f));
        ok("filter: empty is every port", pmd85portfilter_parse(d, "", &f) && f.hit[0] && f.hit[255]);
        pmd85ports_describe(d, t, sizeof t);
        ok("decode: the system PIO at F4h-F7h, each register at 8 ports",
           strstr(t, "SYS PIO") && strstr(t, "F4h-F7h") && strstr(t, "port & 8Ch = 84h") &&
           strstr(t, "4 registers, each at 8 ports"));
        if (!strstr(t, "each at 8 ports"))
            printf("%s", t);
    }

    /* ---- step over / into / out ---- */
    was = pmd85debug_stop_serial(d);
    pmd85debug_step_over(d);
    ok("step over the CALL lands after it", wait_stop(d, was) && pc_of(d) == 0x010D);
    pmd85debug_regs_set(d, PMD85DEBUG_REG_PC, 0x010A);
    was = pmd85debug_stop_serial(d);
    pmd85debug_step(d);
    ok("step into the CALL", wait_stop(d, was) && pc_of(d) == 0x0110);
    was = pmd85debug_stop_serial(d);
    pmd85debug_step_out(d);
    ok("step out returns after the CALL", wait_stop(d, was) && pc_of(d) == 0x010D);
    reason(d, text, sizeof text);
    ok("  'returned to #010D'", !strcmp(text, "returned to 010Dh"));

    /* ---- exec breakpoint with a condition; run to ---- */
    restart_prog(d);
    /* A is #FF there (the IN from an empty port), HL still #0200 */
    id = pmd85debug_breakpoint_add(d, PMD85DEBUG_BP_EXEC, 0x010D, 0x010D,
                                   "hl == #0200 && a == #FF");
    ok("conditional exec breakpoint stops", go(d) && pc_of(d) == 0x010D);
    reason(d, text, sizeof text);
    ok("  'breakpoint: exec #010D'", !strcmp(text, "breakpoint: exec 010Dh"));
    {
        pmd85debug_breakpoint bp[4];
        int n = pmd85debug_breakpoints_list(d, bp, 4);
        ok("  and it counted a hit", n == 1 && bp[0].hits == 1);
    }
    pmd85debug_breakpoint_remove(d, id);
    restart_prog(d);
    was = pmd85debug_stop_serial(d);
    pmd85debug_run_to(d, 0x0110);
    ok("run to", wait_stop(d, was) && pc_of(d) == 0x0110);

    /* ---- frames ---- */
    was = pmd85debug_stop_serial(d);
    pmd85debug_run_frames(d, 2);
    ok("run 2 frames", wait_stop(d, was));
    reason(d, text, sizeof text);
    ok("  'frame N at ...'", !strncmp(text, "frame ", 6));

    /* ---- registers and flags ---- */
    ok("set HL", pmd85debug_regs_set(d, PMD85DEBUG_REG_HL, 0x1234) &&
                 pmd85debug_regs_get(d, &r) && r.h == 0x12 && r.l == 0x34);
    ok("set the zero flag", pmd85debug_flag_set(d, PMD85DEBUG_F_Z, 1) &&
                            pmd85debug_regs_get(d, &r) && (r.f & PMD85DEBUG_F_Z) &&
                            (r.f & 0x02));

    /* ---- trace and the I/O log ---- */
    restart_prog(d);
    pmd85debug_trace_enable(d, 1);
    pmd85debug_io_log_clear(d);
    was = pmd85debug_stop_serial(d);
    pmd85debug_run_to(d, 0x010A);
    ok("run through the program", wait_stop(d, was));
    {
        pmd85debug_trace_entry t[8];
        int n = pmd85debug_trace_read(d, t, 8);
        /* newest first: what ran, so not the #010A it stopped before */
        ok("trace recorded it", n >= 5 && t[0].pc == 0x0108 && t[4].pc == 0x0100);
        pmd85debug_trace_format(d, &t[4], text, sizeof text);
        ok("  formatted", !strncmp(text, "0100  LXI  H,0200h", 18));
        pmd85debug_io_access io[4];
        n = pmd85debug_io_log(d, io, 4);
        ok("I/O log: the OUT and the IN", n == 2 && io[0].out == 0 && io[0].pc == 0x0108 &&
                                          io[1].out == 1 && io[1].value == 0x41);
    }
    pmd85debug_trace_enable(d, 0);

    /* ---- the chips ---- */
    {
        pmd85debug_io io;
        ok("chips", pmd85debug_io_get(d, &io) && io.sys.valid && io.model ==
                    PMD85SESSION_MODEL_2A && io.mega_page == -1);
        const char *n = pmd85debug_port_name(d, 0x84);
        ok("port #84 is the system PIO", n && !strcmp(n, "SYS PIO PA"));
        ok("port #20 answers nothing", pmd85debug_port_name(d, 0x20) == NULL);
    }

    /* ---- the prompt ---- */
    ok("prompt: regs", strstr(cmd(d, "regs"), "PC=") != NULL);
    ok("prompt: disasm", strstr(cmd(d, "disasm 0100h 2"), "LXI  H,0200h") != NULL);
    cmd(d, "syntax z80");
    ok("prompt: z80 syntax", strstr(cmd(d, "disasm $0100 1"), "ld   hl,0200h") != NULL);
    cmd(d, "syntax intel");
    ok("prompt: bpio", strstr(cmd(d, "bpio #84-#87"), "breakpoint #") != NULL);
    ok("prompt: breaks", strstr(cmd(d, "breaks"), "In/Out  ports 84h-87h") != NULL);
    cmd(d, "clear");
    ok("prompt: io", strstr(cmd(d, "io"), "SYS PIO") != NULL);
    ok("prompt: ports", strstr(cmd(d, "ports"), "F4h-F7h") != NULL);
    ok("prompt: bpout exact", strstr(cmd(d, "bpout F6h exact"), "breakpoint #") != NULL &&
                              strstr(cmd(d, "breaks"), "port F6h (SYS PIO PC, exact)") != NULL);
    cmd(d, "clear");
    ok("prompt: print", strstr(cmd(d, "print hl"), "0200h") != NULL);
    ok("prompt: iolog", strstr(cmd(d, "iolog"), "OUT 20h = 41h") != NULL);
    cmd(d, "label #0110 sub");
    ok("prompt: label shows in disassembly",
       strstr(cmd(d, "disasm 0x010A 1"), "; sub") != NULL);

    /* ---- hex styles: every input form, each output style ---- */
    ok("eval C000h (letter first)", pmd85debug_eval(d, "C000h", &v, text, 64) == 0 && v == 0xC000);
    ok("eval 0xC000 $C000 #C000",
       pmd85debug_eval(d, "0xC000 + $C000 - #C000", &v, text, 64) == 0 && v == 0xC000);
    ok("eval: a register still wins over hex (bc, not 0BCh)",
       pmd85debug_eval(d, "bc", &v, text, 64) == 0 && v != 0xBC);
    ok("eval ffh", pmd85debug_eval(d, "ffh", &v, text, 64) == 0 && v == 0xFF);
    ok("prompt: hex $", strstr(cmd(d, "hex $"), "$C000") != NULL &&
                        strstr(cmd(d, "disasm 0100h 1"), "LXI  H,$0200") != NULL);
    ok("prompt: hex 0x", strstr(cmd(d, "hex 0x"), "0xC000") != NULL &&
                         strstr(cmd(d, "print 255"), "0x00FF") != NULL);
    ok("prompt: hex #", strstr(cmd(d, "hex #"), "#C000") != NULL &&
                        strstr(cmd(d, "disasm 0100h 1"), "LXI  H,#0200") != NULL);
    ok("the style persists in the settings",
       pmd85session_get_int(s, "debugger_hex", -1) == I8080_HEX_HASH);
    cmd(d, "hex h");

    /* ---- release, then a stop while paused must not hang ---- */
    pmd85debug_set_engaged(d, 0);
    nap_ms(100);
    ok("released: running", !pmd85debug_is_paused(d));
    pmd85debug_set_engaged(d, 1);
    was = pmd85debug_stop_serial(d);
    pmd85debug_pause(d);
    ok("paused again", wait_stop(d, was));
    pmd85session_stop(s);
    ok("stopped the session while paused", !pmd85session_is_running(s));
    pmd85session_free(s);
    test_rmtree(dir);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("debug: ok\n");
    return 0;
}
