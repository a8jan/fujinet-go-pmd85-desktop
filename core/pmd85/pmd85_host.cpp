/*
 * Desktop host for the GPMD85Emulator machine core -- see pmd85_host.h.
 *
 * What this replaces, from the GPMD85Emulator application:
 *
 *   TEmulator      SetComputerModel/ProcessSettings/Connect*: PmdMachine::Build
 *                  CpuTimerCallback + BaseTimerCallback:      PmdMachine::RunFrame
 *   ScreenPMD85    FillBuffer + the palette/colour profiles:  PmdMachine::Render
 *   SoundDriver    the speaker/tape/MIF 85 mixer:             PmdMixer
 *   Joy4004482     reading the joystick into the GPIO ports:  PmdJoystick
 *
 * with the same machine wiring (devices, ports and signal connections) as
 * upstream's Emulator.cpp, so a staged tree from a newer GPMD85Emulator keeps
 * working as long as the chip classes keep their interfaces.
 *
 * Copyright (C) 2006-2026 Roman Borik, Martin Borik (the logic ported here)
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pmd85_host.h"
#include "pmd85_host_internal.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <condition_variable>
#include <chrono>
#include <vector>

#include "CommonUtils.h"
#include "ChipCpu8080P.h"
#include "ChipMemory12.h"
#include "ChipMemory2A.h"
#include "ChipMemory2AEx.h"
#include "ChipMemory3.h"
#include "ChipMemory3Ex.h"
#include "ChipMemoryC2717.h"
#include "ChipMemoryMato.h"
#include "SystemPIO.h"
#include "IifTimer.h"
#include "IifTape.h"
#include "IifTapePMD85.h"
#include "IifTapeMato.h"
#include "IifGPIO.h"
#include "Mif85.h"
#include "Pmd32.h"
#include "RomModule.h"
#include "RomMegaModule.h"
#include "TapeBrowser.h"
#include "Emulator.h"   /* the shim's TSettings: the deck's four settings */
#include "macros8080.h" /* FLAG_Z, FLAG_CY for the flash-load traps */

#include "pmd85_becker.h"

/* ---- log ------------------------------------------------------------------- */

static Pmd85LogSink g_log_sink = nullptr;
static void *g_log_user = nullptr;
static char g_last_error[512];

void pmd85host_set_log_sink(Pmd85LogSink sink, void *user)
{
    g_log_sink = sink;
    g_log_user = user;
}

static void host_log(int level, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (level >= 2)
        snprintf(g_last_error, sizeof g_last_error, "%s", msg);
    if (g_log_sink) {
        g_log_sink(level, msg, g_log_user);
    } else {
        static const char *const tag[] = { "info", "warning", "error" };
        fprintf(stderr, "pmd85host: %s: %s\n",
                tag[level < 0 ? 0 : (level > 2 ? 2 : level)], msg);
    }
}

const char *pmd85host_last_error(void) { return g_last_error; }

/* ---- frame sink -------------------------------------------------------------- */

static Pmd85FrameSink g_frame_sink = nullptr;
static void *g_frame_user = nullptr;

void pmd85host_set_frame_sink(Pmd85FrameSink sink, void *user)
{
    g_frame_sink = sink;
    g_frame_user = user;
}

/* ---- audio ring ----------------------------------------------------------- */

#define AUDIO_RING_SAMPLES (PMD85HOST_AUDIO_RATE * 2 / 4)  /* 250 ms stereo */
static std::mutex g_audio_mtx;
static std::condition_variable g_audio_cv;
static int16_t g_audio_ring[AUDIO_RING_SAMPLES];
static size_t g_audio_head, g_audio_tail, g_audio_count;
static std::atomic<int> g_audio_active{1};

static void push_audio(const int16_t *interleaved, int nframes)
{
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    for (int i = 0; i < nframes * 2; i++) {
        if (g_audio_count >= AUDIO_RING_SAMPLES) {
            /* overflow: drop the oldest to keep latency bounded */
            g_audio_head = (g_audio_head + 1) % AUDIO_RING_SAMPLES;
            g_audio_count--;
        }
        g_audio_ring[g_audio_tail] = interleaved[i];
        g_audio_tail = (g_audio_tail + 1) % AUDIO_RING_SAMPLES;
        g_audio_count++;
    }
    g_audio_cv.notify_all();
}

int pmd85host_fill_audio(int16_t *out, int max_samples)
{
    if (!out || max_samples <= 0) return 0;
    std::unique_lock<std::mutex> lk(g_audio_mtx);
    g_audio_cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
        return !g_audio_active.load() || g_audio_count >= (size_t)max_samples;
    });
    int produced = 0;
    while (produced < max_samples && g_audio_count > 0) {
        out[produced++] = g_audio_ring[g_audio_head];
        g_audio_head = (g_audio_head + 1) % AUDIO_RING_SAMPLES;
        g_audio_count--;
    }
    lk.unlock();
    for (int i = produced; i < max_samples; i++) out[i] = 0;
    return max_samples;
}

void pmd85host_audio_set_active(int active)
{
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    g_audio_active.store(active ? 1 : 0);
    g_audio_cv.notify_all();
}

void pmd85host_clear_audio(void)
{
    std::lock_guard<std::mutex> lk(g_audio_mtx);
    g_audio_head = g_audio_tail = g_audio_count = 0;
    g_audio_cv.notify_all();
}

/* ---- key queue ------------------------------------------------------------ */

struct KeyEvent { int down; int usage; };
static std::mutex g_key_mtx;
static std::vector<KeyEvent> g_key_queue;
static uint8_t g_key_held_ui[256];   /* what the UI side believes is down */

void pmd85host_inject_key(int down, int usage)
{
    if (usage <= 0 || usage > 255) return;
    std::lock_guard<std::mutex> lk(g_key_mtx);
    g_key_held_ui[usage] = down ? 1 : 0;
    if (g_key_queue.size() < 1024)
        g_key_queue.push_back({ down ? 1 : 0, usage });
}

void pmd85host_release_all_keys(void)
{
    std::lock_guard<std::mutex> lk(g_key_mtx);
    for (int u = 1; u < 256; u++) {
        if (g_key_held_ui[u]) {
            g_key_held_ui[u] = 0;
            g_key_queue.push_back({ 0, u });
        }
    }
}

int pmd85host_drain_keys(void (*sink)(int down, int usage))
{
    std::vector<KeyEvent> now, later;
    {
        std::lock_guard<std::mutex> lk(g_key_mtx);
        now.swap(g_key_queue);
    }
    uint8_t pressed[256] = { 0 };
    int delivered = 0;
    for (size_t i = 0; i < now.size(); i++) {
        const KeyEvent &e = now[i];
        if (!e.down && pressed[e.usage]) {
            /* pressed in this same drain: keep the release for the next frame
             * so the machine's keyboard scan sees the key at least once --
             * and everything after it too, to keep the order */
            later.assign(now.begin() + (long)i, now.end());
            break;
        }
        if (e.down) pressed[e.usage] = 1;
        sink(e.down, e.usage);
        delivered++;
    }
    if (!later.empty()) {
        std::lock_guard<std::mutex> lk(g_key_mtx);
        later.insert(later.end(), g_key_queue.begin(), g_key_queue.end());
        g_key_queue.swap(later);
    }
    return delivered;
}

/* ---- joystick / state atomics --------------------------------------------- */

