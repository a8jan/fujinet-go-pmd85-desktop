/*
 * pmd85session -- the toolkit-agnostic session core of FujiNet Go PMD 85.
 *
 * THE frontend contract: the GNOME (GTK4), KDE (Qt6), macOS (AppKit) and
 * Windows (Win32) frontends call this header (and pmd85debug.h) and nothing
 * else. Everything that is not windowing, painting or event translation lives
 * behind it: the paced emulator thread, the shared settings store, the paths,
 * audio, the in-process FujiNet runtime and the debugger engine.
 *
 * The machine is GPMD85Emulator's (see core/pmd85/pmd85_host.h); FujiNet talks
 * to it through a Becker port (DriveWire over loopback TCP). FujiNet listens
 * and the PMD connects out, so a session starts FujiNet first.
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PMD85SESSION_H
#define PMD85SESSION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* FujiNet's DriveWire listener for this app. 65504 is the CoCo app's; a
 * different port lets the two run side by side. PMD85_BECKER_PORT in the
 * environment overrides it (and the session then rewrites the runtime
 * tree's [BOIP] port to match). */
#define PMD85SESSION_BECKER_PORT 65505
/* FujiNet's web admin, bound to 127.0.0.1. */
#define PMD85SESSION_WEBUI_PORT  64003

#define PMD85SESSION_FB_MAX_WIDTH  384  /* Consul 2717 extended mode */
#define PMD85SESSION_FB_MAX_HEIGHT 256

typedef struct pmd85session pmd85session;
typedef struct pmd85debug pmd85debug;

/* ---- machine options ----------------------------------------------------- */

/* Models, in GPMD85Emulator's order. */
enum {
    PMD85SESSION_MODEL_1 = 0,
    PMD85SESSION_MODEL_2,
    PMD85SESSION_MODEL_2A,
    PMD85SESSION_MODEL_3,
    PMD85SESSION_MODEL_MATO,
    PMD85SESSION_MODEL_ALFA,
    PMD85SESSION_MODEL_ALFA2,
    PMD85SESSION_MODEL_C2717,
};

/* Colour profiles. */
enum {
    PMD85SESSION_COLOR_MONO = 0,
    PMD85SESSION_COLOR_STANDARD,   /* the TV picture: white, half-bright, blink */
    PMD85SESSION_COLOR_COLOR,      /* attributes as four colours */
    PMD85SESSION_COLOR_COLORACE,   /* the ColorACE modification: eight colours */
};

/* What sits in the ROM-module slot -- which, on a PMD 85-2 or 2A, decides
 * what the machine boots into (their monitors auto-run the module). */
enum {
    /* The MegaModule with the FujiNet CONFIG loader in page 0: the machine
     * boots into FujiNet CONFIG, and a ROM module mounted there is downloaded
     * over the Becker port into the MegaModule. The default. */
    PMD85SESSION_MODULE_FUJINET = 0,
    /* The model's stock BASIC-G module (GPMD85Emulator's package for it). */
    PMD85SESSION_MODULE_BASIC,
    /* Nothing in the slot: the bare monitor. */
    PMD85SESSION_MODULE_NONE,
    /* A module image the user picked (setting "module_file"): a .mrm, or
     * anything over 32 KB, goes into a MegaModule; else the classic ROM pack.
     * (Values are stored in the settings, so new ones go last.) */
    PMD85SESSION_MODULE_FILE,
};

/* Integer scaling vs. stretched, for the display widget (shared setting
 * "aspect"; the frontends all honour the same values). */
enum {
    PMD85SESSION_ASPECT_TV = 0,    /* the PMD's 288x256 on a 4:3 TV picture */
    PMD85SESSION_ASPECT_SQUARE,    /* square pixels */
    PMD85SESSION_ASPECT_INTEGER,   /* square pixels, whole multiples only */
};

/* ---- paths and options --------------------------------------------------- */

/* Every member is optional (NULL = the default). */
typedef struct {
    const char *config_dir;  /* settings.ini; default XDG/APPDATA */
    const char *data_dir;    /* ROMs, the FujiNet runtime tree; default XDG */
    const char *fujinet_lib; /* the FujiNet library; "" = disable FujiNet */
    const char *fujinet_runtime_src; /* a pristine runtime tree to provision from */
} pmd85session_paths;

