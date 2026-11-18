/*
 * pmd85session -- session lifecycle and the paced emulator loop.
 *
 * The emulator thread runs pmd85host_core_run_frame (one 20 ms PMD frame) on
 * an absolute-deadline wall-clock sleeper. Frames are published to a
 * latest-frame store the UI thread pulls on its own tick, so a stalled paint
 * can never freeze the 8080 -- and therefore never stall the DriveWire
 * traffic FujiNet depends on.
 *
 * Unlike the ADAM and Apple II ports there is no vsync phase-lock: the PMD 85
 * is a 50 Hz machine and no desktop display ticks at 50 Hz, so locking one
 * emulated frame to each display tick would run it 20% fast (the CoCo app
 * declines to lock at 50 Hz for the same reason). The wall clock is correct.
 *
 * Shape and lifecycle from fujinet-go-coco-desktop's session.c, which has the
 * same transport direction (FujiNet listens, the machine connects out).
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "compat.h"
#include "pmd85_host.h"
#include "session_internal.h"

/****************************************************************************/
/** Small helpers                                                          **/
/****************************************************************************/

void session_set_error(pmd85session *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->last_error, sizeof(s->last_error), fmt, ap);
    va_end(ap);
    fprintf(stderr, "pmd85session: %s\n", s->last_error);
}

static long read_timer_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000000L + ts.tv_nsec / 1000L);
}

static void core_log_sink(int level, const char *msg, void *user)
{
    (void)user;
    if (level >= 1) fprintf(stderr, "pmd85core: %s\n", msg);
}

/****************************************************************************/
/** Option tables                                                          **/
/****************************************************************************/

static const char *const k_models[] = {
    "PMD 85-1", "PMD 85-2", "PMD 85-2A", "PMD 85-3",
    "Mato", "Didaktik Alfa", "Didaktik Alfa 2", "Consul 2717", NULL
};
static const char *const k_colors[] = {
    "Black & White", "Green", "RGBM", "ColorACE", NULL
};
static const char *const k_modules[] = {
    "FujiNet", "BASIC-G", "None", "Module file", NULL
};

#define MODULE_FILE_MAX (8L * 1024 * 1024)   /* the MegaModule's 256 x 32 KB */
#define ROM_PACK_MAX    (32L * 1024)

/* Per model: the monitor ROM, and the stock BASIC-G module package (upstream's
 * res/default.conf [Model-*] and [RomModulePackages]). NULL = the model has no
 * ROM-module slot (Mato, Consul 2717: their BASIC is in the system ROM). */
static const struct {
    const char *monitor;
    const char *basic[3];
} k_model_roms[] = {
    /* 1     */ { "monit1.rom",  { "basic1.rmm", NULL } },
    /* 2     */ { "monit2.rom",  { "basic2.rmm", NULL } },
    /* 2A    */ { "monit2A.rom", { "basic2A.rmm", NULL } },
    /* 3     */ { "monit3.rom",  { "basic3.rmm", NULL } },
    /* Mato  */ { "mato-mb.rom", { NULL } },
    /* Alfa  */ { "didalfa.rom", { "basic1.rmm", NULL } },
    /* Alfa2 */ { "didalfa2.rom", { "basic2.rmm", NULL } },
    /* C2717 */ { "c2717.rom",   { NULL } },
};

#define FUJINET_LOADER_RMM "fujinet-config-loader.rmm"

static const char *table_at(const char *const *tab, int idx)
{
    int n = 0;
    if (idx < 0) return NULL;
    while (tab[n]) n++;
    return idx < n ? tab[idx] : NULL;
}

static int clamp_opt(int v, const char *const *tab, int def)
{
    return table_at(tab, v) ? v : def;
}

const char *pmd85session_model_name(int idx)  { return table_at(k_models, idx); }
const char *pmd85session_color_name(int idx)  { return table_at(k_colors, idx); }
const char *pmd85session_module_name(int idx) { return table_at(k_modules, idx); }

int pmd85session_module_supported(int model, int module)
{
    if (!table_at(k_models, model) || !table_at(k_modules, module))
        return 0;
    if (module == PMD85SESSION_MODULE_NONE)
        return 1;
    /* Mato and the Consul 2717 have no ROM-module slot. */
    if (model == PMD85SESSION_MODEL_MATO || model == PMD85SESSION_MODEL_C2717)
        return 0;
    /* Every model with a slot takes the FujiNet MegaModule. The 2 and 2A
     * monitors auto-run the loader in it; elsewhere it sits in the slot to be
     * started from the monitor, and a module change must not empty it. */
    return 1;
}

const char *pmd85session_module_hint(pmd85session *s, int model, int module)
{
    if (module != PMD85SESSION_MODULE_FUJINET ||
        !pmd85session_module_supported(model, module))
        return NULL;
    switch (model) {
    case PMD85SESSION_MODEL_1:
        return "Start FujiNet from the monitor: JOB 000001100000";
    case PMD85SESSION_MODEL_3:
        if (s && pmd85session_get_int(s, "compat_mode", 0))
            return NULL;
        return "Start FujiNet with JUMP FFF0 (85-2 mode), or turn on "
               "PMD 85-2 compatibility in Preferences";
    case PMD85SESSION_MODEL_2:
    case PMD85SESSION_MODEL_2A:
        return NULL;   /* the monitor auto-runs it */
    default:
        return "Start the FujiNet module from the monitor";
    }
}

/****************************************************************************/
/** The machine configuration                                              **/
/****************************************************************************/

static void rom_path(const pmd85session *s, char *dst, const char *name)
{
    snprintf(dst, PMD85HOST_PATH_MAX, "%s/%s", s->roms_dir, name);
}

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    n = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    fclose(f);
    return n;
}

/* A .mrm, or an image too big for the 32 KB ROM pack, needs the MegaModule. */
static int module_is_mega(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot && (dot[1] == 'm' || dot[1] == 'M') && (dot[2] == 'r' || dot[2] == 'R') &&
        (dot[3] == 'm' || dot[3] == 'M') && !dot[4])
        return 1;
    return file_size(path) > ROM_PACK_MAX;
}

