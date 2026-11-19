/*
 * pmd85session internals shared between the session, audio, FujiNet, paths
 * and debugger translation units. Nothing here is API; frontends must only
 * include pmd85session.h / pmd85debug.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PMD85SESSION_INTERNAL_H
#define PMD85SESSION_INTERNAL_H

#include <pthread.h>
#include <stdint.h>

#include "pmd85session.h"
#include "joy_input.h"

#define PMD85_PATH_MAX 1024

#define PMD85_FB_MAX_PIXELS \
    (PMD85SESSION_FB_MAX_WIDTH * PMD85SESSION_FB_MAX_HEIGHT)

typedef struct setting_kv {
    char *key;
    char *val;
    struct setting_kv *next;
} setting_kv;

struct pmd85session {
    /* ---- resolved paths ---- */
    char config_dir[PMD85_PATH_MAX];
    char data_dir[PMD85_PATH_MAX];
    char roms_dir[PMD85_PATH_MAX];   /* monitors and ROM modules, by name */
    char settings_file[PMD85_PATH_MAX];
    char fujinet_lib[PMD85_PATH_MAX];  /* "" = unavailable/disabled */
    char fujinet_src[PMD85_PATH_MAX];  /* provisioning source override */
    char fujinet_root[PMD85_PATH_MAX];
    char fujinet_config[PMD85_PATH_MAX];
    char fujinet_sd[PMD85_PATH_MAX];
    char fujinet_data[PMD85_PATH_MAX];
    char webui_url[64];

    /* ---- settings store ---- */
    pthread_mutex_t settings_mtx;
    setting_kv *settings;
    int settings_dirty;

    /* ---- lifecycle ---- */
    pthread_mutex_t lifecycle_mtx;
    pthread_t emu_thread;
    int emu_thread_started;
    volatile int stop_flag;
    volatile int running;
    pmd85session_start_opts opts;

    /* what the UI last asked for (applied through the host's command queue) */
    volatile int cur_model;
    volatile int cur_color;
    volatile int cur_module;

    /* the emulator thread's start handshake: 0 pending, 1 up, -1 failed */
    pthread_mutex_t start_mtx;
    pthread_cond_t start_cv;
    int core_started;

    /* ---- latest-frame store (emulator thread -> UI thread) ---- */
    pthread_mutex_t frame_mtx;
    uint32_t *frame;  /* PMD85_FB_MAX_PIXELS XRGB8888 */
    int frame_w;
    int frame_h;
    uint64_t frame_serial;
    pthread_mutex_t cb_mtx;
    pmd85session_frame_cb frame_cb;
    void *frame_cb_ctx;
    volatile uint64_t frame_count;
    volatile uint64_t frames_dropped;   /* given up after stalls (pacer) */

    /* ---- vsync ticks (accepted, never locked to: see notify_vsync) ---- */
    pthread_mutex_t vs_mtx;
    int64_t vs_ns;

    /* ---- backends ---- */
    void *audio;    /* audio_sdl.c state */
    volatile int audio_mute;

    /* ---- input ---- */
    pthread_mutex_t input_mtx;
    uint8_t key_down[256];  /* HID usages the host keyboard holds */

    /* ---- joysticks (joy_input.h) and gamepads (gamepad_sdl.c) ---- */
    pthread_mutex_t joy_mtx;
    unsigned joy_held[PMD85_SRC_COUNT][PMD85_JOY_PORTS];
    void *gamepad;              /* gamepad_sdl.c state */
    unsigned pad_gen_base;      /* the pad generation, carried across a restart */
    int pad_restarted;
    pthread_mutex_t pad_event_mtx;
    char pad_event[160];        /* the last connect / disconnect, for a toast */
    unsigned pad_event_serial;

    /* ---- FujiNet runtime (fujinet_runtime.c) ---- */
    int fujinet_running;
    int fujinet_external;
    char becker_host[64];
    int becker_port;
    char becker_endpoint[80];
    char fujinet_warning[256];  /* "" or why the link may not be ours */

    /* ---- debugger ---- */
    pmd85debug *debugger;

    char last_error[512];
};

/* session.c */
void session_set_error(pmd85session *s, const char *fmt, ...);

/* settings.c */
void settings_init(pmd85session *s);
void settings_free_all(pmd85session *s);

/* paths.c */
int  paths_init(pmd85session *s, const pmd85session_paths *p);
int  paths_provision_fujinet(pmd85session *s); /* fills fujinet_* or "" */
void paths_materialise_roms(pmd85session *s);  /* embedded ROMs -> roms_dir */
int  mkdir_p(const char *path);
int  copy_file(const char *src, const char *dst);
int  path_is_file(const char *path);

/* gamepad_sdl.c */
int  gamepad_start(pmd85session *s);
void gamepad_stop(pmd85session *s);
/* session.c, for gamepad_sdl.c: a connect/disconnect message; the joysticks
 * the machine has now (bit 0 = joystick 1, bit 1 = joystick 2) */
void session_gamepad_event(pmd85session *s, const char *text);
unsigned session_joysticks(pmd85session *s);

/* audio_sdl.c */
int  audio_start(pmd85session *s);
void audio_stop(pmd85session *s);

/* fujinet_runtime.c */
int  fujinet_start(pmd85session *s);
int  fujinet_port_in_use(int port);
int  fujinet_wait_for_becker(pmd85session *s, int timeout_ms);
/* 1 when it stopped (or did not run); 0 when its thread did not end in time
 * (a device blocked) and was left running: it cannot start again until it
 * ends. */
int  fujinet_stop(pmd85session *s);
int  fujinet_copy_log(pmd85session *s, char *dst, int max);
int  fujinet_read_log(pmd85session *s, pmd85_log_cursor *cur, char *dst, int max, int *reset);
void fujinet_mix_audio(pmd85session *s, int16_t *buf, int nsamples, int rate);

/* debugger (core/debugger) -- emulator-thread side except create/destroy */
pmd85debug *pmd85debug_create(pmd85session *s);
void pmd85debug_destroy(pmd85debug *d);
/* Unpark a stopped debugger so a stopping emulator thread can exit. */
void pmd85debug_resume_for_stop(pmd85debug *d);
/* Point the Becker port's trace at d. UI thread, before the emulator starts. */
void pmd85debug_trace_install(pmd85debug *d);

#endif /* PMD85SESSION_INTERNAL_H */