typedef struct {
    int model;          /* PMD85SESSION_MODEL_* */
    int color_profile;  /* PMD85SESSION_COLOR_* */
    int module;         /* PMD85SESSION_MODULE_* */
    int ram256k;        /* 2A/3: the 256 KB RAM expansion */
    int mif85;          /* MIF 85 music interface */
    int pmd32;          /* PMD 32 disk drive */
    int joystick;       /* 4004/482 joysticks on the GPIO ports, driven by
                           pmd85session_set_joystick (gamepads, frontends) */
    int enable_fujinet;
    int external_fujinet;   /* talk to a FujiNet elsewhere instead */
    char fujinet_addr[80];  /* its host:port */
    int enable_audio;
    int enable_gamepad;
} pmd85session_start_opts;

/* ---- lifecycle ------------------------------------------------------------ */

pmd85session *pmd85session_new(const pmd85session_paths *paths);
void pmd85session_free(pmd85session *s);

/* opts from the shared settings (and the defaults for what is not set). */
void pmd85session_default_opts(pmd85session *s, pmd85session_start_opts *opts);
/* 0 on success; -1 with pmd85session_last_error() set. Starts FujiNet first,
 * waits for its DriveWire listener, then the machine. */
int  pmd85session_start(pmd85session *s, const pmd85session_start_opts *opts);
void pmd85session_stop(pmd85session *s);
int  pmd85session_is_running(const pmd85session *s);
const char *pmd85session_last_error(const pmd85session *s);

/* ---- settings (one INI shared by every frontend) -------------------------- */

int  pmd85session_get_int(pmd85session *s, const char *key, int def);
void pmd85session_set_int(pmd85session *s, const char *key, int val);
const char *pmd85session_get_str(pmd85session *s, const char *key, const char *def);
void pmd85session_set_str(pmd85session *s, const char *key, const char *val);
void pmd85session_settings_flush(pmd85session *s);

/* ---- the machine ---------------------------------------------------------- */

/* Menu labels, NULL past the end. */
const char *pmd85session_model_name(int idx);
const char *pmd85session_color_name(int idx);
const char *pmd85session_module_name(int idx);
/* 1 when the module option means anything on this model (the ROM-module slot
 * exists on the PMD 85 models and the Didaktik Alfa/Alfa 2, not on the Mato
 * or the Consul 2717; it auto-runs on the 2 and 2A). */
int  pmd85session_module_supported(int model, int module);
/* How to start the module when this model's monitor does not auto-run it
 * (a line for the user, e.g. the monitor command), or NULL when it boots by
 * itself or there is nothing to start. */
const char *pmd85session_module_hint(pmd85session *s, int model, int module);

/* Live changes. Model, module and peripherals rebuild the machine (a cold
 * start; FujiNet stays up and the Becker link reconnects); the colour profile
 * applies at once. All persist. */
void pmd85session_set_model(pmd85session *s, int model);
void pmd85session_set_color_profile(pmd85session *s, int profile);
void pmd85session_set_module(pmd85session *s, int module);
/* Pick a module image and switch to PMD85SESSION_MODULE_FILE. 0, or -1 with
 * pmd85session_last_error() set (missing, empty or over 8 MB) and nothing
 * changed. */
int  pmd85session_set_module_file(pmd85session *s, const char *path);
/* The picked module image's path, "" for none. */
const char *pmd85session_module_file(pmd85session *s);
/* 1 when the picked image is there to load. */
int  pmd85session_module_file_ok(pmd85session *s);
void pmd85session_set_peripheral(pmd85session *s, const char *key, int on);
int  pmd85session_model(const pmd85session *s);
int  pmd85session_color_profile(const pmd85session *s);
int  pmd85session_module(const pmd85session *s);

/* RESET (the machine's own key: memory kept) and a cold restart. */
void pmd85session_reset(pmd85session *s);
void pmd85session_hard_reset(pmd85session *s);

/* LEDs (bit 0 yellow, 1 red, 2 blue) and the MegaModule page, -1 = none. */
int  pmd85session_leds(const pmd85session *s);
int  pmd85session_mega_page(const pmd85session *s);

/* ---- video ---------------------------------------------------------------- */

/* Copies the latest frame (XRGB8888, *w x *h, at most 384 x 256) into dst when
 * its serial differs from *serial_inout, updating it; returns 1 if copied.
 * Pass serial 0 to force a copy (after a window map). */
