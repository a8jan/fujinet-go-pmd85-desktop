/*
 * rebuild_test: a live machine change (model, ROM module) rebuilds the
 * machine on the emulator thread -- the new one is made before the old one
 * goes. Upstream keeps MIF 85's SAA1099 in a global that each Mif85 makes
 * and deletes; the old machine's going must not take the new one's chip
 * (a NULL SAA1099 crashed the next reset). Rebuilds back and forth with
 * MIF 85 on, a reset after each, and MIF 85 still there at the end.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "pmd85debug.h"
#include "pmd85_host.h"
#include "pmd85_host_internal.h"
#include "roms_embedded.h"
#include "test_util.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

static atomic_int g_frames, g_run;
static pthread_t g_thread;

static void sink(const uint32_t *px, int w, int h, void *user)
{
    (void)px; (void)w; (void)h; (void)user;
    atomic_fetch_add(&g_frames, 1);
}

static void *runner(void *arg)
{
    (void)arg;
    while (atomic_load(&g_run))
        pmd85host_core_run_frame();
    return NULL;
}

static void wait_frames(int n)
{
    const int until = atomic_load(&g_frames) + n;
    struct timespec t = { 0, 1000000L };
    while (atomic_load(&g_frames) < until)
        nanosleep(&t, NULL);
}

int main(void)
{
    char dir[800], p[1300];
    pmd85host_config c;
    pmd85debug_io io;
    static const int models[] = { PMD85_MODEL_2A, PMD85_MODEL_3, PMD85_MODEL_2,
                                  PMD85_MODEL_C2717, PMD85_MODEL_2A };

    if (test_mkdtemp(dir, sizeof dir, "pmd85-rebuild") != 0) return 77;
    for (int i = 0; i < pmd85_embedded_rom_count; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, pmd85_embedded_roms[i].name);
        FILE *f = fopen(p, "wb");
        if (!f) return 77;
        fwrite(pmd85_embedded_roms[i].data, 1, pmd85_embedded_roms[i].size, f);
        fclose(f);
    }

    pmd85host_set_frame_sink(sink, NULL);
    pmd85host_config_defaults(&c);
    c.model = PMD85_MODEL_2A;
    c.mif85 = 1;
    snprintf(c.monitor, sizeof c.monitor, "%s/monit2A.rom", dir);
    if (!pmd85host_core_start(&c)) {
        fprintf(stderr, "start failed: %s\n", pmd85host_last_error());
        return 77;
    }
    atomic_store(&g_run, 1);
    pthread_create(&g_thread, NULL, runner, NULL);
    wait_frames(20);

    for (size_t i = 0; i < sizeof models / sizeof models[0]; i++) {
        c.model = models[i];
        pmd85host_core_reconfigure(&c);
        wait_frames(10);
        pmd85host_core_reset();       /* Mif85::ResetDevice: SAA1099->Clear() */
        wait_frames(10);
    }

    atomic_store(&g_run, 0);
    pthread_join(g_thread, NULL);
    pmd85host_dbg_io(&io);
    CHECK(io.mif85, "MIF 85 is not on the rebuilt 2A");
    pmd85host_core_stop();
    test_rmtree(dir);
    if (failures) return 1;
    printf("rebuild: ok\n");
    return 0;
}
