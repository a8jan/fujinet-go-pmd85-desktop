/*
 * pmd85session FujiNet runtime control: dlopen libfujinet.so and drive the
 * fujinet_desktop_* entry points (the desktop build of fujinet-pc-coco plus
 * the in-process entry wrapper, tools/fujinet/support/fujinet_desktop_entry.cpp).
 * Port of the Android fujinet_android.cpp wrapper. A watchdog (from the
 * Atari 800 app) restarts a runtime that rebooted itself: the web UI's
 * Restart, which in fujinet-pc is exit(75).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "compat.h"
#include "dynlib.h"
#include "session_internal.h"

typedef int (*start_runtime_fn)(const char *root, const char *config,
                                const char *sd, const char *data,
                                int listen_port);
typedef int (*stop_runtime_fn)(void);   /* 1 stopped, 0 detached: still alive */
typedef const char *(*last_error_fn)(void);
typedef int (*read_audio_fn)(int16_t *out, int max_samples, int rate);
typedef void (*clear_audio_fn)(void);
typedef int (*copy_log_fn)(char *out, int max_bytes);
typedef int (*read_log_fn)(unsigned long long *cursor, unsigned *epoch, char *out, int max,
                           int *reset);
typedef int (*int_fn)(void);

/* One runtime per process: the library owns background threads (web admin,
 * network listeners) that live inside its mapping, so it is loaded once and
 * NEVER dlclose'd -- unmapping it while any such thread still runs executes
 * freed code and crashes. A stopped runtime is restarted through the same
 * handle. Same pattern (and reasoning) as the Android wrapper. */
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pmd85_dynlib g_handle;
static start_runtime_fn g_start;
static stop_runtime_fn g_stop;
static last_error_fn g_last_error;
static read_audio_fn g_read_audio;
static clear_audio_fn g_clear_audio;
static copy_log_fn g_copy_log;
static read_log_fn g_read_log;
static int_fn g_is_running, g_exit_code;   /* optional: no watchdog without */

/* the watchdog */
#define FUJINET_EXIT_AND_RESTART 75
static pthread_t g_watch;
static int g_watch_started;
static volatile int g_watch_quit;
static time_t g_restart_times[3];   /* the last restarts it made, oldest first */

static int load_library_locked(pmd85session *s)
{
    char errbuf[256];
    if (g_handle) return 0;

    g_handle = pmd85_dynlib_open(s->fujinet_lib);
    if (!g_handle) {
        session_set_error(s, "FujiNet library load failed: %s",
                          pmd85_dynlib_error(errbuf, sizeof(errbuf)));
        return -1;
    }
    g_start = (start_runtime_fn)pmd85_dynlib_sym(
        g_handle, "fujinet_desktop_start_runtime");
    g_stop = (stop_runtime_fn)pmd85_dynlib_sym(
        g_handle, "fujinet_desktop_stop_runtime");
    g_last_error = (last_error_fn)pmd85_dynlib_sym(
        g_handle, "fujinet_desktop_last_error_message");
    g_read_audio = (read_audio_fn)pmd85_dynlib_sym(
        g_handle, "fujinet_desktop_read_audio");
    g_clear_audio = (clear_audio_fn)pmd85_dynlib_sym(
        g_handle, "fujinet_desktop_clear_audio");
    g_copy_log = (copy_log_fn)pmd85_dynlib_sym(
        g_handle, "fujinet_desktop_copy_recent_log");
    g_read_log = (read_log_fn)pmd85_dynlib_sym(g_handle, "fujinet_desktop_read_log");
    g_is_running = (int_fn)pmd85_dynlib_sym(g_handle, "fujinet_desktop_is_running");
    g_exit_code = (int_fn)pmd85_dynlib_sym(g_handle, "fujinet_desktop_exit_code");
    if (!g_is_running || !g_exit_code)
        g_is_running = g_exit_code = NULL;

    if (!g_start || !g_stop || !g_last_error) {
        session_set_error(s, "%s is missing the desktop runtime contract",
                          s->fujinet_lib);
        /* Leave the handle mapped (never dlclose); just mark it unusable. */
        g_start = NULL;
        return -1;
    }
    return 0;
}

/* 1 if something already accepts connections on 127.0.0.1:port. */
int fujinet_port_in_use(int port)
{
    int ok = 0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
        pmd85_closesocket(fd);
    }
    return ok;
}

/* Make the provisioned fnconfig.ini's [BOIP] port the one this session uses.
 * The runtime tree is provisioned once and then belongs to the user, so a
 * tree from an older build (or a PMD85_BECKER_PORT override) would otherwise
 * leave FujiNet listening where the machine is not looking. Only the port=
 * line inside [BOIP] is touched; everything else is kept byte for byte. */
