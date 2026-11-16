/*
 * Becker port for the PMD 85 -- see pmd85_becker.h.
 *
 * Copyright (C) 2024-2026 Jan Krupa, FujiNet Go contributors
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "pmd85_becker.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
typedef SOCKET sock_t;
#  define SOCK_INVALID INVALID_SOCKET
#  define sock_close closesocket
#  define sock_errno() WSAGetLastError()
#  define SOCK_WOULDBLOCK(e) ((e) == WSAEWOULDBLOCK)
#  define SOCK_INPROGRESS(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS)
#  define poll WSAPoll
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
typedef int sock_t;
#  define SOCK_INVALID (-1)
#  define sock_close ::close
#  define sock_errno() errno
#  define SOCK_WOULDBLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#  define SOCK_INPROGRESS(e) ((e) == EINPROGRESS)
#endif

#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0
#endif

/* Retry a refused/failed connection this often. FujiNet's DriveWire listener
 * comes up about a second after the runtime starts, and a restarted FujiNet
 * should be picked up promptly -- but not hammered. */
#define RETRY_MS          1000
/* Give up on a connect() that has neither succeeded nor failed by then. */
#define CONNECT_TIMEOUT_MS 2000
/* The CPU may stall this long per frame waiting for replies (see Pump). A
 * frame is 20 ms of wall-clock time and runs in about 1 ms, so this keeps the
 * machine at full speed while letting DriveWire transactions complete inside
 * the frame that started them instead of one frame later. */
#define WAIT_BUDGET_US    10000
/* Each single wait for a reply. */
#define REPLY_WAIT_MS     2
/* Status reads with an empty receive buffer between two non-waiting recv()s:
 * a polling loop (IN 3Eh / ANI 2 / JZ) is ~27 T-states, so 16 is ~0.2 ms of
 * emulated time -- cheap, and still prompt. */
#define EMPTY_POLLS_PER_RECV 16
/* A download may wait this long per frame for FujiNet (the CPU is idle). */
#define ROM_WAIT_MS       15
#define ROM_BLOCK_SIZE    32768   /* one MegaModule page */
#define ROM_DRIVE         3
#define DW_OP_READEX      0xD2    /* 'R' + 128 */
#define DW_E_EOF          211

static std::atomic<PmdBecker::TraceFn> g_trace{nullptr};

void PmdBecker::SetTrace(TraceFn fn) { g_trace.store(fn); }

static void trace(int dir, const uint8_t *buf, size_t len)
{
    PmdBecker::TraceFn fn = g_trace.load();
    if (fn) fn(dir, buf, len);
}

static long long now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static long long now_us()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

static void net_init_once()
{
#if defined(_WIN32)
    static bool done = false;
    if (!done) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        done = true;
    }
#endif
}

static bool set_nonblocking(sock_t s)
{
#if defined(_WIN32)
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
#else
    int fl = fcntl(s, F_GETFL, 0);
    return fl >= 0 && fcntl(s, F_SETFL, fl | O_NONBLOCK) == 0;
#endif
}

/* ---- the mount-ROM-module sniffer -----------------------------------------
 * The FujiNet command the PMD's CONFIG issues to mount a ROM module, as the
 * bytes cross the port: OP_FUJI E2 00 (status), the reply 01, then
 * E2 F8 03 <slot>. Matching the last byte starts the download. */
enum { MON_OUT = 0x000, MON_IN = 0x100, MON_STOP = 0x200, MON_ANY = 0x1000 };
static const int k_mon_pattern[] = {
    MON_OUT | 0xE2, MON_OUT | 0x00, MON_IN | 0x01,
    MON_OUT | 0xE2, MON_OUT | 0xF8, MON_OUT | 0x03, MON_OUT | MON_ANY,
    MON_STOP
};