static std::atomic<unsigned> g_joy[2];
static std::atomic<int> g_led{0};
static std::atomic<int> g_mega_page{-1};

void pmd85host_set_joystick(int port, unsigned bits)
{
    if (port == 0 || port == 1) g_joy[port].store(bits & 0x1F);
}

int pmd85host_led_state(void) { return g_led.load(std::memory_order_relaxed); }
int pmd85host_mega_page(void) { return g_mega_page.load(std::memory_order_relaxed); }

/* ---- pending commands (UI -> emulator thread) ------------------------------ */

static std::mutex g_cmd_mtx;
static std::atomic<int> g_cmd_color{-1};
static bool g_cmd_reset;
static bool g_cmd_reconfig;
static pmd85host_config g_cmd_config;

void pmd85host_core_reset(void)
{
    std::lock_guard<std::mutex> lk(g_cmd_mtx);
    g_cmd_reset = true;
}

void pmd85host_set_color_profile(int profile)
{
    if (profile >= 0 && profile < PMD85_COLOR_COUNT)
        g_cmd_color.store(profile);
}

void pmd85host_core_reconfigure(const pmd85host_config *cfg)
{
    if (!cfg) return;
    std::lock_guard<std::mutex> lk(g_cmd_mtx);
    g_cmd_config = *cfg;
    g_cmd_reconfig = true;
}

/* ---- config defaults ------------------------------------------------------- */

void pmd85host_config_defaults(pmd85host_config *c)
{
    memset(c, 0, sizeof *c);
    c->model = PMD85_MODEL_2A;
    c->rom_module = PMD85_ROMMOD_NONE;
    c->becker_port = 65505;
    snprintf(c->becker_host, sizeof c->becker_host, "127.0.0.1");
    c->color_profile = PMD85_COLOR_STANDARD;
    /* upstream's default "rgb" palette for CP_COLOR */
    c->attr[0] = LIME;
    c->attr[1] = RED;
    c->attr[2] = BLUE;
    c->attr[3] = FUCHSIA;
    c->volume = 96;
    c->exchange_zy = 1;
    c->numpad = 1;
    c->mato_ctrl = 1;
}

/* ---- debugger hook ---------------------------------------------------------- */

static Pmd85InstrHook g_instr_hook = nullptr;
static void *g_instr_hook_ctx = nullptr;

void pmd85host_set_instr_hook(Pmd85InstrHook hook, void *ctx)
{
    g_instr_hook_ctx = ctx;
    g_instr_hook = hook;
}

/* ======================================================================== */
/* The mixer: SoundDriver without SDL                                       */
/* ======================================================================== */

/* Upstream's SoundDriver, keeping its sample placement and fade-out exactly
 * (the PMD's beeper and tape signals are square waves whose edges land at the
 * T-state they happened; the fade-out stops a held level from turning into a
 * DC offset), but writing S16 into the host's ring instead of queueing U8 to
 * SDL. */
class PmdMixer
{
public:
    explicit PmdMixer(int volume) : mif85(false) { SetVolume(volume); Clear(); }

    void SetVolume(int vol)
    {
        totalVolume = vol & 0x7F;
        channelVolume = totalVolume / AUDIO_BEEP_CHANNELS;
        if (channelVolume == 0 && totalVolume > 0)
            channelVolume = 1;
        channelFadeout = channelVolume == 0 ? FADEOUT_RATE
                                            : FADEOUT_RATE / channelVolume;
    }

    void EnableMIF85(bool on) { mif85 = on; }

    void Clear()
    {
        for (int c = 0; c < AUDIO_BEEP_CHANNELS; c++) {
            memset(&ch[c], 0, sizeof ch[c]);
        }
    }

    /* upstream SoundDriver::PrepareSample, connected to the beeper (SystemPIO)
     * and tape (IifTape) PrepareSample signals */
    void PrepareSample(int chn, bool state, int ticks)
    {
        if (chn < 0 || chn >= AUDIO_BEEP_CHANNELS) return;
        Channel &c = ch[chn];
        int val = state ? channelVolume : -channelVolume;
        if (c.oldVal == val)
            return;

        int curPos = (int)(((long long)ticks * SAMPS_PER_CPU_FRAME) / TCYCLES_PER_FRAME);
        for (int i = c.fillPos; i < curPos && i < SAMPS_PER_CPU_FRAME; i++)
            c.buf[i] = Fadeout(c);
        if (curPos >= 0 && curPos < SAMPS_PER_CPU_FRAME)
            c.buf[curPos] = (int8_t)val;

        c.fillPos = curPos + 1;
        c.curVal = val;
        c.oldVal = val;
    }

    /* upstream SoundDriver::PrepareBuffer: finish the frame, mix, push */
    void EndFrame()
    {
        int16_t out[SAMPS_PER_CPU_FRAME * 2];
        uint8_t saa[SAMPS_PER_CPU_FRAME * 2];
        bool haveSaa = mif85 && SAA1099 != nullptr;

        for (int c = 0; c < AUDIO_BEEP_CHANNELS; c++) {
            for (int i = ch[c].fillPos; i < SAMPS_PER_CPU_FRAME; i++)
                ch[c].buf[i] = Fadeout(ch[c]);
            ch[c].fillPos = 0;
        }
        if (haveSaa)
            SAA1099->GenerateMany(saa, SAMPS_PER_CPU_FRAME);

        for (int i = 0; i < SAMPS_PER_CPU_FRAME; i++) {
            /* SDL_MixAudioFormat scales by volume/128 */
            int beep = (ch[0].buf[i] + ch[1].buf[i]) * totalVolume / 128;
            int l = beep, r = beep;
            if (haveSaa) {
                l += ((int)saa[i * 2] - 128) * totalVolume / 128;
                r += ((int)saa[i * 2 + 1] - 128) * totalVolume / 128;
            }
            if (l > 127) l = 127;
            if (l < -128) l = -128;
            if (r > 127) r = 127;
            if (r < -128) r = -128;
            out[i * 2] = (int16_t)(l * 256);
            out[i * 2 + 1] = (int16_t)(r * 256);
        }
        push_audio(out, SAMPS_PER_CPU_FRAME);
    }

private:
    struct Channel {
        int8_t buf[SAMPS_PER_CPU_FRAME];
        int curVal, oldVal, fillPos, tick;
    };

    int8_t Fadeout(Channel &c)
    {
        c.tick += SAMPLE_TICK_INC;
        if (c.tick >= channelFadeout) {
            c.tick -= channelFadeout;
            if (c.curVal > 0) c.curVal--;
            else if (c.curVal < 0) c.curVal++;
        }
        return (int8_t)c.curVal;
    }

    Channel ch[AUDIO_BEEP_CHANNELS];
    int totalVolume, channelVolume, channelFadeout;
    bool mif85;
};

/* ======================================================================== */
/* The joystick: Joy4004482's GPIO side                                     */
/* ======================================================================== */

/* The 4004/482 interface puts the stick on a GPIO port, active low, and is
 * read when the CPU reads the port with the strobe in port C set. */
