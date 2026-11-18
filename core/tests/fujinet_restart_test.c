/*
 * FujiNet restarted in the same process (Restart FujiNet, and the web UI's
 * Restart through the watchdog) keeps working: the host listings -- the SD
 * card and, when a tnfsd is at hand, a TNFS server -- are the same before
 * and after each restart, with an image from the host mounted.
 *
 * In-process, a restart is main_shutdown_handler() and main_setup() again on
 * the same globals; what the last run left behind (the bus's shutdown flag,
 * mounted images, hosts' open filesystems) must not leak into the next. With
 * the shutdown flag left set, every TNFS request of the next run "succeeded"
 * unanswered and its listing was garbage.
 *
 * tnfsd: $PMD85_TNFSD, else "tnfsd" on PATH; without one (or with TNFS's
 * port 16384 taken) only the SD card is listed. And the console log read
 * incrementally (the log window): every byte once, a reset on a restart. Skips (77) when the FujiNet
 * runtime has not been built.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>

#include "pmd85session.h"
#include "test_util.h"

#ifndef PMD85_DEV_FUJINET_OUT
#define PMD85_DEV_FUJINET_OUT ""
#endif

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

static int webui_port;

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* One request to FujiNet's web server; the reply (headers and body) into out. */
static int web(const char *method, const char *path, const char *json, char *out, int outsz)
{
    struct sockaddr_in a;
    char req[1024];
    int fd = socket(AF_INET, SOCK_STREAM, 0), total = 0;
    out[0] = 0;
    if (fd < 0) return -1;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)webui_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    snprintf(req, sizeof req,
             "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
             "Content-Length: %u\r\n\r\n%s",
             method, path, json ? (unsigned)strlen(json) : 0u, json ? json : "");
    if (send(fd, req, strlen(req), 0) <= 0) {
        close(fd);
        return -1;
    }
    for (;;) {
        struct pollfd pf = { fd, POLLIN, 0 };
        int n;
        if (poll(&pf, 1, total ? 3000 : 20000) <= 0) break;
        n = (int)recv(fd, out + total, (size_t)(outsz - 1 - total), 0);
        if (n <= 0) break;
        total += n;
        out[total] = 0;
        if (strstr(out, "</html>") || total >= outsz - 1) break;
    }
    close(fd);
    out[total] = 0;
    return total;
}

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    for (; (hay = strstr(hay, needle)) != NULL; hay += strlen(needle)) n++;
    return n;
}

/* Host slot `slot` (0-based): the root listing has the files made, and as
 * many entries as the first listing of that slot (*base, set when 0) --
 * garbage after a restart shows as extra (or missing) entries. */
static void check_listing(int slot, const char *what, const char *const *names, int n,
                          int *base)
{
    static char page[1 << 17];
    char url[96];
    int entries;
    snprintf(url, sizeof url, "/hsdir?hostslot=%d&path=%%2F", slot);
    web("GET", url, NULL, page, sizeof page);
    for (int i = 0; i < n; i++)
        CHECK(strstr(page, names[i]), "%s: %s missing from the listing", what, names[i]);
    entries = count_of(page, "<li>");
    if (!*base)
        *base = entries;
    CHECK(entries >= n && entries == *base, "%s: %d entries, the first listing had %d",
          what, entries, *base);
}

static void mount_drive1(int host_slot, const char *path, const char *what)
{
    static char page[4096];
    char json[256];
    snprintf(json, sizeof json, "{\"path\":\"%s\",\"host_slot\":%d,\"mode\":\"r\"}", path,
             host_slot);
    web("POST", "/api/v1/drives/1/mount", json, page, sizeof page);
    CHECK(strstr(page, " 200 "), "%s: mount %s failed: %.200s", what, path, page);
}

static void make_files(const char *dir, const char *const *names, int n)
{
    char path[1300];
    for (int i = 0; i < n; i++) {
        FILE *f;
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        f = fopen(path, "wb");
        CHECK(f != NULL, "make %s", path);
        if (f) {
            for (int k = 0; k < 4096; k++) fputc(k & 0xFF, f);
            fclose(f);
        }
    }
}