/* Everything the host needs to build the machine, from the session's state
 * and its settings. */
static void apply_tape_options(pmd85session *s);

static void build_config(pmd85session *s, pmd85host_config *c)
{
    int model = s->cur_model;
    int module = s->cur_module;
    int i;
    const char *mon;

    pmd85host_config_defaults(c);
    c->model = model;
    c->color_profile = s->cur_color;

    /* A replacement monitor (e.g. monit2E, monit1cs) by setting. */
    {
        char key[32];
        snprintf(key, sizeof key, "monitor_%d", model);
        mon = pmd85session_get_str(s, key, k_model_roms[model].monitor);
        if (!mon || !*mon) mon = k_model_roms[model].monitor;
    }
    rom_path(s, c->monitor, mon);

    c->ram256k = pmd85session_get_int(s, "ram256k", 0) ? 1 : 0;
    c->compat_mode = pmd85session_get_int(s, "compat_mode", 0) ? 1 : 0;
    c->mif85 = s->opts.mif85;
    c->pmd32 = s->opts.pmd32;
    c->joystick_gpio0 = s->opts.joystick ? 1 : 0;   /* the host leaves A to a PMD 32 */
    c->joystick_gpio1 = s->opts.joystick ? 1 : 0;
    c->exchange_zy = pmd85session_get_int(s, "exchange_zy", 1) ? 1 : 0;
    c->numpad = pmd85session_get_int(s, "numpad", 1) ? 1 : 0;
    c->volume = pmd85session_get_int(s, "volume", 96);
    if (c->volume < 0) c->volume = 0;
    if (c->volume > 127) c->volume = 127;

    if (!pmd85session_module_supported(model, module))
        module = PMD85SESSION_MODULE_NONE;
    switch (module) {
    case PMD85SESSION_MODULE_FUJINET:
        c->rom_module = PMD85_ROMMOD_MEGA;
        rom_path(s, c->mrm, FUJINET_LOADER_RMM);
        break;
    case PMD85SESSION_MODULE_BASIC:
        c->rom_module = PMD85_ROMMOD_PACK;
        for (i = 0; i < 3 && k_model_roms[model].basic[i]; i++)
            rom_path(s, c->rmm[c->rmm_count++], k_model_roms[model].basic[i]);
        break;
    case PMD85SESSION_MODULE_FILE: {
        const char *path = pmd85session_module_file(s);
        if (!*path) {
            c->rom_module = PMD85_ROMMOD_NONE;
        } else if (module_is_mega(path)) {
            c->rom_module = PMD85_ROMMOD_MEGA;
            snprintf(c->mrm, sizeof c->mrm, "%s", path);
        } else {
            c->rom_module = PMD85_ROMMOD_PACK;
            snprintf(c->rmm[c->rmm_count++], sizeof c->rmm[0], "%s", path);
        }
        break;
    }
    default:
        c->rom_module = PMD85_ROMMOD_NONE;
        break;
    }

    /* The Becker port whenever there is a FujiNet to talk to. */
    c->becker = (s->fujinet_running || s->fujinet_external) ? 1 : 0;
    snprintf(c->becker_host, sizeof c->becker_host, "%s", s->becker_host);
    c->becker_port = s->becker_port;
}

/****************************************************************************/
/** Frame store                                                            **/
/****************************************************************************/

static void on_core_frame(const uint32_t *px, int w, int h, void *user)
{
    pmd85session *s = user;
    size_t n;

    if (w <= 0 || h <= 0 || w > PMD85SESSION_FB_MAX_WIDTH ||
        h > PMD85SESSION_FB_MAX_HEIGHT)
        return;

    n = (size_t)w * (size_t)h;
    pthread_mutex_lock(&s->frame_mtx);
    /* An identical frame is not a new frame: the serial stays and nobody is
     * called. Most of what FujiNet CONFIG or a monitor prompt shows does not
     * move. */
    if (s->frame_w == w && s->frame_h == h &&
        memcmp(s->frame, px, n * sizeof(uint32_t)) == 0) {
        pthread_mutex_unlock(&s->frame_mtx);
        return;
    }
    memcpy(s->frame, px, n * sizeof(uint32_t));
    s->frame_w = w;
    s->frame_h = h;
    s->frame_serial++;
    pthread_mutex_unlock(&s->frame_mtx);

    pthread_mutex_lock(&s->cb_mtx);
    if (s->frame_cb)
        s->frame_cb(s->frame_cb_ctx);
    pthread_mutex_unlock(&s->cb_mtx);
}

void pmd85session_set_frame_callback(pmd85session *s, pmd85session_frame_cb cb,
                                     void *ctx)
{
    pthread_mutex_lock(&s->cb_mtx);
    s->frame_cb = cb;
    s->frame_cb_ctx = ctx;
    pthread_mutex_unlock(&s->cb_mtx);
}

uint64_t pmd85session_frame_count(pmd85session *s)
{
    return s->frame_count;
}

int pmd85session_copy_frame(pmd85session *s, uint32_t *dst,
                            uint64_t *serial_inout, int *w_out, int *h_out)
{
    int copied = 0;
    pthread_mutex_lock(&s->frame_mtx);
    if (s->frame_serial != *serial_inout && s->frame_w > 0) {
        memcpy(dst, s->frame,
               (size_t)s->frame_w * (size_t)s->frame_h * sizeof(uint32_t));
        *serial_inout = s->frame_serial;
        if (w_out) *w_out = s->frame_w;
        if (h_out) *h_out = s->frame_h;
        copied = 1;
    }
    pthread_mutex_unlock(&s->frame_mtx);
    return copied;
}

void pmd85session_notify_vsync(pmd85session *s, int64_t frame_time_ns)
{
    if (!s) return;
    pthread_mutex_lock(&s->vs_mtx);
    s->vs_ns = frame_time_ns;
    pthread_mutex_unlock(&s->vs_mtx);
}

