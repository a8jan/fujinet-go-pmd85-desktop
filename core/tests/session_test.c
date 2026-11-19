/*
 * The session lifecycle, end to end and in real time: provision a fresh
 * config/data tree (the embedded firmware materialised into it), start with
 * FujiNet and audio off, see frames published at ~50 Hz, switch the model
 * live, stop, start again, and check the settings persisted.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "pmd85session.h"
#include "test_util.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint32_t fb[PMD85SESSION_FB_MAX_WIDTH * PMD85SESSION_FB_MAX_HEIGHT];

static int lit_now(pmd85session *s)
{
    uint64_t serial = 0;
    int w = 0, h = 0, n = 0;
    if (!pmd85session_copy_frame(s, fb, &serial, &w, &h)) return -1;
    for (int i = 0; i < w * h; i++)
        if (fb[i]) n++;
    return n;
}

int main(void)
{
    char dir[1024], cfg[1100], data[1100], probe[1200];
    pmd85session_paths paths;
    pmd85session_start_opts opts;
    pmd85session *s;

    if (test_mkdtemp(dir, sizeof dir, "pmd85-session") != 0) return 77;
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";

    s = pmd85session_new(&paths);
    CHECK(s != NULL, "pmd85session_new");
    if (!s) return 1;

    snprintf(probe, sizeof probe, "%s/monit2A.rom", pmd85session_roms_path(s));
    CHECK(fopen(probe, "rb") != NULL, "firmware not materialised at %s", probe);

    pmd85session_default_opts(s, &opts);
    CHECK(opts.model == PMD85SESSION_MODEL_2A, "default model %d", opts.model);
    CHECK(opts.module == PMD85SESSION_MODULE_FUJINET, "default module %d", opts.module);
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;

    CHECK(pmd85session_start(s, &opts) == 0, "start: %s", pmd85session_last_error(s));
    /* the loader's banner, a blank, then CONFIG at ~2.6 s of machine time */
    sleep_ms(3500);
    /* The pacer's rate, not the host's: a shared CI machine can stop the
     * emulator thread for over 100 ms at a time, and the pacer then rightly
     * drops those frames rather than race. Run plus dropped is the wall clock;
     * never more than 50 Hz, and any frames lost to stalls are said. */
    uint64_t frames = pmd85session_frame_count(s);
    uint64_t dropped = pmd85session_frames_dropped(s);
    CHECK(frames + dropped >= 160 && frames + dropped <= 190 && frames <= 190,
          "%llu frames + %llu dropped in 3.5 s (want ~175: 50 Hz)",
          (unsigned long long)frames, (unsigned long long)dropped);
    if (dropped)
        fprintf(stderr, "note: %llu frames dropped after host stalls\n",
                (unsigned long long)dropped);
    int lit = lit_now(s);
    CHECK(lit > 1500, "FujiNet CONFIG not on screen (%d lit)", lit);
    CHECK(pmd85session_mega_page(s) == 0, "mega page %d", pmd85session_mega_page(s));

    /* live: to the bare monitor of an 85-3 */
    pmd85session_set_module(s, PMD85SESSION_MODULE_NONE);
    pmd85session_set_model(s, PMD85SESSION_MODEL_3);
    sleep_ms(2500);
    CHECK(pmd85session_model(s) == PMD85SESSION_MODEL_3, "model");
    CHECK(pmd85session_mega_page(s) == -1, "a MegaModule survived module=None");
    lit = lit_now(s);
    CHECK(lit > 30 && lit < 1500, "85-3 monitor screen (%d lit)", lit);

    /* a module file of the user's: a plain .rmm goes into the ROM pack (no
     * MegaModule), a .mrm into a MegaModule; a bad one changes nothing */
    {
        char rmm[1300], mrm[1300], empty[1300];
        FILE *in, *out;
        int ch;
        snprintf(rmm, sizeof rmm, "%s/basic2A.rmm", pmd85session_roms_path(s));
        snprintf(mrm, sizeof mrm, "%s/loader-copy.mrm", dir);
        snprintf(empty, sizeof empty, "%s/empty.rmm", dir);
        snprintf(probe, sizeof probe, "%s/fujinet-config-loader.rmm",
                 pmd85session_roms_path(s));
        in = fopen(probe, "rb");
        out = fopen(mrm, "wb");
        while (in && out && (ch = fgetc(in)) != EOF) fputc(ch, out);
        if (in) fclose(in);
        if (out) fclose(out);
        fclose(fopen(empty, "wb"));

        pmd85session_set_model(s, PMD85SESSION_MODEL_2A);
        CHECK(pmd85session_set_module_file(s, rmm) == 0, "rmm: %s", pmd85session_last_error(s));
        CHECK(pmd85session_module(s) == PMD85SESSION_MODULE_FILE, "module not FILE");
        CHECK(pmd85session_module_file_ok(s), "module file not ok");
        sleep_ms(1500);
        CHECK(pmd85session_mega_page(s) == -1, "a .rmm went into a MegaModule");

        CHECK(pmd85session_set_module_file(s, mrm) == 0, "mrm: %s", pmd85session_last_error(s));
        sleep_ms(3500);
        CHECK(pmd85session_mega_page(s) == 0, "a .mrm not in a MegaModule (%d)",
              pmd85session_mega_page(s));
        CHECK(pmd85session_mega_size(s) > 0 && pmd85session_mega_size(s) <= 32 * 1024,
              "the loader .mrm is %d bytes in the MegaModule", pmd85session_mega_size(s));
        lit = lit_now(s);
        CHECK(lit > 1500, "the loader from a .mrm not on screen (%d lit)", lit);

        CHECK(pmd85session_set_module_file(s, empty) != 0, "an empty file accepted");
        CHECK(!strcmp(pmd85session_module_file(s), mrm), "a refused file was kept");
    }

    pmd85session_stop(s);
    CHECK(!pmd85session_is_running(s), "still running after stop");
    CHECK(pmd85session_start(s, &opts) == 0, "restart: %s", pmd85session_last_error(s));
    sleep_ms(500);
    CHECK(pmd85session_frame_count(s) > frames, "no frames after restart");
    pmd85session_free(s);

    /* the choices persisted */
    s = pmd85session_new(&paths);
    pmd85session_default_opts(s, &opts);
    CHECK(opts.model == PMD85SESSION_MODEL_2A, "model did not persist (%d)", opts.model);
    CHECK(opts.module == PMD85SESSION_MODULE_FILE, "module did not persist (%d)", opts.module);
    CHECK(strstr(pmd85session_module_file(s), "loader-copy.mrm") != NULL,
          "module file did not persist (%s)", pmd85session_module_file(s));
    pmd85session_free(s);

    test_rmtree(dir);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("session: ok\n");
    return 0;
}