static pid_t start_tnfsd(const char *root)
{
    const char *bin = getenv("PMD85_TNFSD");
    pid_t pid;
    if (test_port_in_use(16384)) return -1;
    pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execlp(bin && *bin ? bin : "tnfsd", "tnfsd", root, (char *)NULL);
        _exit(127);
    }
    sleep_ms(500);
    if (pid > 0 && waitpid(pid, NULL, WNOHANG) == pid)
        return -1;   /* not installed, or it could not bind */
    return pid;
}

/* Host 2 of the provisioned config is a public TNFS server; point it here. */
static int tnfs_host_local(const char *ini)
{
    static char text[64 * 1024];
    FILE *f = fopen(ini, "rb");
    size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    char *host2, *name, *eol;
    if (f) fclose(f);
    text[n] = 0;
    if (!(host2 = strstr(text, "[Host2]")) || !(name = strstr(host2, "name=")) ||
        !(eol = strchr(name, '\n')))
        return -1;
    if (!(f = fopen(ini, "wb"))) return -1;
    fwrite(text, 1, (size_t)(name - text), f);
    fputs("name=127.0.0.1", f);
    fputs(eol, f);
    fclose(f);
    return 0;
}

/* Reads a cursor dry into acc (appending); 1 if any read said reset. */
static int drain_log(pmd85session *s, pmd85_log_cursor *cur, char *acc, size_t accsz)
{
    static char chunk[1 << 16];
    int n, reset, any = 0;
    do {
        n = pmd85session_fujinet_read_log(s, cur, chunk, sizeof chunk, &reset);
        if (reset) {
            acc[0] = 0;   /* a new log: what was shown is gone */
            any = 1;
        }
        if (strlen(acc) + (size_t)n >= accsz) {
            CHECK(0, "log accumulator full");
            return any;
        }
        strcat(acc, chunk);
    } while (n > 0);
    return any;
}

static int back_up(pmd85session *s)
{
    for (int i = 0; i < 150; i++) {
        static char page[512];
        if (pmd85session_fujinet_running(s) && pmd85session_fujinet_link(s) &&
            web("GET", "/", NULL, page, sizeof page) > 0)
            return 1;
        sleep_ms(100);
    }
    return 0;
}