static void force_boip_port(pmd85session *s)
{
    FILE *f = fopen(s->fujinet_config, "rb");
    char *text, *out, *sec, *line, *end;
    long n;
    char want[32];

    if (!f) return;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 1024 * 1024) { fclose(f); return; }
    text = calloc(1, (size_t)n + 1);
    if (!text || fread(text, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(text); return; }
    fclose(f);

    snprintf(want, sizeof want, "port=%d", s->becker_port);
    sec = strstr(text, "[BOIP]");
    out = calloc(1, (size_t)n + 64);
    if (!out) { free(text); return; }
    if (!sec) {
        snprintf(out, (size_t)n + 64, "%s%s[BOIP]\nenabled=1\nhost=127.0.0.1\n%s\n",
                 text, (n && text[n - 1] != '\n') ? "\n" : "", want);
    } else {
        /* the port= line between [BOIP] and the next section */
        char *next = strstr(sec + 1, "\n[");
        line = strstr(sec, "\nport=");
        if (line && (!next || line < next)) {
            line++;
            end = strchr(line, '\n');
            if (!end) end = text + n;
            if ((size_t)(end - line) == strlen(want) && !strncmp(line, want, strlen(want))) {
                free(text); free(out);
                return;  /* already right */
            }
            snprintf(out, (size_t)n + 64, "%.*s%s%s", (int)(line - text), text, want, end);
        } else {
            char *after = strchr(sec, '\n');
            if (!after) after = text + n;
            snprintf(out, (size_t)n + 64, "%.*s\n%s%s", (int)(after - text), text, want, after);
        }
    }
    f = fopen(s->fujinet_config, "wb");
    if (f) {
        fwrite(out, 1, strlen(out), f);
        fclose(f);
    }
    free(text);
    free(out);
}

static int start_locked(pmd85session *s)
{
    if (paths_provision_fujinet(s) != 0) {
        fprintf(stderr, "pmd85session: FujiNet runtime unavailable; "
                        "continuing without it\n");
        return -1;
    }
    if (load_library_locked(s) != 0) {
        return -1;
    }
    force_boip_port(s);
    /* Two of the firmware patches (fnFsSPIFFS's flash base and mgHttpClient's
     * CA bundle -- see tools/fujinet/build-fujinet-desktop.sh) root themselves
     * from this rather than from the CWD, so that FujiNet keeps working if
     * anything ever moves the process's working directory out from under it.
     * Both fall back to a relative path when it is unset, which is why leaving
     * it unset fails quietly rather than loudly: the CA store simply loads
     * empty and every https:// fetch starts failing like a network fault. */
    pmd85_setenv("FUJINET_RUNTIME_ROOT", s->fujinet_root);
    if (!g_start(s->fujinet_root, s->fujinet_config, s->fujinet_sd,
                 s->fujinet_data, s->becker_port)) {
        const char *err = g_last_error ? g_last_error() : NULL;
        session_set_error(s, "FujiNet runtime failed to start: %s",
                          err && *err ? err : "(unknown)");
        return -1;
    }
    s->fujinet_running = 1;
    return 0;
}

/* Restarts a runtime that ended with "restart me" (75); one that ended
 * otherwise stays stopped. Three restarts within 30 s: it stays stopped
 * (Restart FujiNet in the menu starts it again). The Becker port reconnects
 * by itself. */
static void *watch_main(void *arg)
{
    pmd85session *s = arg;
    while (!g_watch_quit) {
        pmd85_sleep_ms(100);
        pthread_mutex_lock(&g_mtx);
        if (s->fujinet_running && g_is_running && !g_is_running()) {
            const int code = g_exit_code();
            s->fujinet_running = 0;
            if (code == FUJINET_EXIT_AND_RESTART) {
                const time_t now = time(NULL);
                if (now - g_restart_times[0] < 30) {
                    fprintf(stderr, "pmd85session: FujiNet keeps restarting; "
                                    "it is stopped (FujiNet > Restart FujiNet)\n");
                } else {
                    memmove(g_restart_times, g_restart_times + 1,
                            sizeof g_restart_times[0] * 2);
                    g_restart_times[2] = now;
                    if (start_locked(s) == 0)
                        fprintf(stderr, "pmd85session: FujiNet restarted itself\n");
                    else
                        fprintf(stderr, "pmd85session: FujiNet could not restart: %s\n",
                                s->last_error);
                }
            } else {
                fprintf(stderr, "pmd85session: FujiNet stopped (exit code %d)\n", code);
            }
        }
        pthread_mutex_unlock(&g_mtx);
    }
    return NULL;
}

int fujinet_start(pmd85session *s)
{
    int rc;
    pthread_mutex_lock(&g_mtx);
    if (s->fujinet_running) {
        pthread_mutex_unlock(&g_mtx);
        return 0;
    }
    rc = start_locked(s);
    if (rc == 0 && g_is_running && !g_watch_started) {
        g_watch_quit = 0;
        if (pthread_create(&g_watch, NULL, watch_main, s) == 0)
            g_watch_started = 1;
    }
    pthread_mutex_unlock(&g_mtx);
    return rc;
}

