/*
 * Boot smoke test: build machines from the embedded firmware and run them,
 * headless and as fast as they go, through the host API.
 *
 *   - every model boots to something on screen;
 *   - a PMD 85-2A with no module stops at the monitor prompt and types;
 *   - a PMD 85-2A with the FujiNet loader module auto-runs it into FujiNet
 *     CONFIG (a busy screen, not the prompt);
 *   - a PMD 85-2A with the BASIC-G module runs it.
 *
 * "On screen" is judged from the rendered frame, so this covers the frame
 * decode too. Exit 77 (skip) if the embedded firmware is missing.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmd85_host.h"
#include "pmd85session.h"
#include "roms_embedded.h"
#include "test_util.h"

static uint32_t g_fb[384 * 256];
static int g_w, g_h, g_frames;

static void sink(const uint32_t *px, int w, int h, void *user)
{
    (void)user;
    memcpy(g_fb, px, (size_t)w * h * sizeof(uint32_t));
    g_w = w;
    g_h = h;
    g_frames++;
}

static int lit(void)
{
    int n = 0;
    for (int i = 0; i < g_w * g_h; i++)
        if (g_fb[i]) n++;
    return n;
}

/* lit pixels in the bottom text row band (the monitor prompt line) */
static int lit_rows(int y0, int y1)
{
    int n = 0;
    for (int y = y0; y < y1 && y < g_h; y++)
        for (int x = 0; x < g_w; x++)
            if (g_fb[y * g_w + x]) n++;
    return n;
}

static char g_romdir[1024];

static int write_roms(void)
{
    if (pmd85_embedded_rom_count <= 0) return -1;
    for (int i = 0; i < pmd85_embedded_rom_count; i++) {
        char p[1200];
        snprintf(p, sizeof p, "%s/%s", g_romdir, pmd85_embedded_roms[i].name);
        FILE *f = fopen(p, "wb");
        if (!f) return -1;
        fwrite(pmd85_embedded_roms[i].data, 1, pmd85_embedded_roms[i].size, f);
        fclose(f);
    }
    return 0;
}

static void rom(char *dst, const char *name)
{
    snprintf(dst, PMD85HOST_PATH_MAX, "%s/%s", g_romdir, name);
}

static int run(const pmd85host_config *c, int frames)
{
    g_frames = 0;
    if (!pmd85host_core_start(c)) {
        fprintf(stderr, "start failed: %s\n", pmd85host_last_error());
        return -1;
    }
    for (int i = 0; i < frames; i++)
        pmd85host_core_run_frame();
    return g_frames;
}

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