int main(void)
{
    static const char *const sdfiles[] = { "sdone.dsk", "sdtwo.dsk", "sdthree.dsk" };
    static const char *const tnfsfiles[] = { "tnfsone.dsk", "tnfstwo.dsk", "tnfsthree.dsk" };
    char dir[1024], cfg[1100], data[1100], lib[1200], ini[1300], sd[1300], tdir[1300];
    pmd85session_paths paths;
    pmd85session_start_opts opts;
    pmd85session *s;
    pid_t tnfsd = -1;
    int sd_base = 0, tnfs_base = 0;
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

    if (test_mkdtemp(dir, sizeof dir, "pmd85-fnrestart") != 0) return 77;
    snprintf(cfg, sizeof cfg, "%s/config", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = lib;
    paths.fujinet_runtime_src = PMD85_DEV_FUJINET_OUT;
    {   /* ports of our own (see drivewire_smoke) */
        static char benv[32], wenv[48];
        int port = 0;
        for (int p = 65405; p < 65500 && !port; p++)
            if (!test_port_in_use(p)) port = p;
        for (int p = 64100; p < 64200 && !webui_port; p++)
            if (!test_port_in_use(p)) webui_port = p;
        if (!port || !webui_port) return 77;
        snprintf(benv, sizeof benv, "PMD85_BECKER_PORT=%d", port);
        snprintf(wenv, sizeof wenv, "FUJINET_WEBUI_BIND=127.0.0.1:%d", webui_port);
        putenv(benv);
        putenv(wenv);
    }

    s = pmd85session_new(&paths);
    if (!s) return 1;
    pmd85session_default_opts(s, &opts);
    opts.enable_audio = 0;
    opts.enable_gamepad = 0;

    /* a first start provisions the runtime tree; then its config is ours */
    CHECK(pmd85session_start(s, &opts) == 0, "start: %s", pmd85session_last_error(s));
    pmd85session_stop(s);
    snprintf(ini, sizeof ini, "%s/fujinet/fnconfig.ini", data);
    snprintf(sd, sizeof sd, "%s/fujinet/SD", data);
    snprintf(tdir, sizeof tdir, "%s/tnfsroot", dir);
    make_files(sd, sdfiles, 3);
    mkdir(tdir, 0755);
    make_files(tdir, tnfsfiles, 3);
    tnfsd = start_tnfsd(tdir);
    if (tnfsd > 0)
        CHECK(tnfs_host_local(ini) == 0, "no [Host2] in %s", ini);
    else
        fprintf(stderr, "fujinet_restart: no tnfsd (set PMD85_TNFSD, or TNFS's port is "
                        "taken): the TNFS listing is not tested\n");

    CHECK(pmd85session_start(s, &opts) == 0, "start: %s", pmd85session_last_error(s));
    CHECK(back_up(s), "FujiNet / the Becker link / the web UI did not come up");

    check_listing(0, "SD", sdfiles, 3, &sd_base);
    if (tnfsd > 0) {
        check_listing(1, "TNFS", tnfsfiles, 3, &tnfs_base);
        mount_drive1(1, "/tnfsone.dsk", "TNFS");
    } else {
        mount_drive1(0, "/sdone.dsk", "SD");
    }

    /* the incremental log (the console window): every byte once, in order */
    static char log_a[3 << 20], log_b[3 << 20];
    pmd85_log_cursor c1 = { 0, 0 }, c2 = { 0, 0 };
    CHECK(drain_log(s, &c1, log_a, sizeof log_a), "a first read starts a log");
    CHECK(strstr(log_a, "fnConfig::load"), "the log has the run's start");
    sleep_ms(1500);
    CHECK(!drain_log(s, &c1, log_a, sizeof log_a), "a reset while the same run goes on");
    log_b[0] = 0;
    drain_log(s, &c2, log_b, sizeof log_b);
    CHECK(strlen(log_b) >= strlen(log_a) && strncmp(log_a, log_b, strlen(log_a)) == 0,
          "the appended log is not the log read whole (%zu vs %zu bytes)", strlen(log_a),
          strlen(log_b));

    for (int i = 1; i <= 4; i++) {
        char what[64];
        if (i < 4) {
            CHECK(pmd85session_fujinet_restart(s) == 0, "restart %d: %s", i,
                  pmd85session_last_error(s));
        } else {   /* the web UI's Restart: the runtime ends, the watchdog restarts it */
            static char page[512];
            web("GET", "/restart", NULL, page, sizeof page);
            sleep_ms(1500);
        }
        CHECK(back_up(s), "FujiNet not back after restart %d", i);
        snprintf(what, sizeof what, "SD after restart %d", i);
        check_listing(0, what, sdfiles, 3, &sd_base);
        if (tnfsd > 0) {
            snprintf(what, sizeof what, "TNFS after restart %d", i);
            check_listing(1, what, tnfsfiles, 3, &tnfs_base);
        }
    }
    CHECK(pmd85session_is_running(s), "the PMD stopped with FujiNet");
    CHECK(drain_log(s, &c1, log_a, sizeof log_a), "a restart did not reset the reader");
    CHECK(strstr(log_a, "fnConfig::load"), "the new run's log");

    pmd85session_free(s);
    if (tnfsd > 0) {
        kill(tnfsd, SIGTERM);
        waitpid(tnfsd, NULL, 0);
    }
    test_rmtree(dir);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("fujinet_restart: ok%s\n", tnfsd > 0 ? "" : " (SD only)");
    return 0;
}
