/*
 * mouse_test: the Mouse 602 (GPMD85Emulator's, on port 8Ch) driven by
 * pmd85host_mouse, read by a PMD program that records every change of the
 * port: a move of 2n pixels is n quadrature cycles on its axis (x right
 * 4,12,8,0 in bits 2-3; y down 1,3,2,0 in bits 0-1; the reverse cycles the
 * other way), the left / right buttons are bits 6 / 7, the middle presses
 * both; leaving the screen and coming back elsewhere is no move; a machine
 * without the mouse leaves the port alone.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "pmd85_host.h"
#include "pmd85_host_internal.h"
#include "roms_embedded.h"
#include "test_util.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

static atomic_int g_frames, g_run, g_jumped;
static pthread_t g_thread;
static char g_dir[800];

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

static void jump(void *ctx, uint16_t pc)
{
    pmd85host_cpu c;
    (void)ctx; (void)pc;
    if (atomic_exchange(&g_jumped, 1)) return;
    pmd85host_dbg_cpu(&c);
    c.pc = 0x7000;
    pmd85host_dbg_cpu_set(&c);
}

/* Every change of port 8Ch into 7100h.. (up to 71FFh), the count at 7000h's
 * HL; the first byte is the value at the start. */
static void program(void)
{
    static const uint8_t code[] = {
        0xF3,                   /* 7000 DI */
        0x21, 0x00, 0x71,       /* 7001 LXI H,7100h */
        0x06, 0xFF,             /* 7004 MVI B,FFh */
        0xDB, 0x8C,             /* 7006 loop: IN 8Ch */
        0xB8,                   /* 7008 CMP B */
        0xCA, 0x06, 0x70,       /* 7009 JZ loop */
        0x47,                   /* 700C MOV B,A */
        0x77,                   /* 700D MOV M,A */
        0x23,                   /* 700E INX H */
        0x7C,                   /* 700F MOV A,H */
        0xFE, 0x72,             /* 7010 CPI 72h */
        0xC2, 0x06, 0x70,       /* 7012 JNZ loop */
        0xC3, 0x15, 0x70,       /* 7015 JMP $ */
    };
    for (int i = 0; i < 0x100; i++) pmd85host_dbg_poke((uint16_t)(0x7100 + i), 0xEE);
    for (size_t i = 0; i < sizeof code; i++) pmd85host_dbg_poke((uint16_t)(0x7000 + i), code[i]);
    atomic_store(&g_jumped, 0);
    pmd85host_set_instr_hook(jump, NULL);
    wait_frames(3);
    pmd85host_set_instr_hook(NULL, NULL);
}

/* the recorded changes after the start value, as a string "4 12 8 0 ..." */
static const char *recorded(void)
{
    static char out[1024];
    int k = 0;
    out[0] = 0;
    for (int i = 1; i < 0x100; i++) {
        int v = pmd85host_dbg_peek((uint16_t)(0x7100 + i));
        if (v == 0xEE) break;
        k += snprintf(out + k, sizeof out - (size_t)k, "%s%d", k ? " " : "", v);
    }
    return out;
}

static int start(int mouse, int model, const char *monitor)
{
    pmd85host_config c;
    pmd85host_config_defaults(&c);
    c.model = model;
    c.mouse602 = mouse;
    snprintf(c.monitor, sizeof c.monitor, "%s/%s", g_dir, monitor);
    if (!pmd85host_core_start(&c)) {
        fprintf(stderr, "start: %s\n", pmd85host_last_error());
        return 0;
    }
    atomic_store(&g_run, 1);
    pthread_create(&g_thread, NULL, runner, NULL);
    wait_frames(30);
    return 1;
}

static void stop(void)
{
    atomic_store(&g_run, 0);
    pthread_join(g_thread, NULL);
    pmd85host_core_stop();
}

int main(void)
{
    char p[1300];
    if (test_mkdtemp(g_dir, sizeof g_dir, "pmd85-mouse") != 0) return 77;
    for (int i = 0; i < pmd85_embedded_rom_count; i++) {
        FILE *f;
        snprintf(p, sizeof p, "%s/%s", g_dir, pmd85_embedded_roms[i].name);
        if (!(f = fopen(p, "wb"))) return 77;
        fwrite(pmd85_embedded_roms[i].data, 1, pmd85_embedded_roms[i].size, f);
        fclose(f);
    }
    pmd85host_set_frame_sink(sink, NULL);
    if (!start(1, PMD85_MODEL_2A, "monit2A.rom")) return 1;

    /* the pointer comes onto the screen: a start, no move */
    pmd85host_mouse(100, 100, 0, 1);
    wait_frames(2);
    program();
    CHECK(pmd85host_dbg_peek(0x7100) == 0, "the port at rest reads %02X", pmd85host_dbg_peek(0x7100));

    /* 6 pixels right: three x cycles (a step per two pixels) */
    pmd85host_mouse(106, 100, 0, 1);
    wait_frames(10);
    CHECK(!strcmp(recorded(), "4 12 8 0 4 12 8 0 4 12 8 0"), "6 right: '%s'", recorded());

    /* 4 pixels up: two y cycles the other way */
    program();
    pmd85host_mouse(106, 96, 0, 1);
    wait_frames(10);
    CHECK(!strcmp(recorded(), "2 3 1 0 2 3 1 0"), "4 up: '%s'", recorded());

    /* left button, then right, then the middle (both) */
    program();
    pmd85host_mouse(106, 96, PMD85_MOUSE_LEFT, 1);
    wait_frames(3);
    pmd85host_mouse(106, 96, 0, 1);
    wait_frames(3);
    pmd85host_mouse(106, 96, PMD85_MOUSE_RIGHT, 1);
    wait_frames(3);
    pmd85host_mouse(106, 96, PMD85_MOUSE_MIDDLE, 1);
    wait_frames(3);
    pmd85host_mouse(106, 96, 0, 1);
    wait_frames(3);
    CHECK(!strcmp(recorded(), "64 0 128 192 0"), "buttons: '%s'", recorded());

    /* off the screen with the left held, back elsewhere: released, no move */
    program();
    pmd85host_mouse(106, 96, PMD85_MOUSE_LEFT, 1);
    wait_frames(3);
    pmd85host_mouse(0, 0, PMD85_MOUSE_LEFT, 0);
    wait_frames(3);
    pmd85host_mouse(200, 30, 0, 1);
    wait_frames(10);
    CHECK(!strcmp(recorded(), "64 0"), "out and back: '%s'", recorded());
    /* and moving on from where it came back */
    program();
    pmd85host_mouse(202, 32, 0, 1);
    wait_frames(10);
    CHECK(!strcmp(recorded(), "5 15 10 0"), "2 right, 2 down: '%s'", recorded());
    stop();

    /* no mouse: the port is not the mouse's */
    if (!start(0, PMD85_MODEL_2A, "monit2A.rom")) return 1;
    pmd85host_mouse(10, 10, 0, 1);
    program();
    pmd85host_mouse(20, 10, PMD85_MOUSE_LEFT, 1);
    wait_frames(10);
    CHECK(recorded()[0] == 0, "without a mouse the port changed: '%s'", recorded());
    stop();

    test_rmtree(g_dir);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("mouse: ok\n");
    return 0;
}
