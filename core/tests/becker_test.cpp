/*
 * The Becker port device against a fake DriveWire server on loopback:
 *
 *   1. it connects out (lazily, retrying) and reports the link;
 *   2. bytes the CPU writes to 3Fh reach the server, and the server's reply
 *      is visible through the status port (3Eh bit 1) and readable from 3Fh
 *      -- within the same "frame", thanks to the reply wait;
 *   3. the FujiNet mount-ROM-module sequence (E2 00, reply 01, E2 F8 03 n)
 *      makes it take the link over and READEX drive 3 in 32 KB blocks until
 *      DriveWire EOF, checksum each, and load the result into the
 *      MegaModule -- then hand the link back with the CPU's held bytes;
 *   4. a server that goes away is reconnected to.
 *
 * Exit 77 (skip) when loopback sockets are unavailable.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "pmd85_becker.h"
#include "RomMegaModule.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { std::fprintf(stderr, "FAIL: " __VA_ARGS__); std::fputc('\n', stderr); failures++; } } while (0)

static int listen_on(int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)*port);
    if (bind(fd, (sockaddr *)&a, sizeof a) != 0 || listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof a;
    getsockname(fd, (sockaddr *)&a, &len);
    *port = ntohs(a.sin_port);
    return fd;
}

static bool read_n(int fd, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

/* Drive the device the way the emulator thread does: a frame poll, then the
 * CPU spinning on the status port until data is there (bounded). */