int  pmd85session_copy_frame(pmd85session *s, uint32_t *dst,
                             uint64_t *serial_inout, int *w_out, int *h_out);
/* Called on the emulator thread when a new frame is published (the frontend
 * typically just queues a redraw). NULL to clear; set(NULL) waits for one in
 * flight. */
typedef void (*pmd85session_frame_cb)(void *ctx);
void pmd85session_set_frame_callback(pmd85session *s, pmd85session_frame_cb cb,
                                     void *ctx);
uint64_t pmd85session_frame_count(pmd85session *s);
/* The display's vsync tick. The PMD runs at 50 Hz, which no desktop display
 * does, so the pacer never phase-locks to these; they are accepted so every
 * frontend can share the same display code as the family. */
void pmd85session_notify_vsync(pmd85session *s, int64_t frame_time_ns);
int  pmd85session_frame_rate_mhz(const pmd85session *s);

/* ---- input ---------------------------------------------------------------- */

/* A host key (X11/xkb keysym + its unicode, as every frontend produces) onto
 * the PMD keyboard. Positional: the physical key on a US layout. */
void pmd85session_key(pmd85session *s, int down, uint32_t keysym,
                      uint32_t unicode, int ctrl_down, int shift_down);
/* The pure mapping behind it: keysym -> USB HID usage (what GPMD85Emulator's
 * keyboard tables are keyed by). 0 = not a PMD key, or reserved by the
 * frontends (F12, the debugger). Unit-tested on its own. */
int  pmd85_key_from_event(uint32_t keysym, uint32_t unicode, int ctrl_down,
                          int shift_down, unsigned *usage_out);
/* Let go of every key (the window lost focus). */
void pmd85session_release_all(pmd85session *s);

/* Joystick 4004/482 on GPIO port 0 (A) or 1 (B): PMD85SESSION_JOY_* bits. */
enum {
    PMD85SESSION_JOY_DOWN = 1,
    PMD85SESSION_JOY_UP = 2,
    PMD85SESSION_JOY_RIGHT = 4,
    PMD85SESSION_JOY_LEFT = 8,
    PMD85SESSION_JOY_FIRE = 16,
};
void pmd85session_set_joystick(pmd85session *s, int port, unsigned bits);

/* ---- audio ---------------------------------------------------------------- */

/* Interleaved stereo S16 at 44100 Hz: the machine plus FujiNet's own audio.
 * Safe on the audio thread. Returns nsamples. */
int  pmd85session_render_audio(pmd85session *s, int16_t *out, int nsamples);
void pmd85session_set_mute(pmd85session *s, int mute);

/* ---- FujiNet -------------------------------------------------------------- */

/* "host:port" or "host" or "port" -> host + port (default 127.0.0.1:65505).
 * 0 on success. */
int  pmd85session_parse_hostport(const char *text, char *host, int hostsz,
                                 int *port);
int  pmd85session_fujinet_running(const pmd85session *s);
int  pmd85session_fujinet_external(const pmd85session *s);
/* Stop and start the built-in FujiNet alone (the PMD keeps running; the
 * Becker port reconnects by itself) and wait for its listener. Blocks for
 * a second or so. 0, or -1 with pmd85session_last_error(). */
int  pmd85session_fujinet_restart(pmd85session *s);
/* 1 while the Becker port is connected to FujiNet. */
int  pmd85session_fujinet_link(const pmd85session *s);
const char *pmd85session_becker_endpoint(const pmd85session *s);
/* "" or a warning from the last start worth showing the user -- above all
 * another FujiNet already listening on the Becker port, which the machine
 * then talks to instead of ours (it looks like FujiNet works, with someone
 * else's configuration and SD card). */
const char *pmd85session_fujinet_warning(const pmd85session *s);
const char *pmd85session_fujinet_webui_url(const pmd85session *s);
int  pmd85session_fujinet_copy_log(pmd85session *s, char *dst, int max);
/* ROM modules downloaded from FujiNet into the MegaModule since start, and
 * the last one's size. */
int  pmd85session_rom_loads(const pmd85session *s, int *last_size);

/* ---- the tape deck ---------------------------------------------------------
 * A .ptp/.pmd tape as a list of blocks (a file's header and body are one
 * block), played into the PMD's tape interface; it stops by itself before
 * the next file. What the PMD saves to tape (MGSV, BASIC's SAVE...) is
 * recorded as new blocks; Save Tape As writes them all to a .ptp, which then
 * is the tape. The tape stays in across model changes and is put back at the
 * next start (setting "tape_file").
 *
 * Each returns 0, or -1 with pmd85session_last_error(); load returns 1 when
 * a damaged tape went in up to the damage (the message says so). They wait
 * for the emulator thread (a frame, 20 ms). */