class PmdJoystick
{
public:
    PmdJoystick(IifGPIO *p, bool a, bool b) : pio(p), onA(a), onB(b)
    {
        if (onA) pio->OnCpuReadA.connect(&PmdJoystick::ReadA, this);
        if (onB) pio->OnCpuReadB.connect(&PmdJoystick::ReadB, this);
    }
    ~PmdJoystick()
    {
        pio->OnCpuReadA.disconnect(this);
        pio->OnCpuReadB.disconnect(this);
    }
    void ReadA()
    {
        BYTE v = 0xFF;
        if (pio->ReadByte(PP_PortC) & 0x10) v = (BYTE)~g_joy[0].load();
        pio->WriteByte(PP_PortA, v);
    }
    void ReadB()
    {
        BYTE v = 0xFF;
        if (pio->ReadByte(PP_PortC) & 0x01) v = (BYTE)~g_joy[1].load();
        pio->WriteByte(PP_PortB, v);
    }

private:
    IifGPIO *pio;
    bool onA, onB;
};

/* ======================================================================== */
/* The machine                                                              */
/* ======================================================================== */

static const DWORD k_palette[16] = {
    /* ScreenPMD85::InitPalette, as XRGB8888 */
    0x000000, 0xA00000, 0x00A000, 0xA0A000, 0x0000A0, 0xA000A0, 0x00A0A0, 0xA0A0A0,
    0xBFBFBF, 0xFF5050, 0x50FF50, 0xFFFF50, 0x5050FF, 0xFF50FF, 0x50FFFF, 0xFFFFFF,
};

class PmdMachine
{
public:
    explicit PmdMachine(const pmd85host_config &c);
    ~PmdMachine();

    bool ok() const { return built; }
    void RunFrame();
    void TapeTraps(WORD pc);
    void SetColorProfile(int profile) { cfg.color_profile = profile; SetColors(); }
    void Reset() { cpu->DoReset(); }

    /* the keyboard state the SystemPIO scan reads (SDL_NUM_SCANCODES wide) */
    BYTE keyBuffer[SDL_NUM_SCANCODES];

    pmd85host_config cfg;
    TComputerModel model;
    ChipCpu8080 *cpu;
    ChipMemory *memory;
    SystemPIO *systemPIO;
    IifTimer *ifTimer;
    IifTape *ifTape;
    IifGPIO *ifGpio;
    Mif85 *mif85;
    Pmd32 *pmd32;
    RomModule *romModule;
    RomMegaModule *megaModule;
    PmdBecker *becker;
    PmdJoystick *joystick;
    PmdMixer mixer;

    bool width384;
    bool blinkState;
    int blinkFrames;
    uint64_t frames;
    uint32_t fb[PMD85HOST_FB_WIDTH_MAX * PMD85HOST_FB_HEIGHT];

private:
    bool built;
    BYTE pAttr[8];
    bool blinkingEnabled;

    bool Build();
    void SetColors();
    void Render();
    void InsertRomPack();
    void InsertMegaModule();
};

PmdMachine::PmdMachine(const pmd85host_config &c)
    : cfg(c), model((TComputerModel)c.model), cpu(nullptr), memory(nullptr),
      systemPIO(nullptr), ifTimer(nullptr), ifTape(nullptr), ifGpio(nullptr),
      mif85(nullptr), pmd32(nullptr), romModule(nullptr), megaModule(nullptr),
      becker(nullptr), joystick(nullptr), mixer(c.volume), width384(false),
      blinkState(false), blinkFrames(0), frames(0), built(false),
      blinkingEnabled(false)
{
    memset(keyBuffer, 0, sizeof keyBuffer);
    memset(fb, 0, sizeof fb);
    memset(pAttr, 0, sizeof pAttr);
    if (model < CM_V1 || model > CM_LAST) model = CM_V2A;
    built = Build();
}

PmdMachine::~PmdMachine()
{
    /* the reverse of Build, mirroring TEmulator's destructor order */
    delete joystick;
    delete pmd32;  /* before ifGpio, whose signals it is connected to */
    if (ifTimer) ifTimer->EnableMIF85(false, nullptr);
    delete mif85;
    delete becker;
    delete romModule;  /* megaModule is the same object when present */
    delete cpu;
    delete systemPIO;
    delete ifTimer;
    if (ifTape) {
        if (ifTape->GetModel() == CM_MATO)
            delete (IifTapeMato *)ifTape;
        else
            delete (IifTapePMD85 *)ifTape;
    }
    delete ifGpio;
    delete memory;
}