bool PmdBecker::MonitorByte(int value, bool input)
{
    int pat = k_mon_pattern[monIndex];
    int dir = pat & 0xF00;
    bool dir_ok = input ? dir == MON_IN : dir == MON_OUT;
    if (dir_ok && ((pat & MON_ANY) || (pat & 0xFF) == value)) {
        if ((k_mon_pattern[++monIndex] & 0xF00) == MON_STOP) {
            monIndex = 0;
            return true;
        }
        return false;
    }
    /* A mismatch may itself be the first byte of the pattern. */
    monIndex = 0;
    if (!input && value == (k_mon_pattern[0] & 0xFF))
        monIndex = 1;
    return false;
}

/* ---- construction ---------------------------------------------------------- */

PmdBecker::PmdBecker(const char *h, int p)
    : host(h && *h ? h : "127.0.0.1"), port(p > 0 ? p : 65505),
      state(ST_DOWN), fd((long long)SOCK_INVALID), retryAtMs(0),
      rxPos(0), emptyPolls(0), awaitingReply(false),
      waitBudgetUs(WAIT_BUDGET_US), monIndex(0), romStep(RS_FLUSH),
      romBlock(0), romOffset(0), megaModule(nullptr),
      linkUp(false), rxPending(0), romLoads(0), romLastSize(0)
{
    memset(romCmd, 0, sizeof romCmd);
    memset(romSum, 0, sizeof romSum);
    net_init_once();
    StartConnect();
}

PmdBecker::~PmdBecker()
{
    if ((sock_t)fd != SOCK_INVALID)
        sock_close((sock_t)fd);
}

void PmdBecker::ResetDevice(int)
{
    /* A machine reset does not drop the link: FujiNet keeps its state across a
     * PMD reset exactly as it does across a real one. Only what the CPU had in
     * flight is forgotten. */
    tx.clear();
    rx.clear();
    rxPos = 0;
    monIndex = 0;
    awaitingReply = false;
    if (state == ST_LOADING)
        EndRomLoad(false);
    UpdatePending();
}

void PmdBecker::UpdatePending()
{
    rxPending.store((int)(rx.size() - rxPos), std::memory_order_relaxed);
}

/* ---- connection ------------------------------------------------------------ */

void PmdBecker::StartConnect()
{
    char portstr[16];
    struct addrinfo hints, *res = nullptr;
    sock_t s;

    retryAtMs = now_ms() + RETRY_MS;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof portstr, "%d", port);
    if (getaddrinfo(host.c_str(), portstr, &hints, &res) != 0 || !res) {
        state = ST_DOWN;
        return;
    }
    s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == SOCK_INVALID || !set_nonblocking(s)) {
        if (s != SOCK_INVALID) sock_close(s);
        freeaddrinfo(res);
        state = ST_DOWN;
        return;
    }
#if defined(__APPLE__)
    {
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
    }
#endif
    {
        int on = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
    }
    int rc = connect(s, res->ai_addr, (socklen_t)res->ai_addrlen);
    freeaddrinfo(res);
    fd = (long long)s;
    if (rc == 0) {
        state = ST_UP;
        linkUp.store(true);
        trace(1, nullptr, 0);
    } else if (SOCK_INPROGRESS(sock_errno())) {
        state = ST_CONNECTING;
        retryAtMs = now_ms() + CONNECT_TIMEOUT_MS;
    } else {
        sock_close(s);
        fd = (long long)SOCK_INVALID;
        state = ST_DOWN;
        retryAtMs = now_ms() + RETRY_MS;
    }
}