int  pmd85session_tape_load(pmd85session *s, const char *path);
int  pmd85session_tape_eject(pmd85session *s);
int  pmd85session_tape_play(pmd85session *s);
int  pmd85session_tape_stop(pmd85session *s);
int  pmd85session_tape_rewind(pmd85session *s);
int  pmd85session_tape_save(pmd85session *s, const char *path);
/* That tape's blocks after these (a load when the deck is empty). */
int  pmd85session_tape_append(pmd85session *s, const char *path);

/* The block list (upstream's tape browser). Refused while the tape plays.
 * sel: the blocks selected (nsel of them); with none, block idx. Edits
 * leave the tape changed (info.recorded) until it is saved. */
int  pmd85session_tape_seek(pmd85session *s, int idx);     /* plays next */
int  pmd85session_tape_mark(pmd85session *s, int idx);     /* STOP marker, -1 off */
int  pmd85session_tape_delete(pmd85session *s, int idx, const int *sel, int nsel);
int  pmd85session_tape_move(pmd85session *s, int up, int idx, const int *sel, int nsel);

typedef struct {
    char name[16];        /* "016/? VLAK", ".." for a headerless block */
    int  number;          /* file number, -1 headerless */
    int  start;           /* load address, -1 headerless */
    int  length;          /* bytes in the body */
    int  bad;             /* CRC or length error */
} pmd85session_tape_block;
/* Up to max blocks into out; returns how many there are. *serial (may be
 * NULL) changes whenever the list does (see info.list_serial). */
int  pmd85session_tape_blocks(pmd85session *s, pmd85session_tape_block *out, int max,
                              unsigned *serial);

typedef struct {
    int  blocks;          /* 0: no tape */
    int  current;         /* the block that plays next, -1 none */
    int  playing;
    int  recorded;        /* blocks recorded and not saved yet */
    int  progress, progress_max;   /* bytes of the current block */
    char block[24];       /* the current block: "016/? VLAK", ".." raw data */
    char path[1024];      /* the tape's file, "" when only recorded */
    int  marker;          /* the STOP marker's block, -1 none */
    unsigned list_serial; /* changes with the block list */
} pmd85session_tape_info;
void pmd85session_tape_get_info(pmd85session *s, pmd85session_tape_info *out);

/* Flash load (setting "tape_flash", on by default): a file's body goes into
 * memory at once through the monitor's tape routines, its header still in
 * real time. Tape sound ("tape_sound", off): the signal on the speaker
 * while the tape plays. Both take effect at once and persist. */
void pmd85session_set_tape_options(pmd85session *s, int flash, int sound);

/* Where a playing tape stops by itself (setting "tape_autostop"). */
enum {
    PMD85SESSION_TAPE_STOP_END = 0,   /* at the tape's end */
    PMD85SESSION_TAPE_STOP_HEADER,    /* before the next file (default) */
    PMD85SESSION_TAPE_STOP_MARKER,    /* at the STOP marker */
};
void pmd85session_set_tape_autostop(pmd85session *s, int mode);
int  pmd85session_tape_autostop(pmd85session *s);

/* ---- media and paths ------------------------------------------------------ */

/* Route a dropped/opened file: ROM-module images (.rmm .mrm) and other FujiNet
 * media go to FujiNet's SD folder, to be mounted from CONFIG; monitor ROMs
 * (.rom) to the ROM folder. dest_out gets the copy's path. 0 on success. */
int  pmd85session_import_media(pmd85session *s, const char *src_path,
                               char *dest_out, int dest_sz);
const char *pmd85session_roms_path(const pmd85session *s);
const char *pmd85session_config_path(const pmd85session *s);
const char *pmd85session_data_path(const pmd85session *s);
const char *pmd85session_sd_path(const pmd85session *s);

/* ---- debugger ------------------------------------------------------------- */

/* The engine (pmd85debug.h), created on first use, alive as long as s. */
pmd85debug *pmd85session_debugger(pmd85session *s);

#ifdef __cplusplus
}
#endif

#endif /* PMD85SESSION_H */