int pmd85session_frame_rate_mhz(const pmd85session *s)
{
    (void)s;
    return PMD85HOST_FRAME_RATE_MHZ;
}

/****************************************************************************/
/** Emulator thread                                                        **/
/****************************************************************************/

static void *emu_thread_main(void *arg)
{
    pmd85session *s = arg;
    long next_us;
    const long frame_us = 1000000000L / PMD85HOST_FRAME_RATE_MHZ;  /* 20000 */
    const int pace_log = getenv("PMD85_PACE_LOG") != NULL;
    long log_t0 = 0;
    int log_frames = 0, log_behind = 0;
    pmd85host_config cfg;

    pmd85_thread_setname("pmd85-emu");

    pmd85host_set_log_sink(core_log_sink, s);
    pmd85host_set_frame_sink(on_core_frame, s);

    build_config(s, &cfg);
    {
        int ok = pmd85host_core_start(&cfg);
        if (!ok) {
            session_set_error(s, "The PMD 85 could not start: %s",
                              pmd85host_last_error());
            pmd85host_set_frame_sink(NULL, NULL);
        }
        pthread_mutex_lock(&s->start_mtx);
        s->core_started = ok ? 1 : -1;
        pthread_cond_broadcast(&s->start_cv);
        pthread_mutex_unlock(&s->start_mtx);
        if (!ok) {
            s->running = 0;
            return NULL;
        }
    }

    next_us = read_timer_us();
    while (!s->stop_flag) {
        long now;

        pmd85host_core_run_frame();
        s->frame_count++;

        now = read_timer_us();
        next_us += frame_us;
        log_frames++;
        if (now >= next_us) log_behind++;
        if (log_t0 == 0) log_t0 = now;
        if (now - log_t0 >= 1000000L) {
            if (pace_log)
                fprintf(stderr, "pmd85session pace: %d fps over %ld ms "
                                "(%d behind)\n",
                        log_frames, (now - log_t0) / 1000, log_behind);
            log_t0 = now;
            log_frames = log_behind = 0;
        }
        if (now < next_us)
            pmd85_sleep_until_us(next_us);
        else if (now - next_us > 100000L)
            next_us = now;  /* a long stall (a debugger stop): don't race to
                               catch up, resume real time from here */
    }

    pmd85host_core_stop();
    pmd85host_set_frame_sink(NULL, NULL);
    s->running = 0;
    return NULL;
}

/****************************************************************************/
/** Lifecycle                                                              **/
/****************************************************************************/

pmd85session *pmd85session_new(const pmd85session_paths *paths)
{
    pmd85session *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->frame = calloc(PMD85_FB_MAX_PIXELS, sizeof(uint32_t));
    if (!s->frame) {
        free(s);
        return NULL;
    }
    if (paths_init(s, paths) != 0) {
        free(s->frame);
        free(s);
        return NULL;
    }

    pthread_mutex_init(&s->lifecycle_mtx, NULL);
    pthread_mutex_init(&s->frame_mtx, NULL);
    pthread_mutex_init(&s->cb_mtx, NULL);
    pthread_mutex_init(&s->vs_mtx, NULL);
    pthread_mutex_init(&s->start_mtx, NULL);
    pthread_mutex_init(&s->input_mtx, NULL);
    pthread_mutex_init(&s->joy_mtx, NULL);
    pthread_mutex_init(&s->pad_event_mtx, NULL);
    pmd85_cond_init_monotonic(&s->start_cv);

    settings_init(s);

    /* Publish the persisted selection at once, so the frontends' menus show
     * what the user has selected even before (or without) a start. */
    {
        pmd85session_start_opts opts;
        pmd85session_default_opts(s, &opts);
        s->cur_model = opts.model;
        s->cur_color = opts.color_profile;
        s->cur_module = opts.module;
    }
    snprintf(s->becker_host, sizeof(s->becker_host), "127.0.0.1");
    s->becker_port = PMD85SESSION_BECKER_PORT;
    return s;
}

void pmd85session_free(pmd85session *s)
{
    if (!s) return;
    pmd85session_stop(s);
    pmd85session_settings_flush(s);
    settings_free_all(s);
    pmd85debug_destroy(s->debugger);
    pthread_mutex_destroy(&s->lifecycle_mtx);
    pthread_mutex_destroy(&s->frame_mtx);
    pthread_mutex_destroy(&s->cb_mtx);
    pthread_mutex_destroy(&s->vs_mtx);
    pthread_mutex_destroy(&s->start_mtx);
    pthread_mutex_destroy(&s->input_mtx);
    pthread_mutex_destroy(&s->joy_mtx);
    pthread_mutex_destroy(&s->pad_event_mtx);
    pthread_cond_destroy(&s->start_cv);
    free(s->frame);
    free(s);
}

void pmd85session_default_opts(pmd85session *s, pmd85session_start_opts *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->model = clamp_opt(pmd85session_get_int(s, "model", PMD85SESSION_MODEL_2A),
                            k_models, PMD85SESSION_MODEL_2A);
    {
        /* "colors"; before it "color_profile": Mono, Standard, Colour, ColorACE */
        static const int old_map[] = { PMD85SESSION_COLOR_BW, PMD85SESSION_COLOR_BW,
                                       PMD85SESSION_COLOR_RGBM, PMD85SESSION_COLOR_COLORACE };
        int c = pmd85session_get_int(s, "colors", -1);
        if (c < 0) {
            int old = pmd85session_get_int(s, "color_profile", -1);
            c = (old >= 0 && old < 4) ? old_map[old] : PMD85SESSION_COLOR_BW;
        }
        opts->color_profile = clamp_opt(c, k_colors, PMD85SESSION_COLOR_BW);
    }
    opts->module = clamp_opt(pmd85session_get_int(s, "module",
                                                  PMD85SESSION_MODULE_FUJINET),
                             k_modules, PMD85SESSION_MODULE_FUJINET);
    opts->ram256k = pmd85session_get_int(s, "ram256k", 0) ? 1 : 0;
    opts->mif85 = pmd85session_get_int(s, "mif85", 0) ? 1 : 0;
    opts->pmd32 = pmd85session_get_int(s, "pmd32", 0) ? 1 : 0;
    opts->joystick = pmd85session_get_int(s, "joystick", 1) ? 1 : 0;
    opts->enable_fujinet = pmd85session_get_int(s, "enable_fujinet", 1) ? 1 : 0;
    opts->external_fujinet = pmd85session_get_int(s, "fujinet_external", 0) ? 1 : 0;
    snprintf(opts->fujinet_addr, sizeof(opts->fujinet_addr), "%s",
             pmd85session_get_str(s, "fujinet_external_addr", ""));
    opts->enable_audio = pmd85session_get_int(s, "enable_audio", 1) ? 1 : 0;
    opts->enable_gamepad = pmd85session_get_int(s, "enable_gamepad", 1) ? 1 : 0;
}

