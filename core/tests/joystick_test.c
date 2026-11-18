/*
 * joystick_test: the 4004/482 joysticks.
 *
 *   - the merge (joy_input.h): opposite directions cancel, fire is any;
 *   - which joystick each pad drives: explicit ones keep theirs, automatic
 *     ones fill the free joysticks in order, port A gone with a PMD 32;
 *   - the stick's hysteresis;
 *   - on a running 2A, a program reading the joysticks as PMD software does
 *     (8255 at 4Ch-4Fh: A and B in, C out, strobe PC4 for A and PC0 for B)
 *     sees what pmd85host_set_joystick holds, active low, on each port;
 *     with the strobe low it sees nothing.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "joy_input.h"
#include "pmd85_host.h"
#include "pmd85_host_internal.h"
#include "roms_embedded.h"
#include "test_util.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

enum { U = PMD85SESSION_JOY_UP, D = PMD85SESSION_JOY_DOWN, L = PMD85SESSION_JOY_LEFT,
       R = PMD85SESSION_JOY_RIGHT, F = PMD85SESSION_JOY_FIRE };

static void test_merge(void)
{
    unsigned h[3];
    h[0] = U; h[1] = F; h[2] = 0;
    CHECK(pmd85_joy_merge(h, 3) == (U | F), "up + fire");
    h[0] = U; h[1] = D; h[2] = R;
    CHECK(pmd85_joy_merge(h, 3) == R, "up and down cancel, right stays: %x", pmd85_joy_merge(h, 3));
    h[0] = L | U; h[1] = R; h[2] = F;
    CHECK(pmd85_joy_merge(h, 3) == (U | F), "left and right cancel: %x", pmd85_joy_merge(h, 3));
    h[0] = U | F; h[1] = U | F; h[2] = 0;
    CHECK(pmd85_joy_merge(h, 3) == (U | F), "two sources on one: %x", pmd85_joy_merge(h, 3));
    h[0] = 0; h[1] = 0; h[2] = 0;
    CHECK(pmd85_joy_merge(h, 3) == 0, "nothing");
}

static void ports(const int *assign, int n, unsigned avail, const int *want, const char *what)
{
    int out[8];
    pmd85_pad_ports(assign, n, avail, out);
    for (int i = 0; i < n; i++)
        CHECK(out[i] == want[i], "%s: pad %d on %d, expected %d", what, i, out[i], want[i]);
}

static void test_ports(void)
{
    { int a[] = { -1, -1, -1 }, w[] = { 0, 1, -1 }; ports(a, 3, 3, w, "automatic"); }
    { int a[] = { -1, -1 }, w[] = { 1, -1 };        ports(a, 2, 2, w, "PMD 32 on port A"); }
    { int a[] = { -1, 0 }, w[] = { 1, 0 };          ports(a, 2, 3, w, "explicit 1 first"); }
    { int a[] = { 1, 1 }, w[] = { 1, 1 };           ports(a, 2, 3, w, "two on joystick 2"); }
    { int a[] = { 0, -1 }, w[] = { -1, 1 };         ports(a, 2, 2, w, "explicit on a missing port"); }
    { int a[] = { -1 }, w[] = { -1 };               ports(a, 1, 0, w, "interface off"); }
}

static void test_stick(void)
{
    unsigned d = pmd85_dir_from_stick(0.4f, 0.f, 0, 0.35f, 0.15f);
    CHECK(d == R, "right past enter: %x", d);
    d = pmd85_dir_from_stick(0.2f, 0.f, d, 0.35f, 0.15f);
    CHECK(d == R, "held between exit and enter: %x", d);
    d = pmd85_dir_from_stick(0.1f, 0.f, d, 0.35f, 0.15f);
    CHECK(d == 0, "let go inside exit: %x", d);
    d = pmd85_dir_from_stick(0.2f, 0.f, d, 0.35f, 0.15f);
    CHECK(d == 0, "not engaged below enter: %x", d);
    d = pmd85_dir_from_stick(-0.5f, -0.5f, 0, 0.35f, 0.15f);
    CHECK(d == (L | U), "up-left (+y down): %x", d);
}

/* ---- on the machine ---------------------------------------------------- */

static atomic_int g_frames, g_run, g_jumped;
static pthread_t g_thread;

static void sink(const uint32_t *px, int w, int h, void *u)
{
    (void)px; (void)w; (void)h; (void)u;
    atomic_fetch_add(&g_frames, 1);
}

static void *runner(void *a)
{
    (void)a;
    while (atomic_load(&g_run)) pmd85host_core_run_frame();
    return NULL;
}

static void wait_frames(int n)
{
    const int until = atomic_load(&g_frames) + n;
    struct timespec t = { 0, 1000000L };
    while (atomic_load(&g_frames) < until) nanosleep(&t, NULL);
}