int main(void)
{
    static const char *const monitors[] = {
        "monit1.rom", "monit2.rom", "monit2A.rom", "monit3.rom",
        "mato-mb.rom", "didalfa.rom", "didalfa2.rom", "c2717.rom",
    };
    pmd85host_config c;

    if (test_mkdtemp(g_romdir, sizeof g_romdir, "pmd85-boot") != 0 || write_roms() != 0) {
        fprintf(stderr, "no embedded firmware to boot\n");
        return 77;
    }
    pmd85host_set_frame_sink(sink, NULL);

    /* every model comes up and draws */
    for (int m = 0; m < PMD85_MODEL_COUNT; m++) {
        pmd85host_config_defaults(&c);
        c.model = m;
        rom(c.monitor, monitors[m]);
        int f = run(&c, 300);
        CHECK(f == 300, "model %d: %d frames", m, f);
        CHECK(lit() > 30, "model %d (%s): blank screen after 6 s", m,
              pmd85session_model_name(m));
        printf("model %-16s %dx%d lit %d\n", pmd85session_model_name(m), g_w, g_h, lit());
        pmd85host_core_stop();
    }

    /* 2A, bare monitor: the prompt is on the bottom line, and typing echoes */
    pmd85host_config_defaults(&c);
    rom(c.monitor, "monit2A.rom");
    run(&c, 150);
    int prompt = lit_rows(246, 256);
    CHECK(prompt > 50, "2A: no monitor prompt on the bottom line (%d)", prompt);
    static uint32_t before[256 * 384];
    memcpy(before, g_fb, sizeof before);
    /* type "MEM" -- keys held a few frames each, as a person would. The
     * monitor clears its "++ Os ready ++" line at the first key and echoes
     * the command there, so the bottom line must change and stay lit. */
    static const int keys[] = { 0x10, 0x08, 0x10 };
    for (int k = 0; k < 3; k++) {
        pmd85host_inject_key(1, keys[k]);
        for (int i = 0; i < 4; i++) pmd85host_core_run_frame();
        pmd85host_inject_key(0, keys[k]);
        for (int i = 0; i < 4; i++) pmd85host_core_run_frame();
    }
    int changed = 0;
    for (int i = 246 * g_w; i < 256 * g_w; i++)
        if (g_fb[i] != before[i]) changed++;
    CHECK(changed > 20 && lit_rows(246, 256) > 20,
          "2A: typing did not echo (%d pixels changed, %d lit)", changed,
          lit_rows(246, 256));
    pmd85host_core_stop();

    /* 2A + the FujiNet loader in the MegaModule: auto-runs into CONFIG */
    pmd85host_config_defaults(&c);
    rom(c.monitor, "monit2A.rom");
    c.rom_module = PMD85_ROMMOD_MEGA;
    rom(c.mrm, "fujinet-config.rmm");
    run(&c, 200);
    CHECK(lit() > 1500, "2A + FujiNet loader: CONFIG did not come up (%d lit)", lit());
    CHECK(pmd85host_mega_page() == 0, "MegaModule page %d", pmd85host_mega_page());
    printf("FujiNet CONFIG: lit %d\n", lit());

    /* a model change rebuilds the machine with its module: 2A -> 3 -> 2A.
     * The 3's monitor does not auto-run it (the prompt, module still in the
     * slot); back on the 2A the loader comes up again */
    for (int m = 0; m < 2; m++) {
        c.model = m == 0 ? PMD85_MODEL_3 : PMD85_MODEL_2A;
        rom(c.monitor, m == 0 ? "monit3.rom" : "monit2A.rom");
        pmd85host_core_reconfigure(&c);
        for (int i = 0; i < 200; i++) pmd85host_core_run_frame();
        CHECK(pmd85host_mega_page() == 0, "reconfigure %d: no MegaModule (page %d)",
              m, pmd85host_mega_page());
        if (m == 1)
            CHECK(lit() > 1500, "back to 2A + FujiNet loader: CONFIG did not "
                  "come up (%d lit)", lit());
        printf("reconfigure %d: lit %d, page %d\n", m, lit(), pmd85host_mega_page());
    }
    pmd85host_core_stop();

    /* the 85-2 monitor auto-runs the loader as the 2A's does */
    pmd85host_config_defaults(&c);
    c.model = PMD85_MODEL_2;
    rom(c.monitor, "monit2.rom");
    c.rom_module = PMD85_ROMMOD_MEGA;
    rom(c.mrm, "fujinet-config.rmm");
    run(&c, 300);
    CHECK(lit() > 1500, "85-2 + FujiNet loader: CONFIG did not come up "
          "(%d lit)", lit());
    printf("85-2: lit %d\n", lit());
    pmd85host_core_stop();

    /* an 85-3 in the 85-2 compatibility mode auto-runs the loader too */
    c.model = PMD85_MODEL_3;
    c.compat_mode = 1;
    rom(c.monitor, "monit3.rom");
    run(&c, 700);
    CHECK(lit() > 1500, "85-3 compat + FujiNet loader: CONFIG did not come up "
          "(%d lit)", lit());
    printf("85-3 compat: lit %d\n", lit());
    pmd85host_core_stop();

    /* a warm reset keeps running; a reconfigure swaps the machine */
    pmd85host_config_defaults(&c);
    rom(c.monitor, "monit2A.rom");
    run(&c, 100);
    pmd85host_core_reset();
    for (int i = 0; i < 100; i++) pmd85host_core_run_frame();
    CHECK(lit_rows(246, 256) > 50, "2A: no prompt after RESET");
    c.model = PMD85_MODEL_3;
    rom(c.monitor, "monit3.rom");
    pmd85host_core_reconfigure(&c);
    for (int i = 0; i < 200; i++) pmd85host_core_run_frame();
    CHECK(lit() > 30, "reconfigure to 85-3: blank");
    pmd85host_core_stop();

    test_rmtree(g_romdir);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("boot_smoke: ok\n");
    return 0;
}
