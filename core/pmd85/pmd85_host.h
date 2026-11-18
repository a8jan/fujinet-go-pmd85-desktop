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
    PMD85_COLOR_BW = 0,    /* white / half-bright, blinking: the black-and-white TV */
    PMD85_COLOR_GREEN,     /* the same on a green monitor */
    PMD85_COLOR_RGBM,      /* attribute pairs mapped to four colours */
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
    int mouse602;         /* Mouse 602 on port 8Ch (85-1..3, Consul 2717) */

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
/* The host pointer for the Mouse 602, as a position on the PMD's screen in
 * its pixels (x 0..287, or 383 on a Consul 2717; y 0..255) and the buttons
 * held (PMD85_MOUSE_*). The emulator thread takes the latest at each frame
 * and the mouse moves by the difference -- one quadrature step per two
 * pixels (GPMD85Emulator's: one per pixel, which is fast). inside 0: the pointer left the screen; the next
 * position it comes back at is a fresh start, not a jump. Any thread. */
enum { PMD85_MOUSE_LEFT = 1, PMD85_MOUSE_RIGHT = 2, PMD85_MOUSE_MIDDLE = 4 };
void pmd85host_mouse(int x, int y, unsigned buttons, int inside);

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

/* --- the tape deck ------------------------------------------------------------
 * GPMD85Emulator's TapeBrowser: a .ptp/.pmd tape as a list of blocks, played
 * into the tape interface, stopping before the next header block; what the
 * PMD saves is recorded as new blocks. It outlives machine rebuilds (a model
 * change keeps the tape in) and session restarts.
 *
 * A command is carried out by the emulator thread at its next frame, and
 * pmd85host_tape_command waits for it: 0 done, -1 with err set (also when
 * the emulator does not run a frame within a second -- stopped, or held by
 * the debugger). Any thread but the emulator's. */
enum {
    PMD85_TAPE_LOAD = 1,   /* path: insert this tape (stops a playing one) */
    PMD85_TAPE_EJECT,      /* recorded blocks not saved are dropped */
    PMD85_TAPE_PLAY,
    PMD85_TAPE_STOP,
    PMD85_TAPE_REWIND,     /* stop, back to the first block */
    PMD85_TAPE_SAVE,       /* path: every block as a .ptp; it becomes the tape */
    PMD85_TAPE_APPEND,     /* path: that tape's blocks after these */
    /* the block list (refused while the tape plays): */
    PMD85_TAPE_SEEK,       /* idx: the block that plays next */
    PMD85_TAPE_MARK,       /* idx: the STOP marker (after the position), -1 none */
    PMD85_TAPE_DELETE,     /* the blocks in sel, or block idx when none */
    PMD85_TAPE_MOVE_UP,    /* the blocks in sel (one run), or block idx */
    PMD85_TAPE_MOVE_DOWN,
};
int pmd85host_tape_command(int cmd, const char *path, char *err, int errsz);
/* The same, with the block list's arguments. */
int pmd85host_tape_edit(int cmd, int idx, const int *sel, int nsel,
                        char *err, int errsz);
/* flash: blocks go into memory at once through the monitors' tape routines
 * (on by default); sound: the tape signal on the speaker while it plays.
 * Taken at the next frame; any thread. */
void pmd85host_tape_options(int flash, int sound);
/* Where a playing tape stops by itself (upstream's auto-stop). */
enum {
    PMD85_TAPE_STOP_END = 0,   /* at the tape's end */
    PMD85_TAPE_STOP_HEADER,    /* before the next header block (default) */
    PMD85_TAPE_STOP_MARKER,    /* at the STOP marker */
};
void pmd85host_tape_autostop(int mode);

typedef struct {
    int  blocks;          /* 0: no tape */
    int  current;         /* the block that plays next, -1 none */
    int  playing;
    int  recorded;        /* changed (recorded, edited) and not saved yet */
    int  progress, progress_max;   /* bytes of the current block */
    char block[24];       /* the current block: "016/? VLAK" or ".." (data) */
    char path[1024];      /* the tape file, "" when only recorded */
    int  marker;          /* the STOP marker's block, -1 none */
    unsigned list_serial; /* changes with the block list */
} pmd85host_tape_state;
/* As of the last frame. Any thread. */
void pmd85host_tape_state_get(pmd85host_tape_state *out);

typedef struct {
    char name[16];        /* "016/? VLAK", ".." for a headerless block */
    int  number;          /* the file number, -1 headerless */
    int  start;           /* load address, -1 headerless */
    int  length;          /* bytes in the body */
    int  bad;             /* CRC or length error */
} pmd85host_tape_block;
/* The block list as of the last frame: up to max blocks into out, the
 * number there are returned; *serial (may be NULL) gets its list_serial. */
int pmd85host_tape_blocks(pmd85host_tape_block *out, int max, unsigned *serial);

/* --- state for the frontends (any thread, relaxed) ------------------------- */
int  pmd85host_led_state(void);    /* SystemPIO LED bits: 1 yellow 2 red 4 blue */
int  pmd85host_mega_page(void);    /* current MegaModule page, -1 when absent */
int  pmd85host_mega_size(void);    /* bytes of the image in it (file or FujiNet), 0 none */

#ifdef __cplusplus
}
#endif

#endif /* PMD85_HOST_H */