void PmdBecker::CheckConnect()
{
    struct pollfd p;
    p.fd = (sock_t)fd;
    p.events = POLLOUT;
    p.revents = 0;
    int n = poll(&p, 1, 0);
    if (n > 0) {
        int err = 0;
        socklen_t len = sizeof err;
        getsockopt((sock_t)fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
        if (err == 0 && (p.revents & POLLOUT)) {
            state = ST_UP;
            linkUp.store(true);
            trace(1, nullptr, 0);
            return;
        }
        Drop(nullptr);
        return;
    }
    if (now_ms() >= retryAtMs)
        Drop(nullptr);
}

void PmdBecker::Drop(const char *why)
{
    if (why)
        fprintf(stderr, "pmd85 becker: %s\n", why);
    if ((sock_t)fd != SOCK_INVALID)
        sock_close((sock_t)fd);
    fd = (long long)SOCK_INVALID;
    if (state == ST_LOADING)
        EndRomLoad(false);
    state = ST_DOWN;
    linkUp.store(false);
    retryAtMs = now_ms() + RETRY_MS;
    tx.clear();
    rx.clear();
    rxPos = 0;
    awaitingReply = false;
    monIndex = 0;
    UpdatePending();
}

/* ---- raw socket I/O --------------------------------------------------------
 * Both return the byte count, 0 for "nothing now", -1 after dropping the
 * link. */

long PmdBecker::ReadSock(uint8_t *buf, size_t len, int waitMs)
{
    if (waitMs > 0) {
        struct pollfd p;
        p.fd = (sock_t)fd;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, waitMs) <= 0)
            return 0;
    }
    long n = (long)recv((sock_t)fd, (char *)buf, (int)len, 0);
    if (n > 0) return n;
    if (n == 0) {
        Drop("FujiNet closed the connection");
        return -1;
    }
    if (SOCK_WOULDBLOCK(sock_errno())) return 0;
    Drop("receive failed");
    return -1;
}

long PmdBecker::WriteSock(const uint8_t *buf, size_t len)
{
    long n = (long)send((sock_t)fd, (const char *)buf, (int)len, MSG_NOSIGNAL);
    if (n >= 0) return n;
    if (SOCK_WOULDBLOCK(sock_errno())) return 0;
    Drop("send failed");
    return -1;
}

/* Send what the CPU has written. True when nothing is left to send. */
bool PmdBecker::Flush()
{
    if (state != ST_UP && state != ST_LOADING) return tx.empty();
    while (!tx.empty()) {
        long n = WriteSock(tx.data(), tx.size());
        if (n <= 0) return false;
        trace(1, tx.data(), (size_t)n);
        tx.erase(tx.begin(), tx.begin() + n);
    }
    return true;
}

/* Pull what FujiNet has sent into rx. True when something arrived. */
bool PmdBecker::Receive(int waitMs)
{
    uint8_t buf[4096];
    bool got = false;
    if (state != ST_UP) return false;
    if (rxPos > 0 && rxPos == rx.size()) {
        rx.clear();
        rxPos = 0;
    }
    for (;;) {
        long n = ReadSock(buf, sizeof buf, waitMs);
        if (n <= 0) break;
        rx.insert(rx.end(), buf, buf + n);
        got = true;
        waitMs = 0;
        if ((size_t)n < sizeof buf) break;
    }
    if (got) awaitingReply = false;
    UpdatePending();
    return got;
}

/* The CPU is asking for data and there is none buffered.
 *
 * The emulator runs a whole 20 ms frame in about a millisecond and sleeps
 * the rest, so a reply that arrives "immediately" in real time usually lands
 * after the burst -- and a CPU spinning on the status port would only see it
 * next frame, costing one frame per DriveWire transaction. When the CPU has
 * just sent something (a request), wait briefly for the reply right here, as
 * the real machine would; the per-frame budget bounds the damage when there
 * is no reply coming. */
void PmdBecker::Pump(bool mayWait)
{
    Flush();
    if (state != ST_UP) return;
    if (mayWait && awaitingReply && waitBudgetUs > 0) {
        long long t0 = now_us();
        bool got = Receive(REPLY_WAIT_MS);
        waitBudgetUs -= now_us() - t0;
        if (!got)
            awaitingReply = false;  /* no reply coming: stop waiting for it */
        return;
    }
    Receive(0);
}