bool PmdMachine::Build()
{
    /* ---- TEmulator::SetComputerModel ---- */
    TTapeIfType tapeIfType = TIT_V1;
    if (model == CM_V1 || model == CM_ALFA)
        tapeIfType = TIT_V1;
    else if (model < CM_MATO || model == CM_ALFA2 || model == CM_C2717)
        tapeIfType = TIT_V2;

    std::vector<BYTE> romBuff(16 * KB, 0xFF);
    int fileSize = cfg.monitor[0] ? ReadFromFile(cfg.monitor, 0, 16 * KB, romBuff.data()) : -1;
    if (fileSize <= 0) {
        host_log(2, "cannot read the monitor ROM \"%s\"", cfg.monitor);
        return false;
    }

    fileSize = (fileSize + KB - 1) & ~(KB - 1);
    BYTE romSize = (BYTE)(fileSize / KB);
    if (romSize < 4) romSize = 4;
    if (romSize > 16) romSize = 16;

    switch (model) {
    case CM_V1:
    case CM_ALFA:
    case CM_ALFA2:
    case CM_V2:
        memory = new ChipMemory12(romSize);
        break;
    case CM_V2A:
        memory = cfg.ram256k ? (ChipMemory *)new ChipMemory2AEx(romSize)
                             : (ChipMemory *)new ChipMemory2A(romSize);
        break;
    case CM_V3:
        romSize = 8;
        memory = cfg.ram256k ? (ChipMemory *)new ChipMemory3Ex(romSize)
                             : (ChipMemory *)new ChipMemory3(romSize);
        break;
    case CM_MATO:
        romSize = 16;
        memory = new ChipMemoryMato(romSize, cfg.mato_allram64k != 0);
        break;
    case CM_C2717:
        romSize = 16;
        memory = new ChipMemoryC2717(romSize);
        memory->SetRemapped(true);
        break;
    default:
        return false;
    }

    if (model <= CM_V2A)
        memory->SetSplit8k(cfg.split8k != 0);

    cpu = new ChipCpu8080P(memory);

    systemPIO = new SystemPIO(model, memory);
    systemPIO->PrepareSample.connect(&PmdMixer::PrepareSample, &mixer);
    cpu->AddDevice(SYSTEM_PIO_ADR, SYSTEM_PIO_MASK, systemPIO, true);

    if (model != CM_MATO) {
        cpu->TCyclesListeners.connect(&SystemPIO::SoundService, systemPIO);

        ifGpio = new IifGPIO();
        cpu->AddDevice(IIF_GPIO_ADR, IIF_GPIO_MASK, ifGpio, true);

        ifTimer = new IifTimer(model, cpu);
        cpu->AddDevice(IIF_TIMER_ADR, IIF_TIMER_MASK, ifTimer, false);
        cpu->TCyclesListeners.connect(&IifTimer::ITimerService, ifTimer);
    }

    if (model == CM_MATO)
        ifTape = new IifTapeMato(systemPIO);
    else
        ifTape = new IifTapePMD85(model, tapeIfType);
    ifTape->PrepareSample.connect(&PmdMixer::PrepareSample, &mixer);

    if (model != CM_MATO) {
        PeripheralDevice *tapeDevice = dynamic_cast<PeripheralDevice *>((IifTapePMD85 *)ifTape);
        if (model == CM_ALFA || model == CM_ALFA2)
            cpu->AddDevice(IIF_TAPE_ADR_A, IIF_TAPE_MASK_A, tapeDevice, true);
        else
            cpu->AddDevice(IIF_TAPE_ADR, IIF_TAPE_MASK, tapeDevice, true);
    }

    if (model == CM_MATO) {
        cpu->TCyclesListeners.connect(&IifTapeMato::TapeClockService, (IifTapeMato *)ifTape);
    } else {
        IifTapePMD85 *tapeDevice = (IifTapePMD85 *)ifTape;
        if (model != CM_V1 && model != CM_ALFA) {
            int cnt = (model == CM_C2717) ? 0 : 1;
            ifTimer->Counters[cnt].OnOutChange.connect(&IifTapePMD85::TapeClockService23, tapeDevice);
        }
        cpu->TCyclesListeners.connect(&IifTapePMD85::TapeClockService123, tapeDevice);
    }

    if ((model == CM_V2A || model == CM_V3) && cfg.ram256k)
        cpu->AddDevice(MM256_REG_ADR, MM256_REG_MASK,
                       dynamic_cast<PeripheralDevice *>(memory), true);

    if (model != CM_C2717)
        systemPIO->width384 = 1;

    memory->PutRom(romBuff.data(), romSize * KB);

    /* ---- TEmulator::ConnectMIF85 ---- */
    if (cfg.mif85 && model <= CM_V3) {
        mif85 = new Mif85();
        cpu->AddDevice(MIF85_ADR, MIF85_MASK, mif85, true);
        mixer.EnableMIF85(true);
        if (ifTimer) ifTimer->EnableMIF85(true, mif85);
    }

    /* ---- TEmulator::ConnectPMD32 ---- */
    if (cfg.pmd32 && ifGpio && (model <= CM_V3 || model == CM_C2717)) {
        pmd32 = new Pmd32(ifGpio);
        cpu->TCyclesListeners.connect(&Pmd32::Disk32Service, pmd32);
        if (cfg.pmd32_sdroot[0])
            pmd32->SetExtraCommands(cfg.pmd32_extra != 0, cfg.pmd32_sdroot);
        for (int d = 0; d < 4; d++) {
            if (cfg.pmd32_drive[d][0])
                pmd32->InsertDisk(DRIVE_A + d, cfg.pmd32_drive[d], cfg.pmd32_wp[d] != 0);
        }
    }

    /* ---- the joystick (GPIO 0 is the PMD 32's when that is connected) ---- */
    if (ifGpio && (cfg.joystick_gpio0 || cfg.joystick_gpio1))
        joystick = new PmdJoystick(ifGpio, cfg.joystick_gpio0 && !pmd32,
                                   cfg.joystick_gpio1 != 0);

    /* ---- the ROM module ---- */
    if (cfg.rom_module == PMD85_ROMMOD_PACK)
        InsertRomPack();
    else if (cfg.rom_module == PMD85_ROMMOD_MEGA)
        InsertMegaModule();

    /* ---- the Becker port ---- */
    if (cfg.becker) {
        becker = new PmdBecker(cfg.becker_host, cfg.becker_port);
        becker->SetMegaModule(megaModule);
        cpu->AddDevice(PMD85_BECKER_ADR, PMD85_BECKER_MASK, becker, true);
    }

    /* ---- the keyboard options (TEmulator::ProcessSettings, PS_CONTROLS) ---- */
    systemPIO->exchZY = cfg.exchange_zy != 0;
    systemPIO->numpad = cfg.numpad != 0;
    systemPIO->extMato = cfg.mato_ctrl != 0;

    SetColors();
    cpu->DoReset();
    return true;
}

void PmdMachine::InsertRomPack()
{
    /* TEmulator::InsertRomModule */
    romModule = new RomModule();
    cpu->AddDevice(ROM_MODULE_ADR, ROM_MODULE_MASK, romModule, true);
    romModule->RemoveRomPack();

    std::vector<BYTE> buff(ROM_PACK_SIZE);
    DWORD kBadr = 0;
    for (int i = 0; i < cfg.rmm_count && i < PMD85HOST_RMM_MAX && kBadr < ROM_PACK_SIZE_KB; i++) {
        long size = FileSize(cfg.rmm[i]);
        if (size <= 0) {
            host_log(1, "ROM module file not found: \"%s\"", cfg.rmm[i]);
            continue;
        }
        int sizeKB = (int)((size + KB - 1) / KB);
        if (kBadr + sizeKB > ROM_PACK_SIZE_KB)
            sizeKB = ROM_PACK_SIZE_KB - kBadr;
        memset(buff.data(), 0xFF, (size_t)sizeKB * KB);
        if (ReadFromFile(cfg.rmm[i], 0, sizeKB * KB, buff.data()) > 0)
            romModule->InsertRom((BYTE)kBadr, (BYTE)sizeKB, buff.data());
        kBadr += sizeKB;
    }
}

void PmdMachine::InsertMegaModule()
{
    /* TEmulator::InsertRomMegaModule */
    megaModule = new RomMegaModule();
    romModule = megaModule;
    cpu->AddDevice(ROM_MODULE_ADR, ROM_MODULE_MASK, romModule, true);
    cpu->AddDevice(MEGA_MODULE_ADR, MEGA_MODULE_MASK, romModule, false);

    if (!cfg.mrm[0])
        return;  /* an empty MegaModule, for FujiNet to fill */
    long size = FileSize(cfg.mrm);
    if (size <= 0) {
        host_log(1, "MegaModule image not found: \"%s\"", cfg.mrm);
        return;
    }
    if (size > 8L * KB * KB) {
        host_log(1, "MegaModule image too large, truncated: \"%s\"", cfg.mrm);
        size = 8L * KB * KB;
    }
    std::vector<BYTE> buff((size_t)size, 0xFF);
    if (ReadFromFile(cfg.mrm, 0, (int)size, buff.data()) > 0)
        megaModule->LoadRom((unsigned)size, buff.data());
    else
        host_log(1, "cannot read the MegaModule image \"%s\"", cfg.mrm);
}

void PmdMachine::SetColors()
{
    /* ScreenPMD85::SetColorProfile + SetColorAttr */
    BYTE cAttr[4];
    for (int i = 0; i < 4; i++)
        cAttr[i] = (BYTE)((cfg.attr[i] >= 0 && cfg.attr[i] <= WHITE) ? cfg.attr[i] : WHITE);

    switch (cfg.color_profile) {
    case PMD85_COLOR_MONO:
        pAttr[0] = pAttr[1] = pAttr[2] = pAttr[3] = WHITE;
        blinkingEnabled = false;
        break;
    default:
    case PMD85_COLOR_STANDARD:
        pAttr[0] = WHITE;
        pAttr[1] = SILVER;
        pAttr[2] = WHITE;
        pAttr[3] = SILVER;
        blinkingEnabled = true;
        break;
    case PMD85_COLOR_COLOR:
        for (int i = 0; i < 4; i++)
            pAttr[i] = pAttr[i + 4] = cAttr[i];
        blinkingEnabled = false;
        break;
    case PMD85_COLOR_COLORACE:
        pAttr[0] = BLACK;
        pAttr[1] = RED;
        pAttr[2] = BLUE;
        pAttr[3] = FUCHSIA;
        pAttr[4] = LIME;
        pAttr[5] = YELLOW;
        pAttr[6] = AQUA;
        pAttr[7] = WHITE;
        blinkingEnabled = false;
        break;
    }
}