int pmd85session_start(pmd85session *s, const pmd85session_start_opts *opts)
{
    pthread_mutex_lock(&s->lifecycle_mtx);
    if (s->running) {
        pthread_mutex_unlock(&s->lifecycle_mtx);
        return 0;
    }

    s->opts = *opts;
    s->cur_model = clamp_opt(opts->model, k_models, PMD85SESSION_MODEL_2A);
    s->cur_color = clamp_opt(opts->color_profile, k_colors, PMD85SESSION_COLOR_BW);
    s->cur_module = clamp_opt(opts->module, k_modules, PMD85SESSION_MODULE_FUJINET);
    s->last_error[0] = '\0';

    /* FUJINET MUST BE UP BEFORE THE MACHINE. FujiNet is the DriveWire server
     * listening on the Becker port and the PMD connects out to it -- and
     * fujinet_start() returns before that listener is up (the DriveWire bus
     * waits a second first). FujiNet CONFIG starts talking DriveWire the
     * moment the PMD boots, so wait for the listener to accept. Neither a
     * missing runtime nor a timeout is fatal: the machine boots regardless,
     * and the Becker device keeps retrying the connection. */
    s->fujinet_external = 0;
    s->fujinet_warning[0] = '\0';
    snprintf(s->becker_host, sizeof(s->becker_host), "127.0.0.1");
    s->becker_port = PMD85SESSION_BECKER_PORT;
    {
        const char *env = getenv("PMD85_BECKER_PORT");
        int p = env ? atoi(env) : 0;
        if (p > 0 && p < 65536) s->becker_port = p;
    }
    if (opts->external_fujinet) {
        if (pmd85session_parse_hostport(opts->fujinet_addr, s->becker_host,
                                        sizeof(s->becker_host),
                                        &s->becker_port) != 0) {
            session_set_error(s, "\"%s\" is not an address:port for the "
                              "external FujiNet (example: 192.168.1.20:%d)",
                              opts->fujinet_addr, PMD85SESSION_BECKER_PORT);
            pthread_mutex_unlock(&s->lifecycle_mtx);
            return -1;
        }
        s->fujinet_external = 1;
        s->webui_url[0] = '\0';
    } else if (opts->enable_fujinet) {
        /* PORTING.md gotcha 20: a standalone fujinet-pc (or another copy of
         * this app) already on the port gets the PMD's connection instead of
         * ours, and everything looks like it works -- with the wrong
         * configuration and SD card. Ours then fails to bind and retries. */
        if (fujinet_port_in_use(s->becker_port)) {
            snprintf(s->fujinet_warning, sizeof(s->fujinet_warning),
                     "Another FujiNet is already listening on 127.0.0.1:%d; "
                     "the PMD will talk to it rather than the built-in one. "
                     "Stop it, or set PMD85_BECKER_PORT.", s->becker_port);
            fprintf(stderr, "pmd85session: %s\n", s->fujinet_warning);
        }
        if (fujinet_start(s) == 0)
            fujinet_wait_for_becker(s, 10000);
    }
    snprintf(s->becker_endpoint, sizeof(s->becker_endpoint), "%s:%d",
             s->becker_host, s->becker_port);

    /* The debugger's FujiNet trace runs from here, not from when the
     * debugger opens: it is most useful about what happened first. */
    if (!s->debugger)
        s->debugger = pmd85debug_create(s);
    pmd85debug_trace_install(s->debugger);

    apply_tape_options(s);

    s->stop_flag = 0;
    s->frame_serial = 0;
    s->frame_w = 0;
    s->frame_h = 0;
    s->core_started = 0;
    memset(s->key_down, 0, sizeof(s->key_down));
    s->running = 1;
    if (pthread_create(&s->emu_thread, NULL, emu_thread_main, s) != 0) {
        s->running = 0;
        session_set_error(s, "could not start the emulator thread");
        fujinet_stop(s);
        pthread_mutex_unlock(&s->lifecycle_mtx);
        return -1;
    }
    s->emu_thread_started = 1;

    {
        int up;
        pthread_mutex_lock(&s->start_mtx);
        while (s->core_started == 0) {
            if (pmd85_cond_timedwait_ms(&s->start_cv, &s->start_mtx, 15000)
                == ETIMEDOUT)
                break;
        }
        up = s->core_started;
        pthread_mutex_unlock(&s->start_mtx);

        if (up != 1) {
            s->stop_flag = 1;
            pthread_join(s->emu_thread, NULL);
            s->emu_thread_started = 0;
            s->running = 0;
            fujinet_stop(s);
            if (!s->last_error[0])
                session_set_error(s, "the emulator core did not come up");
            pthread_mutex_unlock(&s->lifecycle_mtx);
            return -1;
        }
    }

    if (opts->enable_audio)
        audio_start(s);
    if (opts->enable_gamepad && gamepad_start(s) != 0) {
        fprintf(stderr, "pmd85session: no gamepads: %s\n", s->last_error);
        s->last_error[0] = '\0';   /* the session itself is up */
    }

    /* the tape that was in the deck last time (upstream does the same); a
     * deck that still has a tape -- a session restart -- keeps it */
    {
        const char *tape = pmd85session_get_str(s, "tape_file", "");
        pmd85host_tape_state st;
        pmd85host_tape_state_get(&st);
        if (tape && *tape && st.blocks == 0) {
            char path[PMD85_PATH_MAX];
            snprintf(path, sizeof path, "%s", tape);
            char err[512] = "";
            if (pmd85host_tape_command(PMD85_TAPE_LOAD, path, err, sizeof err) < 0) {
                fprintf(stderr, "pmd85session: the tape %s is not back in: %s\n", path, err);
                pmd85session_set_str(s, "tape_file", "");
            }
        }
    }

    pthread_mutex_unlock(&s->lifecycle_mtx);
    return 0;
}