/* Wait until something is actually accepting on the Becker port.
 *
 * fujinet_start() returning is NOT enough, and this is the trap that makes
 * this target different. main_setup() comes back as soon as the devices are
 * constructed, but the DriveWire bus then logs "BoIPChannel: No WiFi!" and
 * suspends itself for a full second before it begins listening. Start the
 * emulator in that window and HDB-DOS boots, prints its banner, asks the
 * DriveWire server for its boot sector, gets nothing -- and hangs there
 * forever, with no OK prompt and no way to type. The screen just sits on the
 * banner looking like the emulator is fine.
 *
 * So probe the port instead of guessing at a sleep: connect to it until that
 * succeeds. The probe connection is closed immediately; XRoar's becker client
 * makes the real one. Returns 0 once the listener is up, -1 on timeout --
 * which is not fatal, because the patched becker.c does keep retrying, it
 * just means the first seconds of DriveWire are lost. */
int fujinet_wait_for_becker(pmd85session *s, int timeout_ms)
{
    int waited = 0;

    if (!s->fujinet_running)
        return -1;

    for (;;) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_in addr;
            int ok;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons((uint16_t)s->becker_port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
            pmd85_closesocket(fd);
            if (ok)
                return 0;
        }
        if (waited >= timeout_ms) {
            fprintf(stderr, "pmd85session: the FujiNet Becker listener did not "
                            "come up within %d ms; the machine may need a "
                            "reset once it does\n", timeout_ms);
            return -1;
        }
        pmd85_sleep_ms(25);
        waited += 25;
    }
}

int fujinet_stop(pmd85session *s)
{
    stop_runtime_fn stop = NULL;
    clear_audio_fn clear = NULL;
    int stopped = 1;

    /* the watchdog first, so it cannot restart what is being stopped */
    if (g_watch_started) {
        g_watch_quit = 1;
        pthread_join(g_watch, NULL);
        g_watch_started = 0;
        g_watch_quit = 0;
    }

    pthread_mutex_lock(&g_mtx);
    if (s->fujinet_running) {
        stop = g_stop;
        clear = g_clear_audio;
        s->fujinet_running = 0;
    }
    pthread_mutex_unlock(&g_mtx);

    if (stop) stopped = stop();
    if (clear) clear();
    return stopped;
}

int fujinet_read_log(pmd85session *s, pmd85_log_cursor *cur, char *dst, int max, int *reset)
{
    read_log_fn fn;
    unsigned long long off;
    int n;
    (void)s;
    if (reset) *reset = 0;
    if (!cur || !dst || max <= 0) return 0;
    dst[0] = '\0';
    pthread_mutex_lock(&g_mtx);
    fn = g_read_log;
    pthread_mutex_unlock(&g_mtx);
    if (!fn) return 0;
    off = cur->offset;
    n = fn(&off, &cur->epoch, dst, max, reset);
    cur->offset = off;
    return n;
}

int fujinet_copy_log(pmd85session *s, char *dst, int max)
{
    copy_log_fn fn;
    (void)s;
    if (!dst || max <= 0) return 0;
    dst[0] = '\0';
    pthread_mutex_lock(&g_mtx);
    fn = g_copy_log;
    pthread_mutex_unlock(&g_mtx);
    return fn ? fn(dst, max) : 0;
}

/* Overlay FujiNet (SAM speech) audio onto the emulator mix, saturating.
 *
 * buf is interleaved stereo (nsamples counts int16 samples, so nsamples/2
 * frames) but the runtime produces MONO -- so each sample it hands back is
 * mixed into both channels of one frame. XRoar's sound interface is stereo
 * (see core/xroar/desktop/ao_desktop.c), so the same widening applies here as
 * on the Apple II target. */
void fujinet_mix_audio(pmd85session *s, int16_t *buf, int nsamples, int rate)
{
    static int16_t overlay[2048];
    read_audio_fn fn;
    int frames_left;

    if (!s->fujinet_running || nsamples <= 0) return;
    pthread_mutex_lock(&g_mtx);
    fn = g_read_audio;
    pthread_mutex_unlock(&g_mtx);
    if (!fn) return;

    frames_left = nsamples / 2;

    while (frames_left > 0) {
        int chunk = frames_left > (int)(sizeof(overlay) / sizeof(overlay[0]))
                        ? (int)(sizeof(overlay) / sizeof(overlay[0]))
                        : frames_left;
        int produced = fn(overlay, chunk, rate);
        int i;
        if (produced <= 0) return;
        for (i = 0; i < produced; i++) {
            int c;
            for (c = 0; c < 2; c++) {
                int mixed = (int)buf[i * 2 + c] + (int)overlay[i];
                if (mixed > INT16_MAX) mixed = INT16_MAX;
                if (mixed < INT16_MIN) mixed = INT16_MIN;
                buf[i * 2 + c] = (int16_t)mixed;
            }
        }
        if (produced < chunk) return;
        buf += (size_t)produced * 2;
        frames_left -= produced;
    }
}
