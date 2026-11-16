/*
 * Desktop host for the GPMD85Emulator Tesla PMD 85 machine core.
 *
 * The session statically links the staged GPMD85Emulator *machine* sources
 * (the 8080, the memory maps, the 8255/8253/8251 chips, the tape, GPIO and
 * timer interfaces, PMD 32, MIF 85, the ROM and MegaModule) and nothing of its
 * SDL/Dear ImGui application: TEmulator, ScreenPMD85, SoundDriver, Settings
 * and the gui/ tree are never compiled. This host is what stands in for them
 * (pmd85_host.cpp): it builds a machine from a plain config struct, runs it a
 * frame at a time, renders the frame from video RAM, mixes the sound and owns
 * the Becker port device that talks DriveWire to FujiNet.
 *
 * Everything that touches the machine runs on the emulator thread. Keys,
 * reset and reconfiguration requests from a UI thread are queued and applied
 * at the top of pmd85host_core_run_frame(); the machine sources are not
 * thread-safe and are never touched from anywhere else.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PMD85_HOST_H
#define PMD85_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Machine models, in GPMD85Emulator's TComputerModel order (the values are
 * passed straight through). */
enum {
    PMD85_MODEL_1 = 0,     /* PMD 85-1 */
    PMD85_MODEL_2,         /* PMD 85-2 */
    PMD85_MODEL_2A,        /* PMD 85-2A */
    PMD85_MODEL_3,         /* PMD 85-3 */
    PMD85_MODEL_MATO,      /* Mato */
    PMD85_MODEL_ALFA,      /* Didaktik Alfa */
    PMD85_MODEL_ALFA2,     /* Didaktik Alfa 2 */
    PMD85_MODEL_C2717,     /* Consul 2717 */
    PMD85_MODEL_COUNT
};

/* Colour profiles (TColorProfile). */
enum {
    PMD85_COLOR_MONO = 0,
    PMD85_COLOR_STANDARD,  /* white / half-bright, blinking -- the real TV */
    PMD85_COLOR_COLOR,     /* attribute pairs mapped to four colours */
    PMD85_COLOR_COLORACE,  /* ColorACE: two lines' attributes -> 8 colours */
    PMD85_COLOR_COUNT
};

/* ROM module kinds. */
enum {
    PMD85_ROMMOD_NONE = 0,
    PMD85_ROMMOD_PACK,     /* the classic 32 KB module, assembled from .rmm files */
    PMD85_ROMMOD_MEGA,     /* MegaModule: up to 256 pages of 32 KB, from one image */
};

/* Joystick 4004/482 bits for pmd85host_set_joystick (active high here; the
 * host inverts them onto the GPIO port). */
enum {
    PMD85_JOY_DOWN = 1,
    PMD85_JOY_UP = 2,
    PMD85_JOY_RIGHT = 4,
    PMD85_JOY_LEFT = 8,
    PMD85_JOY_FIRE = 16,
};

#define PMD85HOST_PATH_MAX   1024
#define PMD85HOST_RMM_MAX    8
#define PMD85HOST_FB_WIDTH_MAX  384   /* Consul 2717 extended mode */
#define PMD85HOST_FB_HEIGHT     256
#define PMD85HOST_FRAME_RATE_MHZ 50000
#define PMD85HOST_AUDIO_RATE    44100

typedef struct pmd85host_config {
    int model;                         /* PMD85_MODEL_* */
    char monitor[PMD85HOST_PATH_MAX];  /* the monitor ROM image */
    int ram256k;          /* 2A/3: the 256 KB RAM expansion */
    int compat_mode;      /* 3: switch to the 85-2A compatibility mode */
    int split8k;          /* 1/2/2A: an 8 KB monitor split at 8000h/A000h */
    int mato_allram64k;   /* Mato: 64 KB all-RAM mode */

    int rom_module;                                  /* PMD85_ROMMOD_* */
    int rmm_count;
    char rmm[PMD85HOST_RMM_MAX][PMD85HOST_PATH_MAX]; /* PACK: files, in order */
    char mrm[PMD85HOST_PATH_MAX];                    /* MEGA: the image */

    int mif85;            /* MIF 85 music interface (SAA1099) */

    int pmd32;            /* PMD 32 disk drive on GPIO */
    int pmd32_extra;      /* its SD-card extended commands */
    char pmd32_sdroot[PMD85HOST_PATH_MAX];
    char pmd32_drive[4][PMD85HOST_PATH_MAX];
    int pmd32_wp[4];

    int becker;           /* the Becker port (DriveWire-over-TCP, I/O 3Eh/3Fh) */
    char becker_host[64];
    int becker_port;

    int joystick_gpio0;   /* a 4004/482 joystick on GPIO port A / B */
    int joystick_gpio1;

    int color_profile;    /* PMD85_COLOR_* */
    int attr[4];          /* CP_COLOR: TColor index for attribute 00/01/10/11 */

    int volume;           /* 0..127 */
    int exchange_zy;      /* swap Z and Y (Czech/Slovak keyboards) */
    int numpad;           /* map the numeric keypad */
    int mato_ctrl;        /* Mato: extended control-key mapping */
} pmd85host_config;