/* once: run the program (emulator thread, quiescent) */
static void jump(void *ctx, uint16_t pc)
{
    pmd85host_cpu c;
    (void)ctx; (void)pc;
    if (atomic_exchange(&g_jumped, 1)) return;
    pmd85host_dbg_cpu(&c);
    c.pc = 0x7000;
    pmd85host_dbg_cpu_set(&c);
}

static void program(uint8_t strobe)
{
    const uint8_t code[] = {
        0xF3,                   /* DI */
        0x3E, 0x92, 0xD3, 0x4F, /* MVI A,92h; OUT 4Fh -- A, B in; C out */
        0x3E, strobe,           /* MVI A,strobe */
        0xD3, 0x4E,             /* OUT 4Eh */
        0xDB, 0x4C,             /* loop: IN 4Ch */
        0x32, 0x00, 0x71,       /* STA 7100h */
        0xDB, 0x4D,             /* IN 4Dh */
        0x32, 0x01, 0x71,       /* STA 7101h */
        0xC3, 0x09, 0x70,       /* JMP loop */
    };
    for (size_t i = 0; i < sizeof code; i++) pmd85host_dbg_poke((uint16_t)(0x7000 + i), code[i]);
}

static void test_machine(void)
{
    char dir[800], p[1300];
    pmd85host_config c;

    if (test_mkdtemp(dir, sizeof dir, "pmd85-joy") != 0) return;
    for (int i = 0; i < pmd85_embedded_rom_count; i++) {
        FILE *f;
        snprintf(p, sizeof p, "%s/%s", dir, pmd85_embedded_roms[i].name);
        if (!(f = fopen(p, "wb"))) return;
        fwrite(pmd85_embedded_roms[i].data, 1, pmd85_embedded_roms[i].size, f);
        fclose(f);
    }
    pmd85host_set_frame_sink(sink, NULL);
    pmd85host_config_defaults(&c);
    c.model = PMD85_MODEL_2A;
    c.joystick_gpio0 = c.joystick_gpio1 = 1;
    snprintf(c.monitor, sizeof c.monitor, "%s/monit2A.rom", dir);
    if (!pmd85host_core_start(&c)) {
        fprintf(stderr, "start: %s\n", pmd85host_last_error());
        failures++;
        return;
    }
    atomic_store(&g_run, 1);
    pthread_create(&g_thread, NULL, runner, NULL);
    wait_frames(60);

    /* strobes up: each port reads its joystick, active low */
    program(0x11);
    pmd85host_set_instr_hook(jump, NULL);
    wait_frames(3);
    pmd85host_set_instr_hook(NULL, NULL);
    CHECK(atomic_load(&g_jumped), "the program never ran");
    pmd85host_set_joystick(0, U | F);
    pmd85host_set_joystick(1, L);
    wait_frames(3);
    CHECK(pmd85host_dbg_peek(0x7100) == (uint8_t)~(U | F), "joystick 1 reads %02X",
          pmd85host_dbg_peek(0x7100));
    CHECK(pmd85host_dbg_peek(0x7101) == (uint8_t)~L, "joystick 2 reads %02X",
          pmd85host_dbg_peek(0x7101));
    pmd85host_set_joystick(0, 0);
    pmd85host_set_joystick(1, R | D);
    wait_frames(3);
    CHECK(pmd85host_dbg_peek(0x7100) == 0xFF, "joystick 1 let go: %02X", pmd85host_dbg_peek(0x7100));
    CHECK(pmd85host_dbg_peek(0x7101) == (uint8_t)~(R | D), "joystick 2 reads %02X",
          pmd85host_dbg_peek(0x7101));

    /* strobes down: nothing from either */
    atomic_store(&g_jumped, 0);
    program(0x00);
    pmd85host_set_instr_hook(jump, NULL);
    wait_frames(3);
    pmd85host_set_instr_hook(NULL, NULL);
    pmd85host_set_joystick(0, F);
    pmd85host_set_joystick(1, F);
    wait_frames(3);
    CHECK(pmd85host_dbg_peek(0x7100) == 0xFF && pmd85host_dbg_peek(0x7101) == 0xFF,
          "read without the strobe: %02X %02X", pmd85host_dbg_peek(0x7100),
          pmd85host_dbg_peek(0x7101));

    atomic_store(&g_run, 0);
    pthread_join(g_thread, NULL);
    pmd85host_set_joystick(0, 0);
    pmd85host_set_joystick(1, 0);
    pmd85host_core_stop();
    test_rmtree(dir);
}

int main(void)
{
    test_merge();
    test_ports();
    test_stick();
    test_machine();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("joystick: ok\n");
    return 0;
}