void PmdMachine::Render()
{
    /* ScreenPMD85::FillBuffer, into XRGB8888 */
    const BYTE *vram = memory->GetVramPointer();
    if (!vram) return;

    bool colorace = (cfg.color_profile == PMD85_COLOR_COLORACE);
    BYTE a[4] = { pAttr[0], pAttr[1], pAttr[2], pAttr[3] };
    if (blinkingEnabled && blinkState)
        a[2] = a[3] = 0;

    uint32_t *dst = fb;
    for (int line = 0; line < PMD85HOST_FB_HEIGHT; line++) {
        const BYTE *row = vram + line * 64;
        /* upstream counts h down from 255 and pairs a line with the one below
         * on odd h, the one above on even h */
        int h = PMD85HOST_FB_HEIGHT - 1 - line;
        for (int i = 0; i < 48; i++) {
            BYTE b = row[i];
            BYTE d = (BYTE)((b & 0xC0) >> 6);
            BYTE c;
            if (colorace) {
                const BYTE *pair = (h & 1) ? row + 64 : row - 64;
                BYTE e = (BYTE)((pair[i] & 0xC0) >> 6);
                c = pAttr[d | e | ((d & e) ? 0 : 4)];
            } else if (!width384) {
                c = a[d];
            } else {
                c = a[0];
            }
            int bits = width384 ? 8 : 6;
            for (int k = 0; k < bits; k++)
                *dst++ = k_palette[(b & (1 << k)) ? c : 0];
        }
    }
}

/* TEmulator::CpuTimerCallback's "tape flash loading - ROM entry-point
 * mappings": with a block prepared for flash load, a monitor's read-block
 * (8DC4h, EDC4h on the 85-3) or read-byte (8B6Ch / EB6Ch) routine is done
 * here at once -- the bytes into memory, CRC, registers and flags as the
 * routine leaves them -- and execution goes on at its RET. The ROM bytes
 * checked first keep it to the monitors it was written for. And the Mato's
 * tape write (8D6Ch), which upstream catches the same way. */
void PmdMachine::TapeTraps(WORD pc)
{
    WORD byte, hl, de, bc, tapeBlockLen, len, crc;
    BYTE *tapeBlock, val1, val2, val3;
    bool cy;

    if (ifTape->IsFlashLoadOn()) {
        /* block */
        if (pc == 0x8DC4 || (pc == 0xEDC4 && model == CM_V3)) {
            val1 = memory->ReadByte((pc & 0xFF00) | 0xD3);
            val2 = memory->ReadByte((pc & 0xFF00) | 0xE1);
            val3 = memory->ReadByte(0x8F89);
            if (((model != CM_MATO && val1 == 0x23 && val2 == 0xC9) ||
                 (model == CM_MATO && val1 == 0xD6 && val2 == 0x8F && val3 == 0xC9)) &&
                ifTape->GetTapeBlock(&tapeBlock, &tapeBlockLen) == true) {

                hl = cpu->GetHL();  /* HL = start address */
                de = cpu->GetDE();  /* DE = block length - 1 */
                bc = cpu->GetBC();  /* C = 0 check, C != 0 load */
                cy = ((bc & 0xFF) != 0);
                if (tapeBlock != NULL && (de + 2) <= tapeBlockLen) {
                    tapeBlockLen = (WORD)(de + 1);
                    crc = 0;
                    for (len = 0; len < tapeBlockLen; len++) {
                        if (cy == true)
                            memory->WriteByte(hl, *tapeBlock);
                        crc += *tapeBlock;
                        hl++;
                        tapeBlock++;
                    }
                    if (model == CM_V1)
                        byte = *tapeBlock;
                    else {
                        byte = 0;
                        hl -= tapeBlockLen;
                    }
                    tapeBlockLen++;
                    crc &= 0xFF;

                    cpu->SetHL(hl);
                    cpu->SetDE(0xFFFF);
                    cpu->SetBC((WORD)((crc << 8) | (bc & 0xFF)));
                    if (crc == *tapeBlock)
                        cpu->SetAF((WORD)((byte << 8) | FLAG_Z));
                    else
                        cpu->SetAF((WORD)((byte << 8) | FLAG_CY));
                    if (model == CM_MATO)
                        cpu->SetPC((WORD)0x8F89);
                    else
                        cpu->SetPC((WORD)((pc & 0xFF00) | 0xE1));

                    ifTape->AcceptTapeBlock(tapeBlockLen);
                }
            }
        }
        /* byte */
        else if ((pc == 0x8B6C || (pc == 0xEB6C && model == CM_V3)) &&
                 (model != CM_V1 && model != CM_ALFA && model != CM_MATO)) {
            val1 = memory->ReadByte(pc);
            val2 = memory->ReadByte((pc & 0xFF00) | 0x9B);
            if (val1 == 0xC5 && val2 == 0xC9) {
                BYTE b = 0;
                cy = ifTape->GetTapeByte(&b);
                cpu->SetAF((WORD)((b << 8) | ((cy == true) ? FLAG_CY : 0)));
                cpu->SetPC((WORD)((pc & 0xFF00) | 0x9B));
            }
        }
    }

    /* the tape write on Mato */
    if (model == CM_MATO && pc == 0x8D6C) {
        hl = cpu->GetHL();  /* HL = start address */
        de = cpu->GetDE();  /* DE = length - 1 */
        tapeBlockLen = (WORD)(de + 1);
        int pos = ifTape->GetSavedBlock(&tapeBlock);
        tapeBlock += pos;

        if (ifTape->GetTxState() == TP_TX_WAIT_HEAD && tapeBlockLen == 14) {
            *((WORD *)tapeBlock) = (WORD)63;
            tapeBlock += 2;
            for (int i = 0; i < 16; i++) *tapeBlock++ = 0xFF;
            for (int i = 0; i < 16; i++) *tapeBlock++ = 0;
            for (int i = 0; i < 16; i++) *tapeBlock++ = 0x55;
        } else {
            *((WORD *)tapeBlock) = (WORD)(tapeBlockLen + 1);
            tapeBlock += 2;
        }

        crc = 0;
        for (len = 0; len < tapeBlockLen; len++) {
            val1 = memory->ReadByte(hl);
            *tapeBlock = val1;
            crc += val1;
            hl++;
            tapeBlock++;
        }
        *tapeBlock = (BYTE)crc;

        ifTape->SetTxState((ifTape->GetTxState() == TP_TX_WAIT_HEAD) ? TP_TX_HEAD
                                                                     : TP_TX_BODY);
        cpu->SetHL((WORD)(hl + tapeBlockLen));
        cpu->SetDE((WORD)0);
        cpu->SetBC((WORD)(crc << 8));
        cpu->SetAF((WORD)0x0056);
        cpu->SetPC((WORD)0x8D93);
    }
}