static bool cpu_wait_data(PmdBecker &b, int frames)
{
    for (int f = 0; f < frames; f++) {
        b.FramePoll();
        for (int i = 0; i < 2000; i++)
            if (b.ReadFromDevice(PMD85_BECKER_STATUS, 0) & 0x02)
                return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

static bool wait_link(PmdBecker &b, int frames)
{
    for (int f = 0; f < frames && !b.LinkUp(); f++) {
        b.FramePoll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return b.LinkUp();
}

int main()
{
    int port = 0;
    int lfd = listen_on(&port);
    if (lfd < 0) {
        std::fprintf(stderr, "no loopback sockets\n");
        return 77;
    }

    RomMegaModule mega;
    PmdBecker becker("127.0.0.1", port);
    becker.SetMegaModule(&mega);

    /* ---- 1. connect ---- */
    int cfd = -1;
    std::thread acceptor([&] { cfd = accept(lfd, nullptr, nullptr); });
    CHECK(wait_link(becker, 100), "no link to the fake server");
    acceptor.join();
    CHECK(cfd >= 0, "server accept failed");

    /* ---- 2. a request and its reply ---- */
    {
        const uint8_t req[] = { 0x23, 0x01, 0x02 };  /* any DriveWire-ish bytes */
        std::thread server([&] {
            uint8_t got[3];
            if (read_n(cfd, got, 3) && memcmp(got, req, 3) == 0) {
                const uint8_t reply[] = { 0xAA, 0x55 };
                send(cfd, reply, sizeof reply, 0);
            }
        });
        for (uint8_t v : req) becker.WriteToDevice(PMD85_BECKER_DATA, v, 0);
        /* no FramePoll here on purpose: the status read must flush the
         * request and wait for the reply itself */
        bool ready = false;
        for (int i = 0; i < 50 && !ready; i++)
            ready = becker.ReadFromDevice(PMD85_BECKER_STATUS, 0) & 0x02;
        CHECK(ready, "reply not visible within the frame (no reply wait?)");
        if (!ready) ready = cpu_wait_data(becker, 50);
        uint8_t a = becker.ReadFromDevice(PMD85_BECKER_DATA, 0);
        if (!(becker.ReadFromDevice(PMD85_BECKER_STATUS, 0) & 0x02))
            cpu_wait_data(becker, 50);
        uint8_t b = becker.ReadFromDevice(PMD85_BECKER_DATA, 0);
        CHECK(a == 0xAA && b == 0x55, "reply bytes %02X %02X", a, b);
        CHECK(!(becker.ReadFromDevice(PMD85_BECKER_STATUS, 0) & 0x02), "phantom data");
        server.join();
    }

    /* ---- 3. a ROM-module download: two 32 KB pages ---- */
    {
        std::atomic<int> blocks_served{0};
        std::atomic<bool> server_ok{true};
        std::vector<uint8_t> after;
        std::thread server([&] {
            uint8_t b[8];
            /* the CPU's E2 00 */
            if (!read_n(cfd, b, 2) || b[0] != 0xE2 || b[1] != 0x00) { server_ok = false; return; }
            uint8_t st = 0x01;
            send(cfd, &st, 1, 0);
            /* E2 F8 03 05 */
            if (!read_n(cfd, b, 4) || b[0] != 0xE2 || b[1] != 0xF8 || b[2] != 0x03) { server_ok = false; return; }
            for (int blk = 0;; blk++) {
                uint8_t cmd[5];
                if (!read_n(cfd, cmd, 5)) { server_ok = false; return; }
                int lsn = (cmd[2] << 16) | (cmd[3] << 8) | cmd[4];
                if (cmd[0] != 0xD2 || cmd[1] != 3 || lsn != blk) { server_ok = false; return; }
                std::vector<uint8_t> data(32768, 0xFF);
                bool eof = blk == 2;
                if (!eof)
                    for (size_t i = 0; i < data.size(); i++)
                        data[i] = (uint8_t)(i * 7 + blk);
                send(cfd, data.data(), data.size(), 0);
                uint8_t sum[2];
                if (!read_n(cfd, sum, 2)) { server_ok = false; return; }
                unsigned want = 0;
                for (uint8_t v : data) want += v;
                if (sum[0] != (uint8_t)(want >> 8) || sum[1] != (uint8_t)want) { server_ok = false; return; }
                uint8_t rc = eof ? 211 : 0;
                send(cfd, &rc, 1, 0);
                if (eof) break;
                blocks_served++;
            }
            /* then whatever the CPU wrote during the download */
            uint8_t held[2];
            if (read_n(cfd, held, 2)) after.assign(held, held + 2);
        });

        becker.WriteToDevice(PMD85_BECKER_DATA, 0xE2, 0);
        becker.WriteToDevice(PMD85_BECKER_DATA, 0x00, 0);
        CHECK(cpu_wait_data(becker, 50), "no status reply");
        CHECK(becker.ReadFromDevice(PMD85_BECKER_DATA, 0) == 0x01, "status reply");
        for (uint8_t v : { 0xE2, 0xF8, 0x03, 0x05 })
            becker.WriteToDevice(PMD85_BECKER_DATA, v, 0);
        /* the CPU keeps going while the download runs */
        becker.WriteToDevice(PMD85_BECKER_DATA, 0x11, 0);
        becker.WriteToDevice(PMD85_BECKER_DATA, 0x22, 0);
        int loads = 0, last = 0;
        for (int f = 0; f < 200 && loads == 0; f++) {
            becker.FramePoll();
            CHECK(becker.ReadFromDevice(PMD85_BECKER_STATUS, 0) == 0x00 || becker.RomLoads(nullptr),
                  "data visible to the CPU during the download");
            loads = becker.RomLoads(&last);
            if (!loads) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        for (int f = 0; f < 20; f++) becker.FramePoll();  /* flush the held bytes */
        server.join();
        CHECK(server_ok, "the download did not follow the protocol");
        CHECK(blocks_served == 2, "%d blocks served", blocks_served.load());
        CHECK(loads == 1 && last == 65536, "loads %d, last %d", loads, last);
        CHECK(after.size() == 2 && after[0] == 0x11 && after[1] == 0x22,
              "held CPU bytes not sent after the download");

        /* the MegaModule has it: page 1, offset 3 through the module's 8255 */
        mega.WriteToDevice(MEGA_MODULE_ADR, 1, 0);
        CHECK(mega.GetCurrentPage() == 1, "page select");
    }

    /* ---- 4. the server goes away and comes back ---- */
    close(cfd);
    {
        uint8_t v = 0x00;
        /* the device notices on its next receive */
        for (int f = 0; f < 50 && becker.LinkUp(); f++) {
            becker.FramePoll();
            becker.WriteToDevice(PMD85_BECKER_DATA, v, 0);
            becker.ReadFromDevice(PMD85_BECKER_STATUS, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(!becker.LinkUp(), "a closed server still looks linked");
        std::thread acceptor2([&] { cfd = accept(lfd, nullptr, nullptr); });
        CHECK(wait_link(becker, 150), "did not reconnect");
        acceptor2.join();
        if (cfd >= 0) close(cfd);
    }

    close(lfd);
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("becker: ok\n");
    return 0;
}
