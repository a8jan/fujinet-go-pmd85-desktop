/*
 * tape_test: the tape deck (GPMD85Emulator's TapeBrowser) on a bare 85-2A
 * monitor, with a tape built here (file #5, 256 bytes at 7000h):
 *
 *   - flash load: MGLD 05 brings the bytes in at once (the ROM traps);
 *   - real time: the same at 1200 Bd through the tape interface, on a
 *     rebuilt machine -- the tape stays in the deck across the rebuild;
 *   - recording: MGSV 07 7000,70FF into an empty deck, Save As a .ptp, and
 *     the file holds header #7 and the bytes;
 *   - the commands' errors (no tape, not a tape);
 *   - the block list: seek, STOP marker, move, delete, append, and a
 *     saved edit loads back in its new order.
 *
 * Frames run on a thread of their own, as in the app: the deck's commands
 * are carried out between frames and wait for that.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pmd85_host.h"
#include "pmd85_host_internal.h"
#include "roms_embedded.h"
#include "test_util.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

static char g_dir[800];
static atomic_int g_frames;
static atomic_int g_run;
static pthread_t g_thread;

static void sleep_ms(int ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

static uint32_t g_fb[384 * 256];
static int g_w, g_h;

static void sink(const uint32_t *px, int w, int h, void *user)
{
    (void)user;
    memcpy(g_fb, px, (size_t)w * (size_t)h * 4);
    g_w = w; g_h = h;
    atomic_fetch_add(&g_frames, 1);
}

/* PMD85_TAPE_SHOT=prefix: the screen as prefix-<what>.ppm, for a look */
static void shot(const char *what)
{
    const char *pre = getenv("PMD85_TAPE_SHOT");
    char p[1100];
    FILE *f;
    if (!pre) return;
    snprintf(p, sizeof p, "%s-%s.ppm", pre, what);
    if (!(f = fopen(p, "wb"))) return;
    fprintf(f, "P6 %d %d 255\n", g_w, g_h);
    for (int i = 0; i < g_w * g_h; i++) {
        unsigned char c[3] = { (unsigned char)(g_fb[i] >> 16), (unsigned char)(g_fb[i] >> 8),
                               (unsigned char)g_fb[i] };
        fwrite(c, 1, 3, f);
    }
    fclose(f);
}

static void *runner(void *arg)
{
    (void)arg;
    while (atomic_load(&g_run))
        pmd85host_core_run_frame();
    return NULL;
}

static void run_start(void)
{
    atomic_store(&g_run, 1);
    pthread_create(&g_thread, NULL, runner, NULL);
}

static void run_stop(void)
{
    atomic_store(&g_run, 0);
    pthread_join(g_thread, NULL);
}

static void wait_frames(int n)
{
    const int until = atomic_load(&g_frames) + n;
    while (atomic_load(&g_frames) < until)
        sleep_ms(1);
}

/* Type text on the PMD: letters, digits, space, comma; '\n' is EOL. */
static void type(const char *text)
{
    for (; *text; text++) {
        int c = *text, hid = 0;
        if (c >= 'A' && c <= 'Z') hid = 4 + (c - 'A');
        else if (c >= '1' && c <= '9') hid = 0x1E + (c - '1');
        else if (c == '0') hid = 0x27;
        else if (c == ' ') hid = 0x2C;
        else if (c == ',') hid = 0x36;
        else if (c == '\n') hid = 0x28;
        pmd85host_inject_key(1, hid);
        wait_frames(3);
        pmd85host_inject_key(0, hid);
        wait_frames(3);
    }
}

static int tape(int cmd, const char *path)
{
    char err[256] = "";
    int rc = pmd85host_tape_command(cmd, path, err, sizeof err);
    if (rc < 0) fprintf(stderr, "  (tape command %d: %s)\n", cmd, err);
    return rc;
}

/* Run until the deck stops playing (autostop after the body), at most
 * `frames`; the frames it took, -1 if it never stopped. */
static int wait_stopped(int frames)
{
    pmd85host_tape_state st;
    const int start = atomic_load(&g_frames);
    wait_frames(5);
    while (atomic_load(&g_frames) - start < frames) {
        pmd85host_tape_state_get(&st);
        if (!st.playing) return atomic_load(&g_frames) - start;
        sleep_ms(2);
    }
    return -1;
}

static uint8_t pattern(int i) { return (uint8_t)(i * 7 + 3); }