void PmdMachine::RunFrame()
{
    /* ---- BaseTimerCallback: keyboard, Consul 384 mode ---- */
    systemPIO->ScanKeyboard(keyBuffer);
    {
        BYTE flag = systemPIO->width384;
        if ((flag & 1) && (bool)(flag & 0xFE) != width384) {
            width384 = (flag & 0xFE) != 0;
            systemPIO->width384 = (flag & 0xFE);
        }
    }

    if (becker) becker->FramePoll();

    /* ---- CpuTimerCallback ---- */
    const int tcpf = TCYCLES_PER_FRAME;
    do {
        WORD pc = cpu->GetPC();

        /* switch PMD 85-3 to the compatibility mode */
        if (pc == 0xE04C && model == CM_V3 && cfg.compat_mode && !memory->IsAllRAM())
            cpu->SetPC(0xFFF0);

        /* tape flash load and the Mato's tape save: before the debugger's
         * hook, which then sees where execution really goes on */
        if (ifTape && (ifTape->IsFlashLoadOn() || model == CM_MATO))
            TapeTraps(cpu->GetPC());

        /* The debugger's hook may park this thread (a breakpoint, a step);
         * the frame simply resumes from here when it lets go. */
        if (g_instr_hook)
            g_instr_hook(g_instr_hook_ctx, cpu->GetPC());

        cpu->DoInstruction();
    } while (cpu->GetTCycles() < tcpf);
    cpu->SetTCycles(cpu->GetTCycles() - tcpf);

    /* ---- the rest of BaseTimerCallback ---- */
    mixer.EndFrame();
    /* the blink attribute toggles every 500 ms (upstream counts wall-clock
     * milliseconds; counting frames is the same at 50 Hz, and stays right
     * when the machine runs slower or faster than real time) */
    if (++blinkFrames >= 25) {
        blinkFrames = 0;
        blinkState = !blinkState;
    }
    Render();
    frames++;
    if (g_frame_sink)
        g_frame_sink(fb, width384 ? 384 : 288, PMD85HOST_FB_HEIGHT, g_frame_user);
}

/* ======================================================================== */
/* The C API                                                                */
/* ======================================================================== */

static PmdMachine *g_machine = nullptr;

/* ---- the tape deck ----------------------------------------------------------- */

/* The deck's settings, through the shim's Emulator.h (TapeBrowser.cpp reads
 * them as Settings->TapeBrowser->...). */
static TSettings::SetTapeBrowser g_tape_settings = { false, true, false, AS_NEXTHEAD, nullptr };
static TSettings g_settings_object = { &g_tape_settings };
TSettings *Settings = &g_settings_object;

static TTapeBrowser *g_deck;              /* the emulator thread's */
static std::string g_tape_path;           /* the emulator thread's */
static std::atomic<int> g_tape_flash{1}, g_tape_sound{0};

static std::mutex g_tape_state_mtx;
static pmd85host_tape_state g_tape_state;

struct TapeRequest {
    int cmd = 0;
    std::string path;
    bool pending = false, done = false;
    int rc = -1;
    std::string err;
};
static std::mutex g_tape_caller_mtx;      /* one command at a time */
static std::mutex g_tape_req_mtx;
static std::condition_variable g_tape_req_cv;
static TapeRequest g_tape_req;

void pmd85host_tape_options(int flash, int sound)
{
    g_tape_flash.store(flash ? 1 : 0);
    g_tape_sound.store(sound ? 1 : 0);
}

int pmd85host_tape_command(int cmd, const char *path, char *err, int errsz)
{
    std::lock_guard<std::mutex> one(g_tape_caller_mtx);
    std::unique_lock<std::mutex> lk(g_tape_req_mtx);
    g_tape_req = TapeRequest();
    g_tape_req.cmd = cmd;
    g_tape_req.path = path ? path : "";
    g_tape_req.pending = true;
    if (!g_tape_req_cv.wait_for(lk, std::chrono::seconds(1),
                                [] { return g_tape_req.done; })) {
        g_tape_req.pending = false;
        if (err && errsz > 0)
            snprintf(err, (size_t)errsz, "The PMD is not running (stopped, or "
                     "held by the debugger)");
        return -1;
    }
    if (err && errsz > 0)
        snprintf(err, (size_t)errsz, "%s", g_tape_req.err.c_str());
    return g_tape_req.rc;
}

void pmd85host_tape_state_get(pmd85host_tape_state *out)
{
    if (!out) return;
    std::lock_guard<std::mutex> lk(g_tape_state_mtx);
    *out = g_tape_state;
}

/* The deck onto a (new) machine's tape interface. Whatever was playing
 * stops: the interface it played into is gone. */
static void tape_attach(PmdMachine *m)
{
    if (!g_deck)
        g_deck = new TTapeBrowser();
    g_deck->playing = false;
    if (m && m->ifTape)
        g_deck->SetIfTape(m->ifTape);
}

static int tape_run(int cmd, const std::string &path, std::string &err)
{
    TTapeBrowser *d = g_deck;
    int r;

    switch (cmd) {
    case PMD85_TAPE_LOAD:
        r = d->SetTapeFileName(path.c_str());
        if (r < 0 || d->totalBlocks == 0) {
            if (r >= 0) d->SetNewTape();
            err = (r < 0 && !FileExists(path.c_str())) ? "Cannot open " + path
                                                         : path + " is not a PMD 85 tape";
            g_tape_path.clear();
            return -1;
        }
        g_tape_path = path;
        if (r > 0) {
            err = "The tape's end is damaged; the blocks before it are in";
            return 1;
        }
        return 0;
    case PMD85_TAPE_EJECT:
        d->SetNewTape();
        g_tape_path.clear();
        return 0;
    case PMD85_TAPE_PLAY:
        if (d->totalBlocks == 0) {
            err = "No tape in the deck";
            return -1;
        }
        d->ActionPlay();
        return 0;
    case PMD85_TAPE_STOP:
        d->ActionStop();
        return 0;
    case PMD85_TAPE_REWIND:
        d->ActionStop();
        if (d->totalBlocks > 0)
            d->SetCurrentBlock(0);
        return 0;
    case PMD85_TAPE_SAVE: {
        if (d->totalBlocks == 0) {
            err = "Nothing on the tape to save";
            return -1;
        }
        /* TEmulator::SaveTape: written whole, then it is the tape */
        const int curr = d->currBlockIdx;
        r = d->SaveTape(path.c_str());
        if (r != 0) {
            err = r < 0 ? "Cannot write " + path : "Error writing " + path;
            return -1;
        }
        if (d->SetTapeFileName(path.c_str()) == 0)
            d->SetCurrentBlock(curr);
        g_tape_path = path;
        return 0;
    }
    }
    err = "Unknown tape command";
    return -1;
}

/* The pending command, if any (emulator thread, between frames). */
static void tape_service(void)
{
    TapeRequest req;
    {
        std::lock_guard<std::mutex> lk(g_tape_req_mtx);
        if (!g_tape_req.pending)
            return;
        req = g_tape_req;
        g_tape_req.pending = false;
    }
    std::string err;
    const int rc = g_deck ? tape_run(req.cmd, req.path, err) : -1;
    if (!g_deck) err = "No tape deck";
    {
        std::lock_guard<std::mutex> lk(g_tape_req_mtx);
        g_tape_req.rc = rc;
        g_tape_req.err = err;
        g_tape_req.done = true;
    }
    g_tape_req_cv.notify_all();
}

