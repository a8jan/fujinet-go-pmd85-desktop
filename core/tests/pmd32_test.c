/*
 * pmd32_test: the PMD 32 disk drive through the session.
 *
 *   - a PMD 85-3 with the PMD 32 and a CP/M disk in drive A boots CP/M
 *     (MIKROS): page zero then holds CP/M's jumps (JMP at 0000h, warm boot,
 *     and at 0005h, BDOS);
 *   - a disk goes into drive B and out of it while the machine runs, and is
 *     write-protected, with the settings and the drive's state following;
 *   - a missing or read-only file is refused;
 *   - the disks stay in across a model change.
 *
 * The CP/M disk is $PMD85_P32_IMAGE, else MIKROS from the author's archive;
 * without one the test skips (77). It works on copies.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "pmd85_host.h"
#include "pmd85_host_internal.h"
#include "pmd85session.h"
#include "test_util.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

static void sleep_ms(int ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

static int copy(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb"), *out;
    int ch;
    if (!in) return -1;
    if (!(out = fopen(to, "wb"))) { fclose(in); return -1; }
    while ((ch = fgetc(in)) != EOF) fputc(ch, out);
    fclose(in);
    fclose(out);
    return 0;
}

/* CP/M is up: JMP at 0000h and 0005h (up to 15 s) */
static int cpm_up(void)
{
    for (int i = 0; i < 150; i++) {
        if (pmd85host_dbg_peek(0x0000) == 0xC3 && pmd85host_dbg_peek(0x0005) == 0xC3) return 1;
        sleep_ms(100);
    }
    return 0;
}

int main(void)
{
    const char *src = getenv("PMD85_P32_IMAGE");
    char dir[800], cfg[900], data[900], a[1000], b[1000], ro[1000], missing[1000];
    pmd85session_paths paths;
    pmd85session_start_opts opts;
    pmd85session_disk_info info;
    pmd85session *s;
    FILE *f;

    if (!src || !*src) src = "/srv/oldcomp/archive/PMD85/System/Mikros/mikros48k.p32";
    if (!(f = fopen(src, "rb"))) {
        fprintf(stderr, "no CP/M disk at %s (set PMD85_P32_IMAGE)\n", src);
        return 77;
    }
    fclose(f);
    if (test_mkdtemp(dir, sizeof dir, "pmd85-p32") != 0) return 77;
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    snprintf(a, sizeof a, "%s/cpm.p32", dir);
    snprintf(b, sizeof b, "%s/second.p32", dir);
    snprintf(ro, sizeof ro, "%s/readonly.p32", dir);
    snprintf(missing, sizeof missing, "%s/nothere.p32", dir);
    if (copy(src, a) || copy(src, b) || copy(src, ro)) return 77;
    chmod(ro, 0444);

    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";
    s = pmd85session_new(&paths);
    if (!s) return 1;

    /* before the start: settings only */
    CHECK(pmd85session_disk_insert(s, 0, a, 0) == 0, "insert A before start: %s",
          pmd85session_last_error(s));
    pmd85session_set_int(s, "model", PMD85SESSION_MODEL_3);
    pmd85session_set_int(s, "pmd32", 1);
    pmd85session_set_int(s, "module", PMD85SESSION_MODULE_NONE);
    pmd85session_default_opts(s, &opts);
    opts.enable_fujinet = 0;
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    CHECK(pmd85session_start(s, &opts) == 0, "start: %s", pmd85session_last_error(s));
    CHECK(cpm_up(), "CP/M did not boot from drive A (0000h %02X, 0005h %02X)",
          pmd85host_dbg_peek(0), pmd85host_dbg_peek(5));
    pmd85session_disk_get_info(s, &info);
    CHECK(info.enabled && info.present && !strcmp(info.path[0], a) && !info.path[1][0],
          "info: enabled %d present %d A '%s' B '%s'", info.enabled, info.present,
          info.path[0], info.path[1]);

    /* drive B while running */
    CHECK(pmd85session_disk_insert(s, 1, b, 0) == 0, "insert B: %s", pmd85session_last_error(s));
    pmd85session_disk_get_info(s, &info);
    CHECK(!strcmp(info.path[1], b) && !info.wp[1], "B in: '%s' wp %d", info.path[1], info.wp[1]);
    CHECK(!strcmp(pmd85session_get_str(s, "pmd32_b", ""), b), "B's setting");
    CHECK(pmd85session_disk_protect(s, 1, 1) == 0, "protect B");
    pmd85session_disk_get_info(s, &info);
    CHECK(info.wp[1] && pmd85session_get_int(s, "pmd32_wp_b", 0), "B protected");
    {
        pmd85host_disk_state st;
        pmd85host_disk_state_get(&st);
        CHECK(st.present && !strcmp(st.path[1], b) && st.wp[1], "the drive's B: '%s' wp %d",
              st.path[1], st.wp[1]);
    }
    CHECK(pmd85session_disk_eject(s, 1) == 0, "eject B");
    pmd85session_disk_get_info(s, &info);
    CHECK(!info.path[1][0] && !pmd85session_get_str(s, "pmd32_b", "")[0], "B out: '%s'",
          info.path[1]);

    /* refused: missing, read-only (unless write-protected) */
    CHECK(pmd85session_disk_insert(s, 2, missing, 0) != 0, "a missing file went in");
    CHECK(pmd85session_disk_insert(s, 2, ro, 0) != 0 &&
          strstr(pmd85session_last_error(s), "read-only"), "a read-only file went in: %s",
          pmd85session_last_error(s));
    pmd85session_disk_get_info(s, &info);
    CHECK(!info.path[2][0], "a refused disk left C '%s'", info.path[2]);

    /* a model change: the disks stay in */
    CHECK(pmd85session_disk_insert(s, 1, b, 1) == 0, "B again");
    pmd85session_set_model(s, PMD85SESSION_MODEL_2A);
    sleep_ms(300);
    {
        pmd85host_disk_state st;
        pmd85host_disk_state_get(&st);
        CHECK(st.present && !strcmp(st.path[0], a) && !strcmp(st.path[1], b) && st.wp[1],
              "after the model change: A '%s' B '%s' wp %d", st.path[0], st.path[1], st.wp[1]);
    }

    pmd85session_free(s);
    chmod(ro, 0644);
    test_rmtree(dir);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("pmd32: ok\n");
    return 0;
}