/* ---- the CPU's side --------------------------------------------------------- */

void PmdBecker::WriteToDevice(BYTE p, BYTE value, int)
{
    if (p != PMD85_BECKER_DATA)
        return;
    if (state == ST_LOADING) {
        held.push_back(value);
        return;
    }
    if (state != ST_UP)
        return;  /* nothing on the other end: the byte is lost, as on a cable */
    tx.push_back(value);
    awaitingReply = true;
    if (MonitorByte(value, false)) {
        BeginRomLoad();
        return;
    }
    if (tx.size() >= 512)
        Flush();
}

BYTE PmdBecker::ReadFromDevice(BYTE p, int)
{
    if (state == ST_LOADING) {
        RomLoadStep(0);
        return 0x00;
    }
    if (p == PMD85_BECKER_STATUS) {
        if (rxPos < rx.size())
            return 0x02;
        if (state == ST_DOWN) {
            if (now_ms() >= retryAtMs) StartConnect();
            return 0x00;
        }
        if (state == ST_CONNECTING) {
            CheckConnect();
            return 0x00;
        }
        if (awaitingReply || ++emptyPolls >= EMPTY_POLLS_PER_RECV) {
            emptyPolls = 0;
            Pump(true);
        }
        return rxPos < rx.size() ? 0x02 : 0x00;
    }
    if (p == PMD85_BECKER_DATA) {
        if (rxPos >= rx.size() && state == ST_UP)
            Pump(true);
        if (rxPos < rx.size()) {
            uint8_t v = rx[rxPos++];
            trace(0, &v, 1);
            UpdatePending();
            /* the sniffer needs the reply byte too; the pattern ends on an
             * OUT, so this never completes a match itself */
            MonitorByte(v, true);
            return v;
        }
        return 0x00;
    }
    return 0xFF;
}

void PmdBecker::FramePoll()
{
    waitBudgetUs = WAIT_BUDGET_US;
    switch (state) {
    case ST_DOWN:
        if (now_ms() >= retryAtMs) StartConnect();
        break;
    case ST_CONNECTING:
        CheckConnect();
        break;
    case ST_UP:
        Flush();
        if (state == ST_UP) Receive(0);
        break;
    case ST_LOADING:
        RomLoadStep(ROM_WAIT_MS);
        break;
    }
}

/* ---- ROM-module download ---------------------------------------------------- */

void PmdBecker::BeginRomLoad()
{
    fprintf(stderr, "pmd85 becker: ROM module mount detected, downloading\n");
    state = ST_LOADING;
    romStep = RS_FLUSH;
    romBlock = 0;
    romOffset = 0;
    romData.clear();
    romBlockBuf.assign(ROM_BLOCK_SIZE, 0);
    held.clear();
    awaitingReply = false;
    RomLoadStep(0);
}

void PmdBecker::EndRomLoad(bool ok)
{
    if (ok && megaModule && !romData.empty()) {
        megaModule->LoadRom((unsigned)romData.size(), romData.data());
        romLastSize.store((int)romData.size());
        romLoads.fetch_add(1);
        fprintf(stderr, "pmd85 becker: ROM module loaded (%zu bytes)\n",
                romData.size());
    } else if (ok) {
        fprintf(stderr, "pmd85 becker: ROM module downloaded but no MegaModule "
                        "is inserted to take it\n");
    } else {
        fprintf(stderr, "pmd85 becker: ROM module download failed\n");
    }
    romData.clear();
    romData.shrink_to_fit();
    romBlockBuf.clear();
    romBlockBuf.shrink_to_fit();
    if (state == ST_LOADING) {
        state = (sock_t)fd != SOCK_INVALID ? ST_UP : ST_DOWN;
        /* what the CPU wrote while we had the link */
        tx.insert(tx.end(), held.begin(), held.end());
        if (!tx.empty()) awaitingReply = true;
    }
    held.clear();
}