/* Fill c with the defaults: a PMD 85-2A, standard colours, no peripherals. */
void pmd85host_config_defaults(pmd85host_config *c);

/* --- log sink ---------------------------------------------------------------
 * level 0 = info, 1 = warning, 2 = error. Called on the emulator thread. */
typedef void (*Pmd85LogSink)(int level, const char *msg, void *user);
void pmd85host_set_log_sink(Pmd85LogSink sink, void *user);

/* --- frame sink -------------------------------------------------------------
 * XRGB8888 (uint32 0x00RRGGBB), tightly packed, width*height; 288x256, or
 * 384x256 in the Consul 2717's extended mode. Emulator thread, once a frame. */
typedef void (*Pmd85FrameSink)(const uint32_t *xrgb8888, int width, int height,
                               void *user);
void pmd85host_set_frame_sink(Pmd85FrameSink sink, void *user);

/* --- core lifecycle (emulator thread) --------------------------------------
 * start builds the machine from cfg and cold-boots it; 0 on failure (the
 * reason goes to the log sink and pmd85host_last_error()). */
int  pmd85host_core_start(const pmd85host_config *cfg);
void pmd85host_core_run_frame(void);  /* one 20 ms frame: 40960 T-states */
void pmd85host_core_stop(void);
const char *pmd85host_last_error(void);

/* Any thread; applied at the next frame. reset is the RESET key (a warm
 * restart, memory kept); reconfigure rebuilds the machine from cfg and
 * cold-boots it (a model or peripheral change, or a "hard reset"). */
void pmd85host_core_reset(void);
void pmd85host_core_reconfigure(const pmd85host_config *cfg);
/* Any thread; the colour profile (PMD85_COLOR_*) changes how frames are
 * rendered and nothing else, so it applies without touching the machine. */
void pmd85host_set_color_profile(int profile);

/* --- audio ------------------------------------------------------------------
 * Interleaved stereo S16 at 44100 Hz, 882 frames per emulated frame. fill
 * blocks (bounded, 50 ms) until max_samples are available, silence-padding
 * on underrun, and returns max_samples. */
int  pmd85host_fill_audio(int16_t *out, int max_samples);
void pmd85host_audio_set_active(int active);  /* 0 on shutdown unblocks fill */
void pmd85host_clear_audio(void);

/* --- input ------------------------------------------------------------------
 * Keyboard by USB HID usage code (0..255; SDL scancodes are the same values).
 * Queued and applied per frame; a key pressed and released between two frames
 * is still held for one frame, so the machine always sees it. */
void pmd85host_inject_key(int down, int hid_usage);
void pmd85host_release_all_keys(void);
/* The per-frame drain of the queue above, exported for tests: hands each event
 * to sink(down, usage) in order, holding back a release whose press came in
 * the same drain. Returns how many it delivered. */
int  pmd85host_drain_keys(void (*sink)(int down, int usage));
/* port 0 = GPIO port A, 1 = port B; bits are PMD85_JOY_*. Any thread. */
void pmd85host_set_joystick(int port, unsigned bits);

/* --- Becker port ------------------------------------------------------------
 * Every byte across the link, for the debugger's FujiNet tab: dir 1 is
 * PMD -> FujiNet (sent), dir 0 FujiNet -> PMD (read by the CPU); len 0 means
 * the link (re)connected. Emulator thread; NULL turns it off. */
typedef void (*Pmd85BeckerTrace)(int dir, const uint8_t *buf, size_t len);
void pmd85host_set_becker_trace(Pmd85BeckerTrace fn);
int  pmd85host_becker_link_up(void);     /* any thread */
int  pmd85host_becker_rx_pending(void);  /* any thread */
/* ROM-module downloads over the link: how many completed since start, and
 * the size of the last one. Any thread. */
int  pmd85host_becker_rom_loads(int *last_size);

/* --- state for the frontends (any thread, relaxed) ------------------------- */
int  pmd85host_led_state(void);    /* SystemPIO LED bits: 1 yellow 2 red 4 blue */
int  pmd85host_mega_page(void);    /* current MegaModule page, -1 when absent */

#ifdef __cplusplus
}
#endif

#endif /* PMD85_HOST_H */