void pmd85session_stop(pmd85session *s)
{
    pthread_mutex_lock(&s->lifecycle_mtx);
    if (!s->emu_thread_started) {
        pthread_mutex_unlock(&s->lifecycle_mtx);
        return;
    }
    s->stop_flag = 1;
    if (s->debugger)
        pmd85debug_resume_for_stop(s->debugger);
    pthread_mutex_lock(&s->start_mtx);
    if (s->core_started == 0) s->core_started = -1;
    pthread_cond_broadcast(&s->start_cv);
    pthread_mutex_unlock(&s->start_mtx);
    /* Unblock a fill_audio parked waiting for samples the stopped emulator
     * will never produce. */
    pmd85host_audio_set_active(0);
    pthread_join(s->emu_thread, NULL);
    s->emu_thread_started = 0;
    audio_stop(s);
    gamepad_stop(s);
    joy_input_clear_all(s);
    fujinet_stop(s);
    s->fujinet_external = 0;
    pmd85host_audio_set_active(1);
    s->running = 0;
    pthread_mutex_unlock(&s->lifecycle_mtx);
}

int pmd85session_is_running(const pmd85session *s)
{
    return s && s->running;
}

const char *pmd85session_last_error(const pmd85session *s)
{
    return s->last_error;
}

/****************************************************************************/
/** Live machine changes                                                   **/
/****************************************************************************/

static void reconfigure(pmd85session *s)
{
    pmd85host_config cfg;
    if (!s->running) return;
    build_config(s, &cfg);
    pmd85host_core_reconfigure(&cfg);
}

void pmd85session_set_model(pmd85session *s, int model)
{
    if (!s) return;
    model = clamp_opt(model, k_models, PMD85SESSION_MODEL_2A);
    pmd85session_release_all(s);
    s->cur_model = model;
    pmd85session_set_int(s, "model", model);
    reconfigure(s);
}

void pmd85session_set_color_profile(pmd85session *s, int profile)
{
    if (!s) return;
    profile = clamp_opt(profile, k_colors, PMD85SESSION_COLOR_BW);
    s->cur_color = profile;
    pmd85session_set_int(s, "colors", profile);
    /* The colour profile is only how frames are rendered, but the host keeps
     * it in the machine's config; a rebuild would reboot the machine, so the
     * host is told through the same path with the machine kept. */
    if (s->running)
        pmd85host_set_color_profile(profile);
}

void pmd85session_set_module(pmd85session *s, int module)
{
    if (!s) return;
    module = clamp_opt(module, k_modules, PMD85SESSION_MODULE_FUJINET);
    s->cur_module = module;
    pmd85session_set_int(s, "module", module);
    reconfigure(s);
}

const char *pmd85session_module_file(pmd85session *s)
{
    const char *p = s ? pmd85session_get_str(s, "module_file", "") : "";
    return p ? p : "";
}

int pmd85session_module_file_ok(pmd85session *s)
{
    return file_size(pmd85session_module_file(s)) > 0;
}

int pmd85session_set_module_file(pmd85session *s, const char *path)
{
    long size;
    if (!s) return -1;
    if (!path || !*path) {
        session_set_error(s, "No module file given");
        return -1;
    }
    size = file_size(path);
    if (size < 0) {
        session_set_error(s, "Cannot read %s", path);
        return -1;
    }
    if (size == 0 || size > MODULE_FILE_MAX) {
        session_set_error(s, "%s is not a ROM module image (%ld bytes; 1 B to 8 MB)",
                          path, size);
        return -1;
    }
    pmd85session_set_str(s, "module_file", path);
    s->cur_module = PMD85SESSION_MODULE_FILE;
    pmd85session_set_int(s, "module", PMD85SESSION_MODULE_FILE);
    reconfigure(s);
    return 0;
}

void pmd85session_set_peripheral(pmd85session *s, const char *key, int on)
{
    if (!s || !key) return;
    on = on ? 1 : 0;
    if (!strcmp(key, "mif85")) s->opts.mif85 = on;
    else if (!strcmp(key, "pmd32")) s->opts.pmd32 = on;
    else if (!strcmp(key, "joystick")) s->opts.joystick = on;
    else if (!strcmp(key, "ram256k")) s->opts.ram256k = on;
    else if (strcmp(key, "compat_mode") != 0) return; /* read from settings */
    pmd85session_set_int(s, key, on);
    reconfigure(s);
}

int pmd85session_model(const pmd85session *s)         { return s ? s->cur_model : 0; }
int pmd85session_color_profile(const pmd85session *s) { return s ? s->cur_color : 0; }
int pmd85session_module(const pmd85session *s)        { return s ? s->cur_module : 0; }

void pmd85session_reset(pmd85session *s)
{
    if (s && s->running) pmd85host_core_reset();
}

void pmd85session_hard_reset(pmd85session *s)
{
    if (s) reconfigure(s);
}

int pmd85session_leds(const pmd85session *s)
{
    return (s && s->running) ? pmd85host_led_state() : 0;
}

int pmd85session_mega_page(const pmd85session *s)
{
    return (s && s->running) ? pmd85host_mega_page() : -1;
}

int pmd85session_mega_size(const pmd85session *s)
{
    return (s && s->running) ? pmd85host_mega_size() : 0;
}