static void tape_publish(void)
{
    pmd85host_tape_state st;
    memset(&st, 0, sizeof st);
    st.current = -1;
    if (g_deck) {
        TTapeBrowser *d = g_deck;
        st.blocks = d->totalBlocks;
        st.current = d->totalBlocks > 0 ? d->currBlockIdx : -1;
        st.playing = d->playing ? 1 : 0;
        st.recorded = d->tapeChanged ? 1 : 0;
        if (d->ProgressBar) {
            st.progress = d->ProgressBar->Position;
            st.progress_max = d->ProgressBar->Max;
        }
        TTapeBrowser::TAPE_BLOCK *b = d->GetBlock(st.current);
        if (b && b->cType) {
            char name[9];
            int n = 8;
            memcpy(name, b->cName, 8);
            while (n > 0 && (name[n - 1] == ' ' || name[n - 1] == '\0')) n--;
            name[n] = '\0';
            for (int i = 0; i < n; i++)
                if ((unsigned char)name[i] < 0x20 || (unsigned char)name[i] > 0x7E) name[i] = '?';
            snprintf(st.block, sizeof st.block, "%03u/%c %s", (unsigned)b->bNumber,
                     (b->cType >= 0x20 && b->cType < 0x7F) ? b->cType : '?', name);
        } else if (b) {
            snprintf(st.block, sizeof st.block, "..");
        }
        snprintf(st.path, sizeof st.path, "%s", g_tape_path.c_str());
    }
    std::lock_guard<std::mutex> lk(g_tape_state_mtx);
    g_tape_state = st;
}

/* what other threads may ask about, refreshed by the emulator thread after
 * every frame (the machine itself is the emulator thread's alone, and may be
 * replaced by a reconfiguration at any frame boundary) */
static std::atomic<int> g_link_up{0};
static std::atomic<int> g_rx_pending{0};
static std::atomic<int> g_rom_loads{0};
static std::atomic<int> g_rom_last{0};
static int g_rom_loads_base;  /* loads counted by machines already gone */

static void publish_state(PmdMachine *m)
{
    if (!m) {
        g_led.store(0);
        g_mega_page.store(-1);
        g_link_up.store(0);
        g_rx_pending.store(0);
        return;
    }
    g_led.store(m->systemPIO ? m->systemPIO->ledState : 0);
    g_mega_page.store(m->megaModule ? m->megaModule->GetCurrentPage() : -1);
    if (m->becker) {
        int last = 0;
        int loads = m->becker->RomLoads(&last);
        g_link_up.store(m->becker->LinkUp() ? 1 : 0);
        g_rx_pending.store(m->becker->RxPending());
        if (loads) {
            /* cumulative across machine rebuilds */
            g_rom_loads.store(g_rom_loads_base + loads);
            g_rom_last.store(last);
        }
    } else {
        g_link_up.store(0);
        g_rx_pending.store(0);
    }
}

PmdMachine *pmd85host_machine(void) { return g_machine; }

static void key_sink(int down, int usage)
{
    if (g_machine && usage > 0 && usage < SDL_NUM_SCANCODES)
        g_machine->keyBuffer[usage] = down ? 1 : 0;
}

int pmd85host_core_start(const pmd85host_config *cfg)
{
    pmd85host_core_stop();
    g_last_error[0] = '\0';
    {
        std::lock_guard<std::mutex> lk(g_cmd_mtx);
        g_cmd_reset = g_cmd_reconfig = false;
    }
    {
        std::lock_guard<std::mutex> lk(g_key_mtx);
        g_key_queue.clear();
        memset(g_key_held_ui, 0, sizeof g_key_held_ui);
    }
    pmd85host_clear_audio();
    g_machine = new PmdMachine(*cfg);
    if (!g_machine->ok()) {
        delete g_machine;
        g_machine = nullptr;
        if (!g_last_error[0])
            snprintf(g_last_error, sizeof g_last_error, "the machine could not be built");
        return 0;
    }
    g_rom_loads_base = 0;
    g_rom_loads.store(0);
    g_rom_last.store(0);
    tape_attach(g_machine);
    publish_state(g_machine);
    tape_publish();
    return 1;
}

void pmd85host_core_stop(void)
{
    delete g_machine;
    g_machine = nullptr;
    publish_state(nullptr);
}

void pmd85host_core_run_frame(void)
{
    bool reset = false, reconfig = false;
    pmd85host_config cfg;
    {
        std::lock_guard<std::mutex> lk(g_cmd_mtx);
        reset = g_cmd_reset;
        reconfig = g_cmd_reconfig;
        if (reconfig) cfg = g_cmd_config;
        g_cmd_reset = g_cmd_reconfig = false;
    }
    if (reconfig) {
        PmdMachine *m = new PmdMachine(cfg);
        if (m->ok()) {
            /* keep the keys the user is holding across the rebuild */
            memcpy(m->keyBuffer, g_machine ? g_machine->keyBuffer : m->keyBuffer,
                   sizeof m->keyBuffer);
            g_rom_loads_base = g_rom_loads.load();
            tape_attach(m);
            delete g_machine;
            g_machine = m;
        } else {
            delete m;
            host_log(2, "reconfiguration failed; keeping the running machine");
        }
    }
    if (!g_machine) return;
    if (reset) g_machine->Reset();
    {
        int color = g_cmd_color.exchange(-1);
        if (color >= 0) g_machine->SetColorProfile(color);
    }

    g_tape_settings.flash = g_tape_flash.load() != 0;
    g_tape_settings.monitoring = g_tape_sound.load() != 0;
    tape_service();

    pmd85host_drain_keys(key_sink);
    g_machine->RunFrame();
    publish_state(g_machine);
    tape_publish();
}

void pmd85host_set_becker_trace(Pmd85BeckerTrace fn) { PmdBecker::SetTrace(fn); }

int pmd85host_becker_link_up(void) { return g_link_up.load(); }

int pmd85host_becker_rx_pending(void) { return g_rx_pending.load(); }

int pmd85host_becker_rom_loads(int *last_size)
{
    if (last_size) *last_size = g_rom_last.load();
    return g_rom_loads.load();
}

/* ======================================================================== */
/* The debugger's view (pmd85_host_internal.h). Emulator thread, or parked. */

int pmd85host_dbg_cpu(pmd85host_cpu *out)
{
    PmdMachine *m = g_machine;
    BYTE st[16];
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!m || !m->cpu) return 0;
    m->cpu->GetChipState(st);
    out->iff = (st[0] & 1) != 0;
    out->halt = (st[0] & 8) != 0;
    out->inta = (st[0] & 16) != 0;
    out->af = m->cpu->GetAF();
    out->bc = m->cpu->GetBC();
    out->de = m->cpu->GetDE();
    out->hl = m->cpu->GetHL();
    out->sp = m->cpu->GetSP();
    out->pc = m->cpu->GetPC();
    return 1;
}

int pmd85host_dbg_cpu_set(const pmd85host_cpu *in)
{
    PmdMachine *m = g_machine;
    if (!m || !m->cpu || !in) return 0;
    m->cpu->SetAF(in->af);
    m->cpu->SetBC(in->bc);
    m->cpu->SetDE(in->de);
    m->cpu->SetHL(in->hl);
    m->cpu->SetSP(in->sp);
    m->cpu->SetPC(in->pc);
    return 1;
}

int pmd85host_dbg_peek(uint16_t addr)
{
    PmdMachine *m = g_machine;
    if (!m || !m->memory) return -1;
    return m->memory->ReadByte(addr);   /* a plain pointer read, no side effect */
}