/* One non-blocking (or briefly waiting) step of the download state machine:
 * READEX drive 3, block n -> 32 KB -> our checksum -> FujiNet's result code;
 * 0 = more to come, 211 = end of image. */
void PmdBecker::RomLoadStep(int waitMs)
{
    long long deadline = now_ms() + waitMs;
    for (;;) {
        if (state != ST_LOADING) return;
        long n = 0;
        switch (romStep) {
        case RS_FLUSH:
            /* the mount command itself must reach FujiNet first */
            if (!Flush())
                break;
            romCmd[0] = DW_OP_READEX;
            romCmd[1] = ROM_DRIVE;
            romCmd[2] = (uint8_t)(romBlock >> 16);
            romCmd[3] = (uint8_t)(romBlock >> 8);
            romCmd[4] = (uint8_t)romBlock;
            romOffset = 0;
            romStep = RS_CMD;
            continue;
        case RS_CMD:
            n = WriteSock(romCmd + romOffset, sizeof romCmd - romOffset);
            if (n < 0) return;
            if (n > 0) trace(1, romCmd + romOffset, (size_t)n);
            romOffset += (size_t)n;
            if (romOffset == sizeof romCmd) {
                romOffset = 0;
                romStep = RS_BLOCK;
                continue;
            }
            break;
        case RS_BLOCK: {
            int w = (int)(deadline - now_ms());
            n = ReadSock(romBlockBuf.data() + romOffset,
                         ROM_BLOCK_SIZE - romOffset, w > 0 ? w : 0);
            if (n < 0) return;
            if (n > 0) trace(0, romBlockBuf.data() + romOffset, (size_t)n);
            romOffset += (size_t)n;
            if (romOffset == ROM_BLOCK_SIZE) {
                unsigned sum = 0;
                for (size_t i = 0; i < ROM_BLOCK_SIZE; i++)
                    sum += romBlockBuf[i];
                romSum[0] = (uint8_t)(sum >> 8);
                romSum[1] = (uint8_t)sum;
                romOffset = 0;
                romStep = RS_SUM;
                continue;
            }
            break;
        }
        case RS_SUM:
            n = WriteSock(romSum + romOffset, sizeof romSum - romOffset);
            if (n < 0) return;
            if (n > 0) trace(1, romSum + romOffset, (size_t)n);
            romOffset += (size_t)n;
            if (romOffset == sizeof romSum) {
                romOffset = 0;
                romStep = RS_RC;
                continue;
            }
            break;
        case RS_RC: {
            uint8_t rc;
            int w = (int)(deadline - now_ms());
            n = ReadSock(&rc, 1, w > 0 ? w : 0);
            if (n < 0) return;
            if (n == 1) {
                trace(0, &rc, 1);
                if (rc == 0) {
                    romData.insert(romData.end(), romBlockBuf.begin(),
                                   romBlockBuf.end());
                    romBlock++;
                    if (romData.size() >= (size_t)MEGA_MODULE_MAX_PAGES * ROM_BLOCK_SIZE) {
                        EndRomLoad(true);
                        return;
                    }
                    romCmd[2] = (uint8_t)(romBlock >> 16);
                    romCmd[3] = (uint8_t)(romBlock >> 8);
                    romCmd[4] = (uint8_t)romBlock;
                    romOffset = 0;
                    romStep = RS_CMD;
                    continue;
                }
                EndRomLoad(rc == DW_E_EOF);
                return;
            }
            break;
        }
        }
        /* waiting on the socket */
        if (now_ms() >= deadline) return;
        if (romStep == RS_BLOCK || romStep == RS_RC) continue;  /* ReadSock waited */
        {
            struct pollfd p;
            p.fd = (sock_t)fd;
            p.events = POLLOUT;
            p.revents = 0;
            poll(&p, 1, 1);
        }
    }
}