/****************************************************************************/
/** Input / audio                                                          **/
/****************************************************************************/

void pmd85session_key(pmd85session *s, int down, uint32_t keysym,
                      uint32_t unicode, int ctrl_down, int shift_down)
{
    unsigned usage;
    if (!s) return;
    if (!pmd85_key_from_event(keysym, unicode, ctrl_down, shift_down, &usage))
        return;
    if (usage == 0 || usage > 255) return;
    pthread_mutex_lock(&s->input_mtx);
    s->key_down[usage] = down ? 1 : 0;
    pthread_mutex_unlock(&s->input_mtx);
    pmd85host_inject_key(down, (int)usage);
}

void pmd85session_release_all(pmd85session *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->input_mtx);
    memset(s->key_down, 0, sizeof(s->key_down));
    pthread_mutex_unlock(&s->input_mtx);
    pmd85host_release_all_keys();
}

/****************************************************************************/
/** Joysticks: every source merged (joy_input.h)                           **/
/****************************************************************************/

void joy_input_set(pmd85session *s, int src, int port, unsigned bits)
{
    unsigned col[PMD85_SRC_COUNT], merged;
    if (!s || src < 0 || src >= PMD85_SRC_COUNT || port < 0 || port >= PMD85_JOY_PORTS)
        return;
    pthread_mutex_lock(&s->joy_mtx);
    s->joy_held[src][port] = bits & 0x1F;
    for (int i = 0; i < PMD85_SRC_COUNT; i++) col[i] = s->joy_held[i][port];
    merged = pmd85_joy_merge(col, PMD85_SRC_COUNT);
    pmd85host_set_joystick(port, merged);
    pthread_mutex_unlock(&s->joy_mtx);
}

void joy_input_clear_pad(pmd85session *s, int owner)
{
    if (owner < 0 || owner >= PMD85_MAX_PADS) return;
    for (int p = 0; p < PMD85_JOY_PORTS; p++) {
        joy_input_set(s, PMD85_SRC_PAD(owner), p, 0);
        joy_input_set(s, PMD85_SRC_STICK(owner), p, 0);
    }
}

void joy_input_clear_all(pmd85session *s)
{
    pthread_mutex_lock(&s->joy_mtx);
    memset(s->joy_held, 0, sizeof s->joy_held);
    for (int p = 0; p < PMD85_JOY_PORTS; p++) pmd85host_set_joystick(p, 0);
    pthread_mutex_unlock(&s->joy_mtx);
}

void pmd85session_set_joystick(pmd85session *s, int port, unsigned bits)
{
    joy_input_set(s, PMD85_SRC_FRONTEND, port, bits);
}

unsigned pmd85session_joystick_held(pmd85session *s, int port)
{
    unsigned col[PMD85_SRC_COUNT], r;
    if (!s || port < 0 || port >= PMD85_JOY_PORTS) return 0;
    pthread_mutex_lock(&s->joy_mtx);
    for (int i = 0; i < PMD85_SRC_COUNT; i++) col[i] = s->joy_held[i][port];
    r = pmd85_joy_merge(col, PMD85_SRC_COUNT);
    pthread_mutex_unlock(&s->joy_mtx);
    return r;
}

unsigned session_joysticks(pmd85session *s)
{
    /* as build_config wires them: port A is the PMD 32's when it is there */
    if (!s->opts.joystick) return 0;
    return (s->opts.pmd32 ? 0u : 1u) | 2u;
}

unsigned pmd85session_joysticks(pmd85session *s)
{
    return s ? session_joysticks(s) : 0;
}

void session_gamepad_event(pmd85session *s, const char *text)
{
    pthread_mutex_lock(&s->pad_event_mtx);
    snprintf(s->pad_event, sizeof s->pad_event, "%s", text ? text : "");
    s->pad_event_serial++;
    pthread_mutex_unlock(&s->pad_event_mtx);
}

unsigned pmd85session_gamepad_last_event(pmd85session *s, char *dst, int dstsz)
{
    unsigned serial;
    if (!s) return 0;
    pthread_mutex_lock(&s->pad_event_mtx);
    if (dst && dstsz > 0) snprintf(dst, (size_t)dstsz, "%s", s->pad_event);
    serial = s->pad_event_serial;
    pthread_mutex_unlock(&s->pad_event_mtx);
    return serial;
}

int pmd85session_render_audio(pmd85session *s, int16_t *out, int nsamples)
{
    if (!s->running || s->audio_mute) {
        memset(out, 0, (size_t)nsamples * sizeof(int16_t));
        return nsamples;
    }
    pmd85host_fill_audio(out, nsamples);
    fujinet_mix_audio(s, out, nsamples, PMD85HOST_AUDIO_RATE);
    return nsamples;
}

void pmd85session_set_mute(pmd85session *s, int mute)
{
    if (s) s->audio_mute = mute ? 1 : 0;
}

/****************************************************************************/
/** FujiNet                                                                **/
/****************************************************************************/

const char *pmd85session_fujinet_webui_url(const pmd85session *s) { return s->webui_url; }
int pmd85session_fujinet_running(const pmd85session *s)  { return s->fujinet_running; }
int pmd85session_fujinet_external(const pmd85session *s) { return s->fujinet_external; }

const char *pmd85session_fujinet_warning(const pmd85session *s)
{
    return s ? s->fujinet_warning : "";
}

const char *pmd85session_becker_endpoint(const pmd85session *s)
{
    return s->becker_endpoint[0] ? s->becker_endpoint : "127.0.0.1:65505";
}

/****************************************************************************/
/** The tape deck                                                          **/
/****************************************************************************/

static void apply_tape_options(pmd85session *s)
{
    pmd85host_tape_options(pmd85session_get_int(s, "tape_flash", 1),
                           pmd85session_get_int(s, "tape_sound", 0));
    pmd85host_tape_autostop(pmd85session_tape_autostop(s));
}