/* Whether a read at addr is served from the ROM image. ChipMemory keeps
 * the answer behind protected members (GetMemState says "read/write" where
 * reads come from ROM and writes go to the RAM below, as on the 2A's
 * monitor), so this asks through a derived view of the same object -- the
 * memory classes stay unmodified. */
class PmdMemProbe : public ChipMemory {
public:
    static bool ReadsRom(ChipMemory *mem, int addr)
    {
        PmdMemProbe *m = static_cast<PmdMemProbe *>(mem);
        BYTE *p = nullptr;
        if (m->FindPointer(addr, 1, OP_READ, &p) < 1 || !p || !m->memROM)
            return false;
        return p >= m->memROM && p < m->memROM + m->sizeROM;
    }
};

int pmd85host_dbg_mem_state(uint16_t addr)
{
    PmdMachine *m = g_machine;
    BYTE st = 0;
    if (!m || !m->memory) return 0;
    m->memory->GetMemState(addr, &st, nullptr);
    return (st & MA_RO ? PMD85HOST_MEM_R : 0) | (st & MA_WO ? PMD85HOST_MEM_W : 0) |
           (st & MA_VRAM ? PMD85HOST_MEM_VRAM : 0) |
           (st & MA_VRAM_B ? PMD85HOST_MEM_VRAM_ASIDE : 0) |
           ((st & MA_RO) && PmdMemProbe::ReadsRom(m->memory, addr) ? PMD85HOST_MEM_ROM : 0);
}

int pmd85host_dbg_poke(uint16_t addr, uint8_t value)
{
    PmdMachine *m = g_machine;
    int st = pmd85host_dbg_mem_state(addr);
    /* where reads come from ROM a write would land unseen in the RAM below */
    if (!m || !m->memory || !(st & PMD85HOST_MEM_W) || (st & PMD85HOST_MEM_ROM)) return 0;
    m->memory->WriteByte(addr, value);
    return 1;
}

int pmd85host_dbg_clock(uint64_t *tstates, uint64_t *frame, int *in_frame)
{
    PmdMachine *m = g_machine;
    int t;
    if (!m || !m->cpu) return 0;
    t = m->cpu->GetTCycles();
    if (tstates) *tstates = m->frames * (uint64_t)TCYCLES_PER_FRAME + (uint64_t)(t > 0 ? t : 0);
    if (frame) *frame = m->frames;
    if (in_frame) *in_frame = t;
    return 1;
}

void *pmd85host_machine_ptr(void)
{
    return g_machine;
}

int pmd85host_dbg_model(void)
{
    PmdMachine *m = g_machine;
    return m ? (int)m->model : -1;
}

const char *pmd85host_port_name(uint8_t port)
{
    static const char *const sys[4] = { "SYS PIO PA", "SYS PIO PB", "SYS PIO PC", "SYS PIO CWR" };
    static const char *const rom[4] = { "ROM PIO PA", "ROM PIO PB", "ROM PIO PC", "ROM PIO CWR" };
    static const char *const gpio[4] = { "GPIO PA", "GPIO PB", "GPIO PC", "GPIO CWR" };
    static const char *const pit[4] = { "PIT CT0", "PIT CT1", "PIT CT2", "PIT CWR" };
    static const char *const usart[2] = { "USART data", "USART cmd/status" };
    PmdMachine *m = g_machine;
    TComputerModel model;

    if (!m) return nullptr;
    model = m->model;
    /* in the order Build() adds them, as the CPU's FindDevice walks them */
    if ((port & SYSTEM_PIO_MASK) == SYSTEM_PIO_ADR) return sys[port & 3];
    if (m->ifGpio && (port & IIF_GPIO_MASK) == IIF_GPIO_ADR) return gpio[port & 3];
    if (m->ifTimer && (port & IIF_TIMER_MASK) == IIF_TIMER_ADR) return pit[port & 3];
    if (model != CM_MATO) {
        const bool alfa = model == CM_ALFA || model == CM_ALFA2;
        if (alfa ? (port & IIF_TAPE_MASK_A) == IIF_TAPE_ADR_A
                 : (port & IIF_TAPE_MASK) == IIF_TAPE_ADR)
            return usart[(port & (alfa ? IIF_TAPE_REG_MASK_A : IIF_TAPE_REG_MASK)) & 1];
    }
    if ((model == CM_V2A || model == CM_V3) && m->cfg.ram256k && port == MM256_REG_ADR)
        return "MM256 page";
    if (m->mif85 && (port & MIF85_MASK) == MIF85_ADR) return "MIF 85";
    if (m->becker && (port & PMD85_BECKER_MASK) == PMD85_BECKER_ADR)
        return (port & 1) ? "BECKER data" : "BECKER status";
    if (m->romModule && (port & ROM_MODULE_MASK) == ROM_MODULE_ADR) return rom[port & 3];
    if (m->megaModule && port == MEGA_MODULE_ADR) return "MEGA page";
    return nullptr;
}

static void pio_state(ChipPIO8255 *pio, pmd85debug_pio *out)
{
    BYTE st[16];
    memset(out, 0, sizeof *out);
    if (!pio) return;
    pio->GetChipState(st, true);
    out->valid = 1;
    out->cwr = st[0];
    out->c = st[1];
    out->b = st[2];
    out->a = st[3];
    out->inte = st[4];
    out->in_c = st[5];
    out->in_b = st[6];
    out->in_a = st[8];
}

int pmd85host_dbg_io(pmd85debug_io *out)
{
    PmdMachine *m = g_machine;
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    out->mm256 = -1;
    out->mega_page = -1;
    if (!m) return 0;
    out->model = (int)m->model;
    pio_state(m->systemPIO, &out->sys);
    pio_state(m->ifGpio, &out->gpio);
    pio_state(m->romModule, &out->rom);
    if (m->ifTimer)
        for (int i = 0; i < 3; i++) {
            const auto &c = m->ifTimer->Counters[i];
            out->pit[i].valid = 1;
            out->pit[i].cwr = c.CWR;
            out->pit[i].init = c.InitValue;
            out->pit[i].value = c.CounterValue;
            out->pit[i].gate = c.Gate;
            out->pit[i].out = c.Out;
            out->pit[i].counting = c.Counting;
        }
    if (m->ifTape && m->model != CM_MATO) {
        BYTE st[16];
        ((IifTapePMD85 *)m->ifTape)->GetChipState(st, true);
        out->usart_valid = 1;
        out->usart_mode = st[0];
        out->usart_command = st[3];
        out->usart_status = st[4];
    }
    if ((m->model == CM_V2A || m->model == CM_V3) && m->cfg.ram256k)
        out->mm256 = m->memory->GetPage();
    out->mega_page = m->megaModule ? m->megaModule->GetCurrentPage() : -1;
    out->allram = m->memory->IsAllRAM();
    out->remapped = m->memory->IsRemapped();
    out->in_reset = m->memory->IsInReset();
    out->led = m->systemPIO ? m->systemPIO->ledState : 0;
    out->becker = m->becker != nullptr;
    if (m->becker) {
        out->becker_link = m->becker->LinkUp() ? 1 : 0;
        out->becker_rx = m->becker->RxPending();
    }
    out->mif85 = m->mif85 != nullptr;
    out->pmd32 = m->pmd32 != nullptr;
    return 1;
}
