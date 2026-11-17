/*
 * End to end with the real FujiNet: a session with the in-process runtime
 * (the dev build in tools/fujinet/work/out), a fresh runtime tree provisioned
 * from it, the PMD booting into FujiNet CONFIG from the loader module, and the
 * Becker port linked to FujiNet's DriveWire listener on 65505.
 *
 * CONFIG then reads FujiNet's host and device slots over DriveWire, so a
 * screen with the host list on it means the whole chain works: session ->
 * FujiNet start -> listener wait -> machine -> Becker client -> DriveWire ->
 * FujiNet -> back. PMD85_SCREENSHOT=<file.ppm> saves the final frame.
 *
 * Skips (77) when the FujiNet runtime has not been built.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pmd85session.h"
#include "test_util.h"

#ifndef PMD85_DEV_FUJINET_OUT
#define PMD85_DEV_FUJINET_OUT ""
#endif

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static uint32_t fb[PMD85SESSION_FB_MAX_WIDTH * PMD85SESSION_FB_MAX_HEIGHT];

static int webui_port;

/* GET path from the FujiNet web UI; 0 when it answered. */
static int http_get(int port, const char *path)
{
    struct sockaddr_in a;
    char req[128], buf[512];
    int fd = socket(AF_INET, SOCK_STREAM, 0), ok;
    if (fd < 0) return -1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok = connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
    if (ok) {
        snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", path);
        ok = send(fd, req, strlen(req), 0) == (ssize_t)strlen(req) &&
             recv(fd, buf, sizeof buf, 0) > 0;
    }
    close(fd);
    return ok ? 0 : -1;
}