int pmd85session_tape_autostop(pmd85session *s)
{
    int m = s ? pmd85session_get_int(s, "tape_autostop", PMD85SESSION_TAPE_STOP_HEADER)
              : PMD85SESSION_TAPE_STOP_HEADER;
    return (m >= PMD85SESSION_TAPE_STOP_END && m <= PMD85SESSION_TAPE_STOP_MARKER)
           ? m : PMD85SESSION_TAPE_STOP_HEADER;
}

void pmd85session_set_tape_autostop(pmd85session *s, int mode)
{
    if (!s) return;
    pmd85session_set_int(s, "tape_autostop", mode);
    apply_tape_options(s);
}

void pmd85session_set_tape_options(pmd85session *s, int flash, int sound)
{
    if (!s) return;
    pmd85session_set_int(s, "tape_flash", flash ? 1 : 0);
    pmd85session_set_int(s, "tape_sound", sound ? 1 : 0);
    apply_tape_options(s);
}

static int tape_cmd(pmd85session *s, int cmd, const char *path)
{
    char err[512] = "";
    int rc;
    if (!s) return -1;
    if (!s->running) {
        session_set_error(s, "The PMD is not running");
        return -1;
    }
    rc = pmd85host_tape_command(cmd, path, err, sizeof err);
    if (rc != 0)
        session_set_error(s, "%s", err);
    return rc;
}

int pmd85session_tape_load(pmd85session *s, const char *path)
{
    int rc = tape_cmd(s, PMD85_TAPE_LOAD, path);
    if (rc >= 0)
        pmd85session_set_str(s, "tape_file", path);
    return rc;
}

int pmd85session_tape_eject(pmd85session *s)
{
    int rc = tape_cmd(s, PMD85_TAPE_EJECT, NULL);
    if (rc == 0)
        pmd85session_set_str(s, "tape_file", "");
    return rc;
}

int pmd85session_tape_play(pmd85session *s)   { return tape_cmd(s, PMD85_TAPE_PLAY, NULL); }
int pmd85session_tape_stop(pmd85session *s)   { return tape_cmd(s, PMD85_TAPE_STOP, NULL); }
int pmd85session_tape_rewind(pmd85session *s) { return tape_cmd(s, PMD85_TAPE_REWIND, NULL); }

int pmd85session_tape_append(pmd85session *s, const char *path)
{
    pmd85host_tape_state st;
    int rc;
    pmd85host_tape_state_get(&st);
    rc = tape_cmd(s, PMD85_TAPE_APPEND, path);
    if (rc >= 0 && st.blocks == 0)  /* onto no tape: loaded */
        pmd85session_set_str(s, "tape_file", path);
    return rc;
}

static int tape_edit(pmd85session *s, int cmd, int idx, const int *sel, int nsel)
{
    char err[512] = "";
    int rc;
    if (!s) return -1;
    if (!s->running) {
        session_set_error(s, "The PMD is not running");
        return -1;
    }
    rc = pmd85host_tape_edit(cmd, idx, sel, nsel, err, sizeof err);
    if (rc != 0)
        session_set_error(s, "%s", err);
    return rc;
}

int pmd85session_tape_seek(pmd85session *s, int idx)
{
    return tape_edit(s, PMD85_TAPE_SEEK, idx, NULL, 0);
}

int pmd85session_tape_mark(pmd85session *s, int idx)
{
    return tape_edit(s, PMD85_TAPE_MARK, idx, NULL, 0);
}

int pmd85session_tape_delete(pmd85session *s, int idx, const int *sel, int nsel)
{
    return tape_edit(s, PMD85_TAPE_DELETE, idx, sel, nsel);
}

int pmd85session_tape_move(pmd85session *s, int up, int idx, const int *sel, int nsel)
{
    return tape_edit(s, up ? PMD85_TAPE_MOVE_UP : PMD85_TAPE_MOVE_DOWN, idx, sel, nsel);
}

int pmd85session_tape_blocks(pmd85session *s, pmd85session_tape_block *out, int max,
                             unsigned *serial)
{
    pmd85host_tape_block *tmp = NULL;
    int n;
    (void)s;
    if (out && max > 0 && !(tmp = calloc((size_t)max, sizeof *tmp)))
        return 0;
    n = pmd85host_tape_blocks(tmp, tmp ? max : 0, serial);
    for (int i = 0; tmp && i < n && i < max; i++) {
        memcpy(out[i].name, tmp[i].name, sizeof out[i].name);
        out[i].number = tmp[i].number;
        out[i].start = tmp[i].start;
        out[i].length = tmp[i].length;
        out[i].bad = tmp[i].bad;
    }
    free(tmp);
    return n;
}

int pmd85session_tape_save(pmd85session *s, const char *path)
{
    int rc = tape_cmd(s, PMD85_TAPE_SAVE, path);
    if (rc == 0)
        pmd85session_set_str(s, "tape_file", path);
    return rc;
}

void pmd85session_tape_get_info(pmd85session *s, pmd85session_tape_info *out)
{
    pmd85host_tape_state st;
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->current = -1;
    if (!s) return;
    pmd85host_tape_state_get(&st);
    out->blocks = st.blocks;
    out->current = st.current;
    out->playing = st.playing;
    out->recorded = st.recorded;
    out->progress = st.progress;
    out->progress_max = st.progress_max;
    snprintf(out->block, sizeof out->block, "%s", st.block);
    snprintf(out->path, sizeof out->path, "%s", st.path);
    out->marker = st.marker;
    out->list_serial = st.list_serial;
}

int pmd85session_fujinet_restart(pmd85session *s)
{
    int rc;
    if (!s) return -1;
    pthread_mutex_lock(&s->lifecycle_mtx);
    if (!s->running || s->fujinet_external || !s->opts.enable_fujinet) {
        session_set_error(s, s->fujinet_external
                          ? "FujiNet is external: restart it where it runs"
                          : "The built-in FujiNet is off (Preferences)");
        pthread_mutex_unlock(&s->lifecycle_mtx);
        return -1;
    }
    if (!fujinet_stop(s)) {
        session_set_error(s, "FujiNet did not stop (a device is blocked); it cannot "
                          "restart until it does");
        pthread_mutex_unlock(&s->lifecycle_mtx);
        return -1;
    }
    rc = fujinet_start(s);
    if (rc == 0)
        fujinet_wait_for_becker(s, 10000);
    pthread_mutex_unlock(&s->lifecycle_mtx);
    return rc;
}