static int memory_has_pattern(uint16_t at)
{
    for (int i = 0; i < 256; i++)
        if (pmd85host_dbg_peek((uint16_t)(at + i)) != pattern(i)) return 0;
    return 1;
}

static void put16(FILE *f, unsigned v) { fputc(v & 0xFF, f); fputc(v >> 8, f); }

/* A .ptp file: [63][FF*16 00*16 55*16, header 14 + CRC], [len][256 bytes + CRC]. */
static void write_file(FILE *f, int number, const char *name)
{
    uint8_t h[15] = { (uint8_t)number, '?', 0x00, 0x70, 0xFF, 0x00, ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 0 };
    uint8_t crc = 0;
    memcpy(h + 6, name, strlen(name) < 8 ? strlen(name) : 8);
    for (int i = 0; i < 14; i++) crc += h[i];
    h[14] = crc;
    put16(f, 63);
    for (int i = 0; i < 16; i++) fputc(0xFF, f);
    for (int i = 0; i < 16; i++) fputc(0x00, f);
    for (int i = 0; i < 16; i++) fputc(0x55, f);
    fwrite(h, 1, 15, f);
    put16(f, 257);
    crc = 0;
    for (int i = 0; i < 256; i++) { fputc(pattern(i), f); crc += pattern(i); }
    fputc(crc, f);
}

static int write_tape(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    write_file(f, 5, "TEST");
    fclose(f);
    return 0;
}

/* files #1 A, #2 B, #3 C */
static int write_abc(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    write_file(f, 1, "A");
    write_file(f, 2, "B");
    write_file(f, 3, "C");
    fclose(f);
    return 0;
}

/* the deck's file numbers in order, as "1 2 3" */
static const char *order(void)
{
    static char out[64];
    pmd85host_tape_block b[16];
    int n = pmd85host_tape_blocks(b, 16, NULL), k = 0;
    out[0] = '\0';
    for (int i = 0; i < n && i < 16; i++)
        k += snprintf(out + k, sizeof out - (size_t)k, "%s%d", i ? " " : "", b[i].number);
    return out;
}

static int edit(int cmd, int idx, const int *sel, int nsel)
{
    char err[256] = "";
    int rc = pmd85host_tape_edit(cmd, idx, sel, nsel, err, sizeof err);
    if (rc < 0) fprintf(stderr, "  (tape edit %d: %s)\n", cmd, err);
    return rc;
}

