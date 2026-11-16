/*
 * Becker port for the PMD 85: DriveWire over TCP to FujiNet.
 *
 * Two I/O ports, decoded as 3Eh/3Fh (address 3Eh, mask FEh -- nothing else on
 * the PMD 85 bus lives there):
 *
 *   3Eh  status  bit 1 set = a received byte is waiting
 *   3Fh  data    read: the next received byte; write: send a byte
 *
 * The PMD connects *out* to FujiNet, which runs the DriveWire server on the
 * loopback port (65505 for this app). Like XRoar's patched becker.c in the
 * CoCo app, the connection is lazy: a FujiNet that is not up yet (or is
 * restarted) is retried in the background, and the machine simply sees no data
 * until it is.
 *
 * ROM-module download. FujiNet CONFIG for the PMD mounts a ROM module image
 * (.rmm/.mrm) with the FujiNet command E2 F8 03 <slot>. The device watches the
 * CPU's traffic for that sequence; when it sees it, it takes the link over,
 * reads drive 3 from FujiNet itself in 32 KB READEX blocks (FujiNet's MRM media
 * type serves 32 KB blocks for exactly this) until DriveWire EOF, and loads
 * the result into the MegaModule. Whatever the CPU writes meanwhile is held
 * and sent afterwards. This is the protocol of the GPMD85Emulator becker-port
 * branch (BeckerPort.cpp), restructured for a non-blocking, emulator-thread-
 * driven host.
 *
 * Emulator thread only, except LinkUp()/RxPending(), which are atomics.
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

#ifndef PMD85_BECKER_H
#define PMD85_BECKER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "PeripheralDevice.h"
#include "RomMegaModule.h"

#define PMD85_BECKER_ADR    0x3E
#define PMD85_BECKER_MASK   0xFE
#define PMD85_BECKER_STATUS 0x3E
#define PMD85_BECKER_DATA   0x3F

class PmdBecker : public PeripheralDevice
{
public:
    typedef void (*TraceFn)(int dir, const uint8_t *buf, size_t len);

    PmdBecker(const char *host, int port);
    virtual ~PmdBecker();

    virtual void ResetDevice(int ticks) override;
    virtual void WriteToDevice(BYTE port, BYTE value, int ticks) override;
    virtual BYTE ReadFromDevice(BYTE port, int ticks) override;

    /* Where a downloaded ROM module goes; NULL when no MegaModule is in. */
    void SetMegaModule(RomMegaModule *mega) { megaModule = mega; }

    /* Once per frame, before the frame runs: connect/retry, flush, receive,
     * and advance a ROM download; also refills the per-frame wait budget. */
    void FramePoll();

    bool LinkUp() const { return linkUp.load(std::memory_order_relaxed); }
    int  RxPending() const { return rxPending.load(std::memory_order_relaxed); }
    int  RomLoads(int *lastSize) const {
        if (lastSize) *lastSize = romLastSize.load(std::memory_order_relaxed);
        return romLoads.load(std::memory_order_relaxed);
    }

    static void SetTrace(TraceFn fn);

private:
    enum State { ST_DOWN, ST_CONNECTING, ST_UP, ST_LOADING };
    enum RomStep { RS_FLUSH, RS_CMD, RS_BLOCK, RS_SUM, RS_RC };

    std::string host;
    int port;
    State state;
    long long fd;            /* socket handle (SOCKET on Windows) */
    long long retryAtMs;

    std::vector<uint8_t> tx;       /* CPU -> FujiNet, not yet sent */
    std::vector<uint8_t> held;     /* CPU writes during a ROM download */
    std::vector<uint8_t> rx;       /* FujiNet -> CPU, not yet read */
    size_t rxPos;

    int  emptyPolls;         /* status reads with nothing to read, since a pump */
    bool awaitingReply;      /* sent something, nothing back yet */
    long long waitBudgetUs;  /* how long the CPU may stall on replies this frame */

    /* the mount-ROM-module sniffer */
    int monIndex;
    /* the download */
    RomStep romStep;
    int romBlock;
    size_t romOffset;
    uint8_t romCmd[5];
    uint8_t romSum[2];
    std::vector<uint8_t> romBlockBuf;
    std::vector<uint8_t> romData;
    RomMegaModule *megaModule;

    std::atomic<bool> linkUp;
    std::atomic<int>  rxPending;
    std::atomic<int>  romLoads;
    std::atomic<int>  romLastSize;

    void StartConnect();
    void CheckConnect();
    void Drop(const char *why);
    bool Flush();
    bool Receive(int waitMs);
    void Pump(bool mayWait);
    bool MonitorByte(int value, bool input);
    void BeginRomLoad();
    void RomLoadStep(int waitMs);
    void EndRomLoad(bool ok);
    long ReadSock(uint8_t *buf, size_t len, int waitMs);
    long WriteSock(const uint8_t *buf, size_t len);
    void UpdatePending();
};

#endif /* PMD85_BECKER_H */