int pmd85session_fujinet_link(const pmd85session *s)
{
    return s && s->running && pmd85host_becker_link_up();
}

int pmd85session_fujinet_read_log(pmd85session *s, pmd85_log_cursor *cur, char *dst, int max,
                                  int *reset)
{
    return fujinet_read_log(s, cur, dst, max, reset);
}

int pmd85session_fujinet_copy_log(pmd85session *s, char *dst, int max)
{
    return fujinet_copy_log(s, dst, max);
}

int pmd85session_rom_loads(const pmd85session *s, int *last_size)
{
    if (!s || !s->running) {
        if (last_size) *last_size = 0;
        return 0;
    }
    return pmd85host_becker_rom_loads(last_size);
}

int pmd85session_parse_hostport(const char *text, char *host, int hostsz,
                                int *port)
{
    char buf[80], *colon, *end;
    const char *hst = "127.0.0.1";
    long num = PMD85SESSION_BECKER_PORT;
    size_t len, pos;

    if (!text) text = "";
    while (*text == ' ' || *text == '\t') text++;
    len = strlen(text);
    while (len && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                   text[len - 1] == '\n' || text[len - 1] == '\r'))
        len--;
    if (len >= sizeof(buf)) return -1;
    memcpy(buf, text, len);
    buf[len] = '\0';

    if (len) {
        colon = strrchr(buf, ':');
        if (colon) {
            *colon = '\0';
            hst = buf;
            if (!*hst) return -1;
            text = colon + 1;
            if (!*text) return -1;
            num = strtol(text, &end, 10);
            if (*end || num < 1 || num > 65535) return -1;
        } else {
            /* a bare number is a port; anything else a host */
            num = strtol(buf, &end, 10);
            if (*end) {
                hst = buf;
                num = PMD85SESSION_BECKER_PORT;
            } else if (num < 1 || num > 65535) {
                return -1;
            }
        }
        if (hst == buf) {
            /* A name or IPv4 literal: letters, digits, '.' and '-' only. */
            for (pos = 0; hst[pos]; pos++) {
                char chr = hst[pos];
                if (!((chr >= '0' && chr <= '9') || (chr >= 'a' && chr <= 'z') ||
                      (chr >= 'A' && chr <= 'Z') || chr == '.' || chr == '-'))
                    return -1;
            }
            if (hst[0] == '.' || hst[0] == '-' || hst[strlen(hst) - 1] == '.')
                return -1;
        }
    }
    if ((int)strlen(hst) >= hostsz) return -1;
    memcpy(host, hst, strlen(hst) + 1);
    *port = (int)num;
    return 0;
}

pmd85debug *pmd85session_debugger(pmd85session *s)
{
    pthread_mutex_lock(&s->lifecycle_mtx);
    if (!s->debugger)
        s->debugger = pmd85debug_create(s);
    pthread_mutex_unlock(&s->lifecycle_mtx);
    return s->debugger;
}

/****************************************************************************/
/** Paths and media import                                                 **/
/****************************************************************************/

const char *pmd85session_roms_path(const pmd85session *s)   { return s->roms_dir; }
const char *pmd85session_config_path(const pmd85session *s) { return s->config_dir; }
const char *pmd85session_data_path(const pmd85session *s)   { return s->data_dir; }
const char *pmd85session_sd_path(const pmd85session *s)     { return s->fujinet_sd; }

static const char *path_basename(const char *path)
{
    const char *a = strrchr(path, '/');
    const char *b = strrchr(path, '\\');
    if (b && (!a || b > a)) a = b;
    return a ? a + 1 : path;
}

static const char *path_ext(const char *path)
{
    const char *base = path_basename(path);
    const char *dot = strrchr(base, '.');
    return dot ? dot + 1 : "";
}

static int ext_is(const char *ext, const char *want)
{
    for (; *ext && *want; ext++, want++)
        if ((*ext | 0x20) != *want) return 0;
    return *ext == '\0' && *want == '\0';
}

int pmd85session_import_media(pmd85session *s, const char *src_path,
                              char *dest_out, int dest_sz)
{
    /* ROM-module images and PMD 32 disk images go to FujiNet's SD folder, to
     * be mounted from FujiNet CONFIG; tapes too, for when FujiNet serves them.
     * A monitor ROM is the machine's own business. */
    static const char *const k_sd_exts[] = { "rmm", "mrm", "p32", "ptp", "pmd", NULL };
    const char *ext = path_ext(src_path);
    const char *base = path_basename(src_path);
    const char *dir = NULL;
    char dst[PMD85_PATH_MAX];
    int i;

    for (i = 0; k_sd_exts[i] && !dir; i++)
        if (ext_is(ext, k_sd_exts[i])) dir = s->fujinet_sd;
    if (!dir && ext_is(ext, "rom"))
        dir = s->roms_dir;

    if (!dir) {
        session_set_error(s, "Unsupported media type \".%s\" (expected a ROM "
                          "module .rmm .mrm, a disk .p32, a tape .ptp .pmd, "
                          "or a monitor ROM .rom)", ext);
        return -1;
    }
    if (!dir[0]) {
        session_set_error(s, "No FujiNet SD directory");
        return -1;
    }

    mkdir_p(dir);
    snprintf(dst, sizeof(dst), "%s/%s", dir, base);
    if (copy_file(src_path, dst) != 0) {
        session_set_error(s, "Could not copy %s to %s", src_path, dst);
        return -1;
    }
    if (dest_out && dest_sz > 0)
        snprintf(dest_out, (size_t)dest_sz, "%s", dst);
    return 0;
}