int main(void)
{
    char tapefile[1200], saved[1200], junk[1200], abc[1200], edited[1200], romdir[900];
    pmd85host_config c;
    pmd85host_tape_state st;

    if (test_mkdtemp(g_dir, sizeof g_dir, "pmd85-tape") != 0) return 77;
    snprintf(romdir, sizeof romdir, "%s", g_dir);
    for (int i = 0; i < pmd85_embedded_rom_count; i++) {
        char p[1300];
        snprintf(p, sizeof p, "%s/%s", romdir, pmd85_embedded_roms[i].name);
        FILE *f = fopen(p, "wb");
        if (!f) return 77;
        fwrite(pmd85_embedded_roms[i].data, 1, pmd85_embedded_roms[i].size, f);
        fclose(f);
    }
    snprintf(tapefile, sizeof tapefile, "%s/test.ptp", g_dir);
    snprintf(saved, sizeof saved, "%s/saved.ptp", g_dir);
    snprintf(junk, sizeof junk, "%s/junk.ptp", g_dir);
    snprintf(abc, sizeof abc, "%s/abc.ptp", g_dir);
    snprintf(edited, sizeof edited, "%s/edited.ptp", g_dir);
    if (write_tape(tapefile) != 0 || write_abc(abc) != 0) return 77;
    {
        FILE *f = fopen(junk, "wb");
        fputs("not a tape", f);
        fclose(f);
    }

    pmd85host_set_frame_sink(sink, NULL);
    pmd85host_config_defaults(&c);
    c.model = PMD85_MODEL_2A;
    snprintf(c.monitor, sizeof c.monitor, "%s/monit2A.rom", romdir);
    c.rom_module = PMD85_ROMMOD_NONE;
    if (!pmd85host_core_start(&c)) {
        fprintf(stderr, "start failed: %s\n", pmd85host_last_error());
        return 77;
    }
    run_start();
    wait_frames(150);                           /* the monitor's prompt */

    /* errors */
    CHECK(tape(PMD85_TAPE_PLAY, NULL) < 0, "PLAY with no tape succeeded");
    CHECK(tape(PMD85_TAPE_LOAD, junk) < 0, "a non-tape loaded");
    pmd85host_tape_state_get(&st);
    CHECK(st.blocks == 0, "a refused tape left %d blocks", st.blocks);

    /* flash load */
    pmd85host_tape_options(1, 0);
    CHECK(tape(PMD85_TAPE_LOAD, tapefile) == 0, "LOAD");
    wait_frames(2);
    pmd85host_tape_state_get(&st);
    /* the deck's block is a file: header and body */
    CHECK(st.blocks == 1 && st.current == 0, "deck: %d blocks, at %d", st.blocks, st.current);
    CHECK(!strcmp(st.block, "005/? TEST"), "block name '%s'", st.block);
    CHECK(!strcmp(st.path, tapefile), "path '%s'", st.path);
    type("MGLD 05\n");
    CHECK(tape(PMD85_TAPE_PLAY, NULL) == 0, "PLAY");
    {
        int took = wait_stopped(500);
        CHECK(took >= 0, "flash load: the deck never stopped");
        printf("flash load: %d frames\n", took);
        /* upstream flashes the body only: the header still plays in real
         * time (2.8 s leader + 63 bytes at 1200 Bd = ~170 frames) */
        CHECK(took >= 0 && took < 200, "flash load took %d frames", took);
    }
    run_stop();
    shot("flash");
    CHECK(memory_has_pattern(0x7000), "flash load: the bytes are not at 7000h");

    /* real time, on a rebuilt machine (the tape stays in) */
    pmd85host_tape_options(0, 0);
    if (!pmd85host_core_start(&c)) return 77;   /* RAM starts over */
    run_start();
    wait_frames(150);
    CHECK(!memory_has_pattern(0x7000), "the bytes survived a rebuild");
    pmd85host_tape_state_get(&st);
    CHECK(st.blocks == 1, "the tape did not stay in the deck (%d blocks)", st.blocks);
    CHECK(tape(PMD85_TAPE_REWIND, NULL) == 0, "REWIND");
    type("MGLD 05\n");
    CHECK(tape(PMD85_TAPE_PLAY, NULL) == 0, "PLAY");
    {
        int took = wait_stopped(1500);
        printf("real time: %d frames\n", took);
        CHECK(took > 280, "real-time load was too quick (%d frames): flash load?", took);
    }
    run_stop();
    CHECK(memory_has_pattern(0x7000), "real time: the bytes are not at 7000h");

    /* recording into an empty deck, then Save As */
    run_start();
    CHECK(tape(PMD85_TAPE_EJECT, NULL) == 0, "EJECT");
    type("MGSV 07 7000,70FF\n");
    {
        int ok = 0;
        for (int i = 0; i < 1500 && !ok; i += 10) {
            wait_frames(10);
            pmd85host_tape_state_get(&st);
            ok = st.recorded && st.blocks >= 1;
        }
        CHECK(ok, "MGSV recorded %d blocks (recorded %d)", st.blocks, st.recorded);
    }
    CHECK(st.path[0] == '\0', "a recording-only tape has a path '%s'", st.path);
    CHECK(tape(PMD85_TAPE_SAVE, saved) == 0, "SAVE");
    wait_frames(2);
    pmd85host_tape_state_get(&st);
    CHECK(!st.recorded && !strcmp(st.path, saved), "after Save As: recorded %d, path '%s'",
          st.recorded, st.path);
    run_stop();
    {
        static uint8_t b[4096];
        FILE *f = fopen(saved, "rb");
        size_t n = f ? fread(b, 1, sizeof b, f) : 0;
        if (f) fclose(f);
        /* [63][leader 48][#7 type start=7000 len=00FF ...][len 257][bytes] */
        CHECK(n >= 2 + 63 + 2 + 257, "saved tape is %zu bytes", n);
        if (n >= 2 + 63 + 2 + 257) {
            const uint8_t *h = b + 2 + 48;
            CHECK(b[0] == 63 && h[0] == 7 && h[2] == 0x00 && h[3] == 0x70 &&
                  h[4] == 0xFF && h[5] == 0x00, "saved header: #%d at %02X%02X len %02X%02X",
                  h[0], h[3], h[2], h[5], h[4]);
            int same = 1;
            for (int i = 0; i < 256; i++)
                if (b[2 + 63 + 2 + i] != pattern(i)) same = 0;
            CHECK(same, "saved body is not the bytes from 7000h");
        }
    }

    /* the block list: seek, marker, move, delete, append, save */
    run_start();
    CHECK(tape(PMD85_TAPE_LOAD, abc) == 0, "LOAD abc");
    wait_frames(2);
    CHECK(!strcmp(order(), "1 2 3"), "abc: %s", order());
    {
        pmd85host_tape_block b[4];
        int n = pmd85host_tape_blocks(b, 4, NULL);
        CHECK(n == 3 && !strcmp(b[1].name, "002/? B") && b[1].start == 0x7000 &&
              b[1].length == 256 && b[1].number == 2 && !b[1].bad, "block 1: '%s' %d %d %d", b[1].name,
              b[1].start, b[1].length, b[1].bad);
    }
    CHECK(edit(PMD85_TAPE_SEEK, 1, NULL, 0) == 0, "SEEK 1");
    CHECK(edit(PMD85_TAPE_MARK, 0, NULL, 0) < 0, "a marker before the position");
    CHECK(edit(PMD85_TAPE_MARK, 2, NULL, 0) == 0, "MARK 2");
    wait_frames(2);
    pmd85host_tape_state_get(&st);
    CHECK(st.current == 1 && st.marker == 2 && !st.recorded, "at %d, marker %d, changed %d",
          st.current, st.marker, st.recorded);
    {
        /* #2 (the position) up to the top: position and marker follow */
        unsigned before, after;
        pmd85host_tape_blocks(NULL, 0, &before);
        CHECK(edit(PMD85_TAPE_MOVE_UP, 1, NULL, 0) == 0, "MOVE UP");
        wait_frames(2);
        pmd85host_tape_blocks(NULL, 0, &after);
        CHECK(before != after, "the list serial did not change");
    }
    pmd85host_tape_state_get(&st);
    CHECK(!strcmp(order(), "2 1 3"), "after move up: %s", order());
    CHECK(st.current == 0 && st.marker == 2 && st.recorded, "after move: at %d, marker %d, "
          "changed %d", st.current, st.marker, st.recorded);
    CHECK(edit(PMD85_TAPE_MOVE_UP, 0, NULL, 0) < 0, "moved above the top");
    {
        const int gap[] = { 0, 2 }, run[] = { 0, 1 };
        CHECK(edit(PMD85_TAPE_MOVE_DOWN, -1, gap, 2) < 0, "moved a split selection");
        CHECK(edit(PMD85_TAPE_MOVE_DOWN, -1, run, 2) == 0, "MOVE DOWN 0-1");
        wait_frames(2);
        CHECK(!strcmp(order(), "3 2 1"), "after move down: %s", order());
        pmd85host_tape_state_get(&st);
        CHECK(st.current == 1 && st.marker < 0, "after move down: at %d, marker %d",
              st.current, st.marker);   /* the marker's #3 went before the position */
    }
    {
        const int sel[] = { 0, 2 };
        CHECK(edit(PMD85_TAPE_DELETE, -1, sel, 2) == 0, "DELETE 0,2");
        wait_frames(2);
        CHECK(!strcmp(order(), "2"), "after delete: %s", order());
        pmd85host_tape_state_get(&st);
        CHECK(st.blocks == 1 && st.current == 0, "after delete: %d blocks, at %d", st.blocks,
              st.current);
    }
    CHECK(tape(PMD85_TAPE_APPEND, junk) < 0, "appended a non-tape");
    CHECK(tape(PMD85_TAPE_APPEND, abc) == 0, "APPEND abc");
    wait_frames(2);
    CHECK(!strcmp(order(), "2 1 2 3"), "after append: %s", order());
    CHECK(tape(PMD85_TAPE_SAVE, edited) == 0, "SAVE edited");
    CHECK(tape(PMD85_TAPE_EJECT, NULL) == 0, "EJECT");
    CHECK(tape(PMD85_TAPE_LOAD, edited) == 0, "LOAD edited");
    wait_frames(2);
    CHECK(!strcmp(order(), "2 1 2 3"), "saved and loaded: %s", order());
    CHECK(tape(PMD85_TAPE_PLAY, NULL) == 0, "PLAY");
    CHECK(edit(PMD85_TAPE_DELETE, 0, NULL, 0) < 0, "an edit while playing");
    CHECK(tape(PMD85_TAPE_STOP, NULL) == 0, "STOP");
    run_stop();

    pmd85host_core_stop();
    test_rmtree(g_dir);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("tape: ok\n");
    return 0;
}