int main(void)
{
    char dir[1024], cfg[1100], data[1100], lib[1200];
    pmd85session_paths paths;
    pmd85session_start_opts opts;
    pmd85session *s;
    FILE *f;

    snprintf(lib, sizeof lib, "%s/libfujinet.so", PMD85_DEV_FUJINET_OUT);
    if (!(f = fopen(lib, "rb"))) {
        snprintf(lib, sizeof lib, "%s/libfujinet.dylib", PMD85_DEV_FUJINET_OUT);
        f = fopen(lib, "rb");
    }
    if (!f) {
        fprintf(stderr, "no FujiNet runtime built in %s\n", PMD85_DEV_FUJINET_OUT);
        return 77;
    }
    fclose(f);

    if (test_mkdtemp(dir, sizeof dir, "pmd85-dw") != 0) return 77;
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = lib;
    paths.fujinet_runtime_src = PMD85_DEV_FUJINET_OUT;

    /* A port nothing else uses, so this is OUR FujiNet answering (a developer
     * machine may well have a standalone fujinet-pc on 65505). */
    {
        static char env[32];
        int port = 0;
        for (int p = 65405; p < 65500 && !port; p++)
            if (!test_port_in_use(p)) port = p;
        if (!port) return 77;
        snprintf(env, sizeof env, "PMD85_BECKER_PORT=%d", port);
        putenv(env);
    }
    /* and the web UI on one of its own: the default 64003 may be a running
     * app's, and this test asks it to restart */
    {
        static char env[48];
        for (int p = 64100; p < 64200 && !webui_port; p++)
            if (!test_port_in_use(p)) webui_port = p;
        if (!webui_port) return 77;
        snprintf(env, sizeof env, "FUJINET_WEBUI_BIND=127.0.0.1:%d", webui_port);
        putenv(env);
    }

    s = pmd85session_new(&paths);
    if (!s) return 1;
    pmd85session_default_opts(s, &opts);
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;
    CHECK(pmd85session_start(s, &opts) == 0, "start: %s", pmd85session_last_error(s));
    CHECK(pmd85session_fujinet_running(s), "FujiNet did not start: %s",
          pmd85session_last_error(s));

    int linked = 0;
    for (int i = 0; i < 100 && !linked; i++) {
        linked = pmd85session_fujinet_link(s);
        if (!linked) sleep_ms(100);
    }
    CHECK(linked, "the Becker port never linked to FujiNet on %s",
          pmd85session_becker_endpoint(s));
    CHECK(!pmd85session_fujinet_warning(s)[0], "%s", pmd85session_fujinet_warning(s));

    /* CONFIG boots at ~2.6 s, then talks DriveWire */
    sleep_ms(6000);
    {
        uint64_t serial = 0;
        int w = 0, h = 0, lit = 0;
        pmd85session_copy_frame(s, fb, &serial, &w, &h);
        for (int i = 0; i < w * h; i++)
            if (fb[i]) lit++;
        printf("CONFIG with FujiNet: %dx%d, lit %d\n", w, h, lit);
        CHECK(lit > 1500, "CONFIG not on screen (%d lit)", lit);
        const char *shot = getenv("PMD85_SCREENSHOT");
        if (shot && (f = fopen(shot, "wb"))) {
            fprintf(f, "P6 %d %d 255\n", w, h);
            for (int i = 0; i < w * h; i++) {
                unsigned char c[3] = { fb[i] >> 16, fb[i] >> 8, fb[i] };
                fwrite(c, 1, 3, f);
            }
            fclose(f);
        }
    }
    {
        static char log[64 * 1024];
        int n = pmd85session_fujinet_copy_log(s, log, sizeof log);
        CHECK(n > 0, "no FujiNet log");
        {
            char want[64];
            snprintf(want, sizeof want, "listening on 127.0.0.1:%s", getenv("PMD85_BECKER_PORT"));
            CHECK(strstr(log, want) != NULL, "FujiNet did not listen on the session's port");
            CHECK(strstr(log, "bind failed") == NULL, "FujiNet could not bind its port");
        }
        if (getenv("PMD85_DUMP_LOG")) fwrite(log, 1, (size_t)n, stdout);
    }

    /* FujiNet alone restarted under a running PMD: back up, and the Becker
     * port links again by itself */
    {
        int relinked = 0;
        CHECK(pmd85session_fujinet_restart(s) == 0, "restart: %s",
              pmd85session_last_error(s));
        CHECK(pmd85session_fujinet_running(s), "FujiNet not running after restart");
        for (int i = 0; i < 100 && !relinked; i++) {
            relinked = pmd85session_fujinet_link(s);
            if (!relinked) sleep_ms(100);
        }
        CHECK(relinked, "the Becker port did not link again after the restart");
        CHECK(pmd85session_is_running(s), "the PMD stopped with FujiNet");
    }

    /* the web UI's Restart: fujinet-pc would exit(75) for its supervisor to
     * start it again; here the runtime ends and the session's watchdog
     * restarts it, the PMD running on and its Becker port linking again */
    {
        int relinked = 0, seen = 0;
        static char log[64 * 1024];
        CHECK(http_get(webui_port, "/restart") == 0, "no web UI on %d", webui_port);
        for (int i = 0; i < 100; i++) {          /* the runtime goes down... */
            int n = pmd85session_fujinet_copy_log(s, log, sizeof log);
            if (n > 0 && strstr(log, "FujiNet restarted itself")) { seen = 1; break; }
            sleep_ms(100);
        }
        CHECK(seen, "the watchdog did not restart FujiNet after the web UI's Restart");
        for (int i = 0; i < 100 && !relinked; i++) {   /* ...and comes back */
            relinked = pmd85session_fujinet_running(s) && pmd85session_fujinet_link(s) &&
                       http_get(webui_port, "/") == 0;
            if (!relinked) sleep_ms(100);
        }
        CHECK(relinked, "FujiNet / the Becker link / the web UI not back after the restart");
        CHECK(pmd85session_is_running(s), "the PMD stopped with FujiNet");
    }

    pmd85session_free(s);
    test_rmtree(dir);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("drivewire_smoke: ok\n");
    return 0;
}
