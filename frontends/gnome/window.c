/*
 * Pmd85Window: main window of the GNOME frontend. Header bar + menu over the
 * emulator display; keyboard capture routes every key to the PMD 85 except
 * the Alt hotkeys (upstream GPMD85's, and the debugger): Alt+F fullscreen,
 * Alt+C colors, Alt+T tape browser, Alt+P tape play/stop, Alt+F5 RESET,
 * Alt+Shift+F5 cold restart, Alt+F12 debugger. Ctrl (STOP) and Shift are the
 * PMD's own modifiers, so no hotkey uses them.
 *
 * The PMD keeps more of the keyboard than the other family apps: its function
 * keys K0..K11 are F1..F12 (FujiNet CONFIG boots with K10 = F11), so neither
 * F10 (GNOME's menu key), F11 (fullscreen) nor F12 (the family's debugger
 * key) can be the window's. No
 * on-screen input panels are shown unless the user asks for them.
 *
 * Structure from fujinet-go-coco-desktop's window.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include "debugger/dbg_window.h"
#include "pmd85debug.h"

#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "prefs.h"
#include "fujilog.h"
#include "tape.h"

struct _Pmd85Window {
    AdwApplicationWindow parent_instance;

    pmd85session *session;
    Pmd85Display *display;
    AdwToastOverlay *toasts;
    AdwWindowTitle *title;
    GtkMenuButton *menu_button;
    GtkWidget *led[3];
    GtkWidget *disk_led;  /* the PMD 32's light */
    GMenu *disk_menu;     /* Machine > PMD 32 Disks, rebuilt as the drives change */
    char *disk_sig;       /* what disk_menu shows */
    guint status_id;
    int last_rom_loads;
    unsigned pad_event;   /* the last gamepad connect/disconnect toasted */
    guint hot_key;        /* an Alt hotkey that went down: its release stays here */
};

G_DEFINE_FINAL_TYPE(Pmd85Window, pmd85_window, ADW_TYPE_APPLICATION_WINDOW)

/* ---- helpers ------------------------------------------------------------ */

void pmd85_window_toast(Pmd85Window *self, const char *message)
{
    AdwToast *t = adw_toast_new(message);
    adw_toast_set_use_markup(t, FALSE);   /* plain text: "Black & White" */
    adw_toast_set_timeout(t, 4);
    adw_toast_overlay_add_toast(self->toasts, t);
}

void pmd85_window_apply_display(Pmd85Window *self)
{
    pmd85_display_set_aspect_mode(
        self->display,
        (Pmd85AspectMode)pmd85session_get_int(self->session, "aspect_mode",
                                              PMD85_ASPECT_TV_4_3));
    pmd85_display_set_scaling(
        self->display,
        (Pmd85Scaling)pmd85session_get_int(self->session, "scaling",
                                           PMD85_SCALING_SHARP));
}

static void show_start_warnings(Pmd85Window *self)
{
    const char *w = pmd85session_fujinet_warning(self->session);
    if (!pmd85session_is_running(self->session))
        pmd85_window_toast(self, pmd85session_last_error(self->session));
    else if (w && *w)
        pmd85_window_toast(self, w);
}

void pmd85_window_restart_session(Pmd85Window *self)
{
    pmd85session_start_opts opts;
    pmd85session_stop(self->session);
    pmd85session_default_opts(self->session, &opts);
    pmd85session_start(self->session, &opts);
    show_start_warnings(self);
    pmd85_window_sync_menu(self);
}

/* ---- status: the PMD's LEDs, the FujiNet link, ROM downloads ----------- */

static void set_enabled(Pmd85Window *self, const char *name, gboolean on)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), name);
    if (a)
        g_simple_action_set_enabled(G_SIMPLE_ACTION(a), on);
}

static void tape_actions(Pmd85Window *self, const pmd85session_tape_info *t)
{
    set_enabled(self, "tape-play", t->blocks > 0 && !t->playing);
    set_enabled(self, "tape-stop", t->playing);
    set_enabled(self, "tape-rewind", t->blocks > 0);
    set_enabled(self, "tape-prev", pmd85_window_tape_file(self, -1) >= 0);
    set_enabled(self, "tape-next", pmd85_window_tape_file(self, 1) >= 0);
    set_enabled(self, "tape-eject", t->blocks > 0);
    set_enabled(self, "tape-save", t->blocks > 0);
}

/* ---- the PMD 32's disks ----------------------------------------------------- */

/* Machine > PMD 32 Disks: a submenu per drive, titled with its image; and
 * the drive light. Rebuilt only when what it shows changed. */
static void disks_refresh(Pmd85Window *self)
{
    pmd85session_disk_info info;
    GString *sig = g_string_new(NULL);

    pmd85session_disk_get_info(self->session, &info);
    gtk_widget_set_visible(self->disk_led, info.present);
    gtk_widget_set_opacity(self->disk_led, info.activity ? 1.0 : 0.3);
    if (info.activity) {
        g_autofree char *tip = g_strdup_printf("PMD 32: drive %c %s", 'A' + (info.activity - 1) % 4,
                                               info.activity > 4 ? "writing" : "busy");
        gtk_widget_set_tooltip_text(self->disk_led, tip);
    } else {
        gtk_widget_set_tooltip_text(self->disk_led, "PMD 32");
    }

    g_string_append_printf(sig, "%d", info.enabled);
    for (int d = 0; d < 4; d++)
        g_string_append_printf(sig, "|%s|%d", info.path[d], info.wp[d]);
    if (self->disk_sig && !strcmp(self->disk_sig, sig->str)) {
        g_string_free(sig, TRUE);
        return;
    }
    g_free(self->disk_sig);
    self->disk_sig = g_string_free(sig, FALSE);

    g_menu_remove_all(self->disk_menu);
    {   /* the drive itself, on and off (the same setting as Preferences'): a
         * command that says what it does, under a title that says how it is --
         * an unchecked toggle shows no box in GTK's menus */
        GMenu *conn = g_menu_new();
        g_menu_append(conn, info.enabled ? "Disconnect the PMD 32" : "Connect the PMD 32",
                      info.enabled ? "win.pmd32(false)" : "win.pmd32(true)");
        g_menu_append_section(self->disk_menu, info.enabled ? "Connected" : "Not connected",
                              G_MENU_MODEL(conn));
        g_object_unref(conn);
    }
    {
        GMenu *drives = g_menu_new();
        for (int d = 0; d < 4; d++) {
            GMenu *sub = g_menu_new();
            g_autofree char *base = info.path[d][0] ? g_path_get_basename(info.path[d]) : NULL;
            g_autofree char *label = g_strdup_printf("%c: %s", 'A' + d, base ? base : "empty");
            g_autofree char *ins = g_strdup_printf("win.disk-insert(%d)", d);
            g_autofree char *ej = g_strdup_printf("win.disk-eject(%d)", d);
            g_autofree char *wp = g_strdup_printf("win.disk-wp-%d", d);
            g_autofree char *wpname = g_strdup_printf("disk-wp-%d", d);
            GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), wpname);
            g_menu_append(sub, "Insert Disk\xe2\x80\xa6", ins);
            if (base)
                g_menu_append(sub, "Eject", ej);
            g_menu_append(sub, "Write Protected", wp);
            if (a)
                g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_boolean(info.wp[d] != 0));
            g_menu_append_submenu(drives, label, G_MENU_MODEL(sub));
            g_object_unref(sub);
        }
        g_menu_append_section(self->disk_menu, NULL, G_MENU_MODEL(drives));
        g_object_unref(drives);
    }
}

typedef struct {
    Pmd85Window *self;
    int drive;
} DiskPick;

static void disk_insert_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    DiskPick *pick = user_data;
    Pmd85Window *self = pick->self;
    const int d = pick->drive;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    pmd85session_disk_info info;

    g_free(pick);
    if (!path)
        return;
    pmd85session_disk_get_info(self->session, &info);
    if (pmd85session_disk_insert(self->session, d, path, info.wp[d]) != 0) {
        pmd85_window_toast(self, pmd85session_last_error(self->session));
        return;
    }
    {
        g_autofree char *base = g_path_get_basename(path);
        g_autofree char *msg = info.enabled
            ? g_strdup_printf("Drive %c: %s", 'A' + d, base)
            : g_strdup_printf("Drive %c: %s -- the PMD 32 is not connected "
                              "(PMD 32 Disks > Connect the PMD 32)", 'A' + d, base);
        pmd85_window_toast(self, msg);
    }
    disks_refresh(self);
}

static void action_disk_insert(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    const int d = g_variant_get_int32(param);
    GtkFileDialog *dialog = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *disks = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    DiskPick *pick = g_new0(DiskPick, 1);
    pmd85session_disk_info info;
    g_autofree char *title = g_strdup_printf("Insert a Disk into Drive %c", 'A' + d);
    (void)action;

    gtk_file_filter_set_name(disks, "PMD 32 disks (.p32)");
    gtk_file_filter_add_suffix(disks, "p32");
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, disks);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dialog, title);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    pmd85session_disk_get_info(self->session, &info);
    {   /* start where this drive's disk, or another one, came from */
        const char *near = info.path[d][0] ? info.path[d] : NULL;
        for (int k = 0; !near && k < 4; k++)
            if (info.path[k][0]) near = info.path[k];
        if (near) {
            g_autoptr(GFile) f = g_file_new_for_path(near);
            if (info.path[d][0])
                gtk_file_dialog_set_initial_file(dialog, f);
            else {
                g_autoptr(GFile) dir = g_file_get_parent(f);
                if (dir) gtk_file_dialog_set_initial_folder(dialog, dir);
            }
        }
    }
    pick->self = self;
    pick->drive = d;
    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, disk_insert_done, pick);
    g_object_unref(disks);
    g_object_unref(all);
    g_object_unref(filters);
    g_object_unref(dialog);
}

static void action_disk_eject(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    const int d = g_variant_get_int32(param);
    (void)action;
    if (pmd85session_disk_eject(self->session, d) != 0) {
        pmd85_window_toast(self, pmd85session_last_error(self->session));
        return;
    }
    {
        g_autofree char *msg = g_strdup_printf("Drive %c ejected", 'A' + d);
        pmd85_window_toast(self, msg);
    }
    disks_refresh(self);
}

static void action_disk_wp(GSimpleAction *action, GVariant *value, gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    const int d = g_action_get_name(G_ACTION(action))[8] - '0';   /* disk-wp-N */
    if (pmd85session_disk_protect(self->session, d, g_variant_get_boolean(value)) != 0) {
        pmd85_window_toast(self, pmd85session_last_error(self->session));
        return;
    }
    g_simple_action_set_state(action, value);
    disks_refresh(self);
}

/* Connect / Disconnect the PMD 32: a peripheral, so the machine restarts
 * with it (or without it; the disks stay in the settings for next time) */
static void action_pmd32(GSimpleAction *action, GVariant *value, gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    const gboolean on = g_variant_get_boolean(value);
    (void)action;
    pmd85session_set_peripheral(self->session, "pmd32", on);
    pmd85_window_toast(self, on ? "PMD 32 connected: the machine restarted with the disks in"
                                : "PMD 32 disconnected: the machine restarted without it");
    disks_refresh(self);
}

static gboolean status_tick(gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    pmd85session *s = self->session;
    int leds = pmd85session_leds(s);
    int page = pmd85session_mega_page(s);
    int last = 0, loads = pmd85session_rom_loads(s, &last);
    g_autofree char *sub = NULL;
    const char *link;
    int i;

    if (!pmd85session_is_running(s))
        link = "stopped";
    else if (pmd85session_fujinet_link(s))
        link = pmd85session_fujinet_external(s) ? "external FujiNet" : "FujiNet linked";
    else if (pmd85session_fujinet_running(s) || pmd85session_fujinet_external(s))
        link = "FujiNet not linked";
    else
        link = "no FujiNet";

    {
        GString *g = g_string_new(pmd85session_model_name(pmd85session_model(s)));
        pmd85session_tape_info t;
        g_string_append_printf(g, " \xc2\xb7 %s",
                               pmd85session_color_name(pmd85session_color_profile(s)));
        g_string_append_printf(g, " \xc2\xb7 %s", link);
        /* the page only when there are pages: an image over 32 KB */
        if (page >= 0 && pmd85session_mega_size(s) > 32 * 1024)
            g_string_append_printf(g, " \xc2\xb7 MRM #%d", page);
        pmd85session_tape_get_info(s, &t);
        if (t.blocks > 0) {
            const char *blk = strcmp(t.block, "..") ? t.block : "data";
            if (t.playing && t.progress_max > 0)
                g_string_append_printf(g, " \xc2\xb7 Tape \xe2\x96\xb6 %s %d%%", blk,
                                       t.progress * 100 / t.progress_max);
            else
                g_string_append_printf(g, " \xc2\xb7 Tape %s (%d/%d)%s", blk,
                                       t.current + 1, t.blocks,
                                       t.recorded ? ", recorded" : "");
        }
        tape_actions(self, &t);
        sub = g_string_free(g, FALSE);
    }
    adw_window_title_set_subtitle(self->title, sub);

    for (i = 0; i < 3; i++)
        gtk_widget_set_opacity(self->led[i], (leds & (1 << i)) ? 1.0 : 0.18);
    disks_refresh(self);

    {   /* a gamepad plugged in or out */
        char msg[160];
        unsigned serial = pmd85session_gamepad_last_event(s, msg, sizeof msg);
        if (serial != self->pad_event) {
            self->pad_event = serial;
            if (msg[0])
                pmd85_window_toast(self, msg);
        }
    }

    if (loads != self->last_rom_loads) {
        g_autofree char *msg = NULL;
        self->last_rom_loads = loads;
        msg = g_strdup_printf("ROM module loaded from FujiNet (%d KB)",
                              (last + 1023) / 1024);
        pmd85_window_toast(self, msg);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard capture ---------------------------------------------------
 * Both press and release are forwarded: the machine's keyboard matrix tracks
 * held keys, and a missed release leaves that key down inside the PMD. */

/* An Alt hotkey (k lowercased); FALSE when k is none. The PMD's matrix
 * ignores keys while Alt is held (upstream's hotkey modifier), so these
 * take nothing from it. */
static gboolean hotkey(Pmd85Window *self, guint k, GdkModifierType state)
{
    GActionGroup *g = G_ACTION_GROUP(self);
    switch (k) {
    case GDK_KEY_f:
        g_action_group_activate_action(g, "fullscreen", NULL);
        return TRUE;
    case GDK_KEY_c: {
        const int next = (pmd85session_color_profile(self->session) + 1) % PMD85SESSION_COLOR_COUNT;
        g_action_group_activate_action(g, "color", g_variant_new_int32(next));
        return TRUE;   /* the subtitle shows it */
    }
    case GDK_KEY_t:
        g_action_group_activate_action(g, "tape-browser", NULL);
        return TRUE;
    case GDK_KEY_p: {
        pmd85session_tape_info t;
        pmd85session_tape_get_info(self->session, &t);
        g_action_group_activate_action(g, t.playing ? "tape-stop" : "tape-play", NULL);
        return TRUE;
    }
    case GDK_KEY_F5:
        g_action_group_activate_action(g, (state & GDK_SHIFT_MASK) ? "cold" : "reset", NULL);
        return TRUE;
    case GDK_KEY_F12:
        pmd85_debugger_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    }
    return FALSE;
}

static gboolean window_key(Pmd85Window *self, guint keyval,
                           GdkModifierType state, gboolean down)
{
    const guint k = gdk_keyval_to_lower(keyval);

    if (self->hot_key && k == self->hot_key) {
        /* its auto-repeat and its release */
        if (!down)
            self->hot_key = 0;
        return TRUE;
    }
    if (down && (state & GDK_ALT_MASK) && hotkey(self, k, state)) {
        self->hot_key = k;
        return TRUE;
    }

    pmd85session_key(self->session, down ? 1 : 0, keyval,
                     gdk_keyval_to_unicode(keyval),
                     (state & GDK_CONTROL_MASK) != 0,
                     (state & GDK_SHIFT_MASK) != 0);
    return TRUE;
}

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data)
{
    (void)controller;
    (void)keycode;
    return window_key(PMD85_WINDOW(user_data), keyval, state, TRUE);
}

static void on_key_released(GtkEventControllerKey *controller, guint keyval,
                            guint keycode, GdkModifierType state,
                            gpointer user_data)
{
    (void)controller;
    (void)keycode;
    window_key(PMD85_WINDOW(user_data), keyval, state, FALSE);
}

/* Focus leaving the window with keys held would leave the machine believing
 * they are still down. */
static void on_focus_leave(GtkEventControllerFocus *controller,
                           gpointer user_data)
{
    (void)controller;
    pmd85session_release_all(PMD85_WINDOW(user_data)->session);
}

static void on_active_changed(GObject *object, GParamSpec *pspec,
                              gpointer user_data)
{
    (void)pspec;
    if (!gtk_window_is_active(GTK_WINDOW(object)))
        pmd85session_release_all(PMD85_WINDOW(user_data)->session);
}

/* ---- actions ------------------------------------------------------------ */

static void set_radio_state(Pmd85Window *self, const char *name, int value)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), name);
    if (a)
        g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_int32(value));
}

void pmd85_window_sync_menu(Pmd85Window *self)
{
    set_radio_state(self, "model", pmd85session_model(self->session));
    set_radio_state(self, "module", pmd85session_module(self->session));
    set_radio_state(self, "color", pmd85session_color_profile(self->session));
}

static void action_reset(GSimpleAction *action, GVariant *param,
                         gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    (void)action;
    (void)param;
    pmd85session_reset(self->session);
    pmd85_window_toast(self, "RESET");
}

static void action_debugger(GSimpleAction *action, GVariant *param,
                            gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    (void)action;
    (void)param;
    pmd85_debugger_show(GTK_WINDOW(self), self->session);
}

static void action_cold(GSimpleAction *action, GVariant *param,
                        gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    (void)action;
    (void)param;
    pmd85session_hard_reset(self->session);
    pmd85_window_toast(self, "Cold restart");
}

static void action_model(GSimpleAction *action, GVariant *param,
                         gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    g_autofree char *msg = NULL;
    int model = g_variant_get_int32(param);

    pmd85session_set_model(self->session, model);
    g_simple_action_set_state(action, g_variant_new_int32(
                                  pmd85session_model(self->session)));
    if (!pmd85session_module_supported(model, pmd85session_module(self->session)))
        msg = g_strdup_printf("%s (no %s module on this model)",
                              pmd85session_model_name(model),
                              pmd85session_module_name(pmd85session_module(self->session)));
    else if (pmd85session_module_hint(self->session, model,
                                      pmd85session_module(self->session)))
        msg = g_strdup_printf("%s. %s", pmd85session_model_name(model),
                              pmd85session_module_hint(self->session, model,
                                  pmd85session_module(self->session)));
    else
        msg = g_strdup_printf("%s", pmd85session_model_name(model));
    pmd85_window_toast(self, msg);
}

/* ---- a ROM module file of the user's ------------------------------------ */

typedef struct {
    Pmd85Window *self;
    Pmd85ModuleFileDone done;
    gpointer data;
    GDestroyNotify destroy;
} ModuleFileJob;

static void module_file_done(GObject *source, GAsyncResult *result,
                             gpointer user_data)
{
    ModuleFileJob *job = user_data;
    Pmd85Window *self = job->self;
    g_autoptr(GFile) file =
        gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    gboolean chosen = FALSE;

    if (path) {
        if (pmd85session_set_module_file(self->session, path) == 0) {
            g_autofree char *base = g_path_get_basename(path);
            g_autofree char *msg = g_strdup_printf("ROM module: %s", base);
            pmd85_window_toast(self, msg);
            chosen = TRUE;
        } else {
            pmd85_window_toast(self, pmd85session_last_error(self->session));
        }
    }
    pmd85_window_sync_menu(self);
    if (job->done)
        job->done(job->data, chosen);
    if (job->destroy)
        job->destroy(job->data);
    g_object_unref(self);
    g_free(job);
}

void pmd85_window_choose_module_file(Pmd85Window *self, Pmd85ModuleFileDone done,
                                     gpointer data, GDestroyNotify destroy)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *roms = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    ModuleFileJob *job = g_new0(ModuleFileJob, 1);
    const char *cur = pmd85session_module_file(self->session);

    job->self = g_object_ref(self);
    job->done = done;
    job->data = data;
    job->destroy = destroy;

    gtk_file_filter_set_name(roms, "ROM modules (.rmm .mrm)");
    gtk_file_filter_add_suffix(roms, "rmm");
    gtk_file_filter_add_suffix(roms, "mrm");
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, roms);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dialog, "Choose ROM Module File");
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    if (*cur) {
        g_autoptr(GFile) f = g_file_new_for_path(cur);
        gtk_file_dialog_set_initial_file(dialog, f);
    }
    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, module_file_done, job);
    g_object_unref(roms);
    g_object_unref(all);
    g_object_unref(filters);
    g_object_unref(dialog);
}

static void action_module_file(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    (void)action;
    (void)param;
    pmd85_window_choose_module_file(PMD85_WINDOW(user_data), NULL, NULL, NULL);
}

static void action_module(GSimpleAction *action, GVariant *param,
                          gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    int module = g_variant_get_int32(param);

    /* "Module file" with nothing (left) to load asks for one first */
    if (module == PMD85SESSION_MODULE_FILE &&
        !pmd85session_module_file_ok(self->session)) {
        pmd85_window_choose_module_file(self, NULL, NULL, NULL);
        return;
    }
    pmd85session_set_module(self->session, module);
    g_simple_action_set_state(action, g_variant_new_int32(
                                  pmd85session_module(self->session)));
}

static void action_color(GSimpleAction *action, GVariant *param,
                         gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    pmd85session_set_color_profile(self->session, g_variant_get_int32(param));
    g_simple_action_set_state(action, g_variant_new_int32(
                                  pmd85session_color_profile(self->session)));
}

static void action_fullscreen(GSimpleAction *action, GVariant *param,
                              gpointer user_data)
{
    GtkWindow *w = GTK_WINDOW(user_data);
    (void)action;
    (void)param;
    if (gtk_window_is_fullscreen(w))
        gtk_window_unfullscreen(w);
    else
        gtk_window_fullscreen(w);
}

/* ---- the tape deck ------------------------------------------------------- */

static void tape_open_finish(Pmd85Window *self, GObject *source, GAsyncResult *result,
                             gboolean append)
{
    g_autoptr(GFile) file =
        gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    g_autofree char *base = NULL, *msg = NULL;
    pmd85session_tape_info t;
    int before, rc;

    if (!path)
        return;
    pmd85session_tape_get_info(self->session, &t);
    before = t.blocks;
    rc = append ? pmd85session_tape_append(self->session, path)
                : pmd85session_tape_load(self->session, path);
    if (rc < 0) {
        pmd85_window_toast(self, pmd85session_last_error(self->session));
        return;
    }
    pmd85session_tape_get_info(self->session, &t);
    base = g_path_get_basename(path);
    if (rc > 0)
        msg = g_strdup_printf("%s: %s", base, pmd85session_last_error(self->session));
    else if (append && before > 0)
        msg = g_strdup_printf("%s: %d file%s added to the tape", base, t.blocks - before,
                              t.blocks - before == 1 ? "" : "s");
    else
        msg = g_strdup_printf("Tape: %s, %d file%s. Load it on the PMD (e.g. MGLD nn), "
                              "then Play", base, t.blocks, t.blocks == 1 ? "" : "s");
    pmd85_window_toast(self, msg);
}

static void tape_load_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    tape_open_finish(PMD85_WINDOW(user_data), source, result, FALSE);
}

static void tape_append_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    tape_open_finish(PMD85_WINDOW(user_data), source, result, TRUE);
}

static GtkFileDialog *tape_dialog(Pmd85Window *self, const char *title)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *tapes = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    pmd85session_tape_info t;

    gtk_file_filter_set_name(tapes, "PMD 85 tapes (.ptp .pmd)");
    gtk_file_filter_add_suffix(tapes, "ptp");
    gtk_file_filter_add_suffix(tapes, "pmd");
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, tapes);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dialog, title);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    pmd85session_tape_get_info(self->session, &t);
    if (t.path[0]) {
        g_autoptr(GFile) f = g_file_new_for_path(t.path);
        gtk_file_dialog_set_initial_file(dialog, f);
    }
    g_object_unref(tapes);
    g_object_unref(all);
    g_object_unref(filters);
    return dialog;
}

void pmd85_window_tape_open(Pmd85Window *self, GtkWindow *parent, gboolean append)
{
    GtkFileDialog *dialog = tape_dialog(self, append ? "Append Tape" : "Load Tape");
    gtk_file_dialog_open(dialog, parent ? parent : GTK_WINDOW(self), NULL,
                         append ? tape_append_done : tape_load_done, self);
    g_object_unref(dialog);
}

static void action_tape_load(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action;
    (void)param;
    pmd85_window_tape_open(PMD85_WINDOW(user_data), NULL, FALSE);
}

static void tape_save_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    g_autoptr(GFile) file =
        gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, NULL);
    g_autofree char *path = file ? g_file_get_path(file) : NULL;
    g_autofree char *base = NULL, *msg = NULL;

    if (!path)
        return;
    {   /* a tape without an extension gets .ptp, the format it is written in */
        g_autofree char *lower = g_ascii_strdown(path, -1);
        if (!g_str_has_suffix(lower, ".ptp") && !g_str_has_suffix(lower, ".pmd")) {
            char *with = g_strconcat(path, ".ptp", NULL);
            g_free(path);
            path = with;
        }
    }
    if (pmd85session_tape_save(self->session, path) != 0) {
        pmd85_window_toast(self, pmd85session_last_error(self->session));
        return;
    }
    base = g_path_get_basename(path);
    msg = g_strdup_printf("Tape saved as %s", base);
    pmd85_window_toast(self, msg);
}

void pmd85_window_tape_save(Pmd85Window *self, GtkWindow *parent)
{
    GtkFileDialog *dialog = tape_dialog(self, "Save Tape As");
    pmd85session_tape_info t;
    pmd85session_tape_get_info(self->session, &t);
    if (!t.path[0])
        gtk_file_dialog_set_initial_name(dialog, "tape.ptp");
    gtk_file_dialog_save(dialog, parent ? parent : GTK_WINDOW(self), NULL,
                         tape_save_done, self);
    g_object_unref(dialog);
}

static void action_tape_save(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action;
    (void)param;
    pmd85_window_tape_save(PMD85_WINDOW(user_data), NULL);
}

static void tape_result(Pmd85Window *self, int rc, const char *done)
{
    if (rc != 0)
        pmd85_window_toast(self, pmd85session_last_error(self->session));
    else if (done)
        pmd85_window_toast(self, done);
}

static void action_tape_play(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    (void)action;
    (void)param;
    tape_result(self, pmd85session_tape_play(self->session), NULL);
}

static void action_tape_stop(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    (void)action;
    (void)param;
    tape_result(self, pmd85session_tape_stop(self->session), NULL);
}

static void action_tape_rewind(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    (void)action;
    (void)param;
    tape_result(self, pmd85session_tape_rewind(self->session), "Tape rewound");
}

/* The previous or next file on the tape (a header block; the data blocks
 * a program loads by itself are passed over), -1 none. */
int pmd85_window_tape_file(Pmd85Window *self, int delta)
{
    pmd85session_tape_info t;
    pmd85session_tape_block *b;
    int n, to = -1;
    pmd85session_tape_get_info(self->session, &t);
    if (t.blocks == 0)
        return -1;
    b = g_new0(pmd85session_tape_block, t.blocks);
    n = pmd85session_tape_blocks(self->session, b, t.blocks, NULL);
    if (n > t.blocks) n = t.blocks;
    for (int i = t.current + delta; i >= 0 && i < n; i += delta)
        if (b[i].number >= 0) {
            to = i;
            break;
        }
    g_free(b);
    return to;
}

/* the previous or next file as the tape's position */
int pmd85_window_tape_step(Pmd85Window *self, int delta)
{
    pmd85session_tape_info t;
    const int to = pmd85_window_tape_file(self, delta);
    if (to < 0)
        return -1;
    pmd85session_tape_get_info(self->session, &t);
    if (t.playing)
        pmd85session_tape_stop(self->session);
    return pmd85session_tape_seek(self->session, to);
}

static void tape_step(Pmd85Window *self, int delta)
{
    pmd85session_tape_info t;
    if (pmd85_window_tape_step(self, delta) != 0)
        return;
    pmd85session_tape_get_info(self->session, &t);
    {
        g_autofree char *msg = g_strdup_printf("Tape at %s (%d of %d)", t.block,
                                               t.current + 1, t.blocks);
        pmd85_window_toast(self, msg);
    }
}

static void action_tape_prev(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action;
    (void)param;
    tape_step(PMD85_WINDOW(user_data), -1);
}

static void action_tape_next(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action;
    (void)param;
    tape_step(PMD85_WINDOW(user_data), 1);
}

static void action_tape_append(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action;
    (void)param;
    pmd85_window_tape_open(PMD85_WINDOW(user_data), NULL, TRUE);
}

static void action_tape_browser(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    (void)action;
    (void)param;
    pmd85_tape_window_show(self, self->session);
}

static void tape_eject_response(AdwAlertDialog *dialog, const char *response,
                                gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    if (!strcmp(response, "save")) {
        GtkWidget *parent = g_object_get_data(G_OBJECT(dialog), "pmd85-parent");
        pmd85_window_tape_save(self, parent ? GTK_WINDOW(parent) : NULL);
    }
    else if (!strcmp(response, "eject"))
        tape_result(self, pmd85session_tape_eject(self->session), "Tape ejected");
}

void pmd85_window_tape_eject(Pmd85Window *self, GtkWidget *parent)
{
    pmd85session_tape_info t;
    pmd85session_tape_get_info(self->session, &t);
    if (t.recorded) {
        /* what the PMD saved to tape is only in the deck so far */
        AdwDialog *d = adw_alert_dialog_new("Eject the Tape?",
            "The tape has recordings or changes that are not saved to a file.");
        adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(d), "cancel", "_Cancel",
                                       "save", "_Save As\xe2\x80\xa6",
                                       "eject", "_Eject", NULL);
        adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(d), "eject",
                                                 ADW_RESPONSE_DESTRUCTIVE);
        adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(d), "cancel");
        adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(d), "cancel");
        g_signal_connect(d, "response", G_CALLBACK(tape_eject_response), self);
        g_object_set_data(G_OBJECT(d), "pmd85-parent", parent);
        adw_dialog_present(d, parent ? parent : GTK_WIDGET(self));
        return;
    }
    tape_result(self, pmd85session_tape_eject(self->session), "Tape ejected");
}

static void action_tape_eject(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action;
    (void)param;
    pmd85_window_tape_eject(PMD85_WINDOW(user_data), NULL);
}

/* ---- FujiNet's SD card ---------------------------------------------------- */

static void import_sd_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    g_autoptr(GListModel) files =
        gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), result, NULL);
    g_autofree char *msg = NULL;
    char dest[1024], last[256] = "";
    int ok = 0, failed = 0;

    if (!files)
        return;
    for (guint i = 0; i < g_list_model_get_n_items(files); i++) {
        g_autoptr(GFile) f = g_list_model_get_item(files, i);
        g_autofree char *path = g_file_get_path(f);
        if (path && pmd85session_import_media(self->session, path, dest, sizeof dest) == 0) {
            const char *base = strrchr(dest, '/') ? strrchr(dest, '/') + 1 : dest;
            g_strlcpy(last, base, sizeof last);
            ok++;
        } else {
            failed++;
        }
    }
    if (failed && !ok)
        msg = g_strdup(pmd85session_last_error(self->session));
    else if (failed)
        msg = g_strdup_printf("%d copied to FujiNet's SD card, %d could not be", ok, failed);
    else if (ok == 1)
        msg = g_strdup_printf("Copied %s to FujiNet's SD card: mount it from CONFIG", last);
    else
        msg = g_strdup_printf("%d copied to FujiNet's SD card: mount them from CONFIG", ok);
    pmd85_window_toast(self, msg);
}

static void action_import_sd(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    GtkFileDialog *dialog = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *media = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    static const char *const exts[] = { "rmm", "mrm", NULL };   /* all FujiNet serves a PMD */
    (void)action;
    (void)param;

    gtk_file_filter_set_name(media, "ROM modules (.rmm .mrm)");
    for (int i = 0; exts[i]; i++)
        gtk_file_filter_add_suffix(media, exts[i]);
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, media);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dialog, "Import to FujiNet's SD Card");
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(self), NULL, import_sd_done, self);
    g_object_unref(media);
    g_object_unref(all);
    g_object_unref(filters);
    g_object_unref(dialog);
}

static void action_sd_folder(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    const char *path = pmd85session_sd_path(self->session);
    g_autoptr(GFile) dir = NULL;
    GtkFileLauncher *l;
    (void)action;
    (void)param;
    if (!path || !*path) {
        pmd85_window_toast(self, "No FujiNet SD folder");
        return;
    }
    g_mkdir_with_parents(path, 0755);
    dir = g_file_new_for_path(path);
    l = gtk_file_launcher_new(dir);
    gtk_file_launcher_launch(l, GTK_WINDOW(self), NULL, NULL, NULL);
    g_object_unref(l);
}

/* The restart waits for FujiNet's listener (a second or so): off the UI
 * thread. */
static void fujinet_restart_thread(GTask *task, gpointer source, gpointer data,
                                   GCancellable *cancel)
{
    Pmd85Window *self = PMD85_WINDOW(source);
    (void)data;
    (void)cancel;
    g_task_return_int(task, pmd85session_fujinet_restart(self->session));
}

static void fujinet_restart_done(GObject *source, GAsyncResult *result, gpointer data)
{
    Pmd85Window *self = PMD85_WINDOW(source);
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), "fujinet-restart");
    (void)data;
    if (a)
        g_simple_action_set_enabled(G_SIMPLE_ACTION(a), TRUE);
    if (g_task_propagate_int(G_TASK(result), NULL) == 0)
        pmd85_window_toast(self, "FujiNet restarted");
    else
        pmd85_window_toast(self, pmd85session_last_error(self->session));
}

static void action_fujinet_restart(GSimpleAction *action, GVariant *param,
                                   gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    GTask *task;
    (void)param;
    if (pmd85session_fujinet_external(self->session)) {
        pmd85_window_toast(self, "FujiNet is external: restart it where it runs");
        return;
    }
    if (!pmd85session_get_int(self->session, "enable_fujinet", 1)) {
        pmd85_window_toast(self, "The built-in FujiNet is off (Preferences)");
        return;
    }
    g_simple_action_set_enabled(action, FALSE);
    pmd85_window_toast(self, "Restarting FujiNet\xe2\x80\xa6");
    task = g_task_new(self, NULL, fujinet_restart_done, NULL);
    g_task_run_in_thread(task, fujinet_restart_thread);
    g_object_unref(task);
}

static void action_fujinet_config(GSimpleAction *action, GVariant *param,
                                  gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    (void)action;
    (void)param;
    if (pmd85session_fujinet_external(self->session)) {
        pmd85_window_toast(self, "FujiNet is external: use its own web UI");
        return;
    }
    if (!pmd85session_fujinet_running(self->session)) {
        pmd85_window_toast(self, "The built-in FujiNet is not running");
        return;
    }
    pmd85_fujinet_config_show(GTK_WINDOW(self), self->session);
}

static void action_fujinet_log(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    (void)action;
    (void)param;
    if (pmd85session_fujinet_external(self->session)) {
        pmd85_window_toast(self, "FujiNet is external: see its own console");
        return;
    }
    pmd85_fujinet_log_show(GTK_WINDOW(self), self->session);
}

static void action_preferences(GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    (void)action;
    (void)param;
    pmd85_prefs_show(self, self->session);
}

static void action_about(GSimpleAction *action, GVariant *param,
                         gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    (void)action;
    (void)param;
    adw_show_about_dialog(
        GTK_WIDGET(self),
        "application-name", "FujiNet Go PMD 85",
        "application-icon", pmd85_icon_name(),
        "developer-name", "FujiNet Go contributors",
        "version", PMD85_VERSION_STRING,
        "license-type", GTK_LICENSE_GPL_3_0,
        "comments", "Self-contained Tesla PMD 85 with built-in FujiNet.\n"
                    "Emulation by GPMD85Emulator (Roman and Martin Bor\xc3\xadk).",
        "website", "https://fujinet.online/",
        NULL);
}

/* ---- construction ------------------------------------------------------- */

static GMenu *radio_menu(const char *action, const char *(*name_at)(int))
{
    GMenu *m = g_menu_new();
    const char *label;
    int i;

    for (i = 0; (label = name_at(i)) != NULL; i++) {
        g_autofree char *detail = g_strdup_printf("%s(%d)", action, i);
        g_menu_append(m, label, detail);
    }
    return m;
}

static GMenu *build_menu(GMenu *disks)
{
    GMenu *menu = g_menu_new();
    GMenu *machine = g_menu_new();
    GMenu *view = g_menu_new();
    GMenu *fujinet = g_menu_new();
    GMenu *tail = g_menu_new();
    GMenu *sub;

    sub = radio_menu("win.model", pmd85session_model_name);
    g_menu_append_submenu(machine, "Model", G_MENU_MODEL(sub));
    g_object_unref(sub);
    sub = radio_menu("win.module", pmd85session_module_name);
    {
        GMenu *pick = g_menu_new();
        g_menu_append(pick, "Choose Module File\xe2\x80\xa6", "win.module-file");
        g_menu_append_section(sub, NULL, G_MENU_MODEL(pick));
        g_object_unref(pick);
    }
    g_menu_append_submenu(machine, "ROM Module", G_MENU_MODEL(sub));
    g_object_unref(sub);
    {
        GMenu *tape = g_menu_new(), *deck = g_menu_new(), *file = g_menu_new();
        g_menu_append(file, "Tape Browser (Alt+T)", "win.tape-browser");
        g_menu_append(file, "Load Tape\xe2\x80\xa6", "win.tape-load");
        g_menu_append(file, "Save Tape As\xe2\x80\xa6", "win.tape-save");
        g_menu_append_section(tape, NULL, G_MENU_MODEL(file));
        g_menu_append(deck, "Play (Alt+P)", "win.tape-play");
        g_menu_append(deck, "Stop (Alt+P)", "win.tape-stop");
        g_menu_append(deck, "Previous File", "win.tape-prev");
        g_menu_append(deck, "Next File", "win.tape-next");
        g_menu_append(deck, "Rewind", "win.tape-rewind");
        g_menu_append(deck, "Eject", "win.tape-eject");
        g_menu_append_section(tape, NULL, G_MENU_MODEL(deck));
        g_menu_append_submenu(machine, "Tape", G_MENU_MODEL(tape));
        g_menu_append_submenu(machine, "PMD 32 Disks", G_MENU_MODEL(disks));
        g_object_unref(file);
        g_object_unref(deck);
        g_object_unref(tape);
    }
    g_menu_append(machine, "RESET (Alt+F5)", "win.reset");
    g_menu_append(machine, "Cold Restart (Alt+Shift+F5)", "win.cold");
    g_menu_append_section(menu, "Machine", G_MENU_MODEL(machine));

    sub = radio_menu("win.color", pmd85session_color_name);
    g_menu_append_submenu(view, "Colors (Alt+C)", G_MENU_MODEL(sub));
    g_object_unref(sub);
    g_menu_append(view, "Fullscreen (Alt+F)", "win.fullscreen");
    g_menu_append_section(menu, "View", G_MENU_MODEL(view));

    g_menu_append(fujinet, "FujiNet Web UI", "win.fujinet-config");
    g_menu_append(fujinet, "Console Log", "win.fujinet-log");
    g_menu_append(fujinet, "Restart FujiNet", "win.fujinet-restart");
    g_menu_append(fujinet, "Import to SD Card\xe2\x80\xa6", "win.import-sd");
    g_menu_append(fujinet, "Open SD Folder", "win.sd-folder");
    g_menu_append_section(menu, "FujiNet", G_MENU_MODEL(fujinet));

    g_menu_append(tail, "Debugger (Alt+F12)", "win.debugger");
    g_menu_append(tail, "Preferences\xe2\x80\xa6", "win.preferences");
    g_menu_append(tail, "About FujiNet Go PMD 85", "win.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(tail));

    g_object_unref(machine);
    g_object_unref(view);
    g_object_unref(fujinet);
    g_object_unref(tail);
    return menu;
}

static const GActionEntry win_actions[] = {
    {.name = "reset", .activate = action_reset},
    {.name = "cold", .activate = action_cold},
    {.name = "debugger", .activate = action_debugger},
    {.name = "fullscreen", .activate = action_fullscreen},
    {.name = "import-sd", .activate = action_import_sd},
    {.name = "disk-insert", .activate = action_disk_insert, .parameter_type = "i"},
    {.name = "disk-eject", .activate = action_disk_eject, .parameter_type = "i"},
    {.name = "pmd32", .activate = action_pmd32, .parameter_type = "b"},
    {.name = "sd-folder", .activate = action_sd_folder},
    {.name = "fujinet-restart", .activate = action_fujinet_restart},
    {.name = "module-file", .activate = action_module_file},
    {.name = "tape-load", .activate = action_tape_load},
    {.name = "tape-play", .activate = action_tape_play},
    {.name = "tape-stop", .activate = action_tape_stop},
    {.name = "tape-rewind", .activate = action_tape_rewind},
    {.name = "tape-eject", .activate = action_tape_eject},
    {.name = "tape-save", .activate = action_tape_save},
    {.name = "tape-append", .activate = action_tape_append},
    {.name = "tape-prev", .activate = action_tape_prev},
    {.name = "tape-next", .activate = action_tape_next},
    {.name = "tape-browser", .activate = action_tape_browser},
    {.name = "fujinet-config", .activate = action_fujinet_config},
    {.name = "fujinet-log", .activate = action_fujinet_log},
    {.name = "preferences", .activate = action_preferences},
    {.name = "about", .activate = action_about},
};

static void add_radio_action(Pmd85Window *self, const char *name, int value,
                             GCallback change_state)
{
    GSimpleAction *a = g_simple_action_new_stateful(
        name, G_VARIANT_TYPE_INT32, g_variant_new_int32(value));
    g_signal_connect(a, "change-state", change_state, self);
    g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(a));
    g_object_unref(a);
}

/* The PMD 85's three keyboard LEDs, as in GPMD85Emulator's status bar:
 * yellow, red, blue. */
static GtkWidget *led_box(Pmd85Window *self)
{
    static const char *const colors[3] = { "#e8c832", "#e04040", "#4080f0" };
    static const char *const names[3] = { "yellow", "red", "blue" };
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkCssProvider *css = gtk_css_provider_new();
    char buf[512];
    int i;

    g_snprintf(buf, sizeof buf,
               ".pmd85-led { min-width: 10px; min-height: 10px; border-radius: 5px; }\n"
               ".pmd85-led-yellow { background: %s; }\n"
               ".pmd85-led-red { background: %s; }\n"
               ".pmd85-led-blue { background: %s; }\n",
               colors[0], colors[1], colors[2]);
    gtk_css_provider_load_from_string(css, buf);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    for (i = 0; i < 3; i++) {
        char cls[32];
        self->led[i] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(self->led[i], "pmd85-led");
        g_snprintf(cls, sizeof cls, "pmd85-led-%s", names[i]);
        gtk_widget_add_css_class(self->led[i], cls);
        gtk_widget_set_valign(self->led[i], GTK_ALIGN_CENTER);
        gtk_widget_set_opacity(self->led[i], 0.18);
        gtk_box_append(GTK_BOX(box), self->led[i]);
    }
    gtk_widget_set_tooltip_text(box, "The PMD 85 keyboard LEDs");
    return box;
}

/* PMD85_OPEN_MENU=<submenu label>: the menu popped up (development) */
static gboolean open_menu_later(gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    const char *sub = getenv("PMD85_OPEN_MENU");
    GtkPopover *pop;
    disks_refresh(self);
    gtk_menu_button_popup(self->menu_button);
    pop = gtk_menu_button_get_popover(self->menu_button);
    if (pop && sub && *sub && GTK_IS_POPOVER_MENU(pop))
        g_object_set(pop, "visible-submenu", sub, NULL);
    return G_SOURCE_REMOVE;
}

static void pmd85_window_dispose(GObject *object)
{
    Pmd85Window *self = PMD85_WINDOW(object);
    if (self->status_id) {
        g_source_remove(self->status_id);
        self->status_id = 0;
    }
    g_clear_object(&self->disk_menu);
    g_clear_pointer(&self->disk_sig, g_free);
    G_OBJECT_CLASS(pmd85_window_parent_class)->dispose(object);
}

static void pmd85_window_class_init(Pmd85WindowClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = pmd85_window_dispose;
}

static void pmd85_window_init(Pmd85Window *self)
{
    (void)self;
}

GtkWidget *pmd85_window_new(AdwApplication *app, pmd85session *session)
{
    Pmd85Window *self = g_object_new(PMD85_TYPE_WINDOW,
                                     "application", app,
                                     "title", "FujiNet Go PMD 85",
                                     "default-width", 960,
                                     "default-height", 780,
                                     NULL);
    AdwToolbarView *view;
    AdwHeaderBar *header;
    GtkEventController *keys, *focus;
    GMenu *menu;

    self->session = session;

    g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions,
                                    G_N_ELEMENTS(win_actions), self);
    add_radio_action(self, "model", pmd85session_model(session),
                     G_CALLBACK(action_model));
    add_radio_action(self, "module", pmd85session_module(session),
                     G_CALLBACK(action_module));
    add_radio_action(self, "color", pmd85session_color_profile(session),
                     G_CALLBACK(action_color));

    header = ADW_HEADER_BAR(adw_header_bar_new());
    self->title = ADW_WINDOW_TITLE(adw_window_title_new("FujiNet Go PMD 85", ""));
    adw_header_bar_set_title_widget(header, GTK_WIDGET(self->title));
    self->menu_button = GTK_MENU_BUTTON(gtk_menu_button_new());
    gtk_menu_button_set_icon_name(self->menu_button, "open-menu-symbolic");
    self->disk_menu = g_menu_new();
    {   /* each drive's write protection: a toggle in its submenu */
        for (int d = 0; d < 4; d++) {
            g_autofree char *name = g_strdup_printf("disk-wp-%d", d);
            GSimpleAction *a = g_simple_action_new_stateful(name, NULL, g_variant_new_boolean(FALSE));
            g_signal_connect(a, "change-state", G_CALLBACK(action_disk_wp), self);
            g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(a));
            g_object_unref(a);
        }
    }
    menu = build_menu(self->disk_menu);
    gtk_menu_button_set_menu_model(self->menu_button, G_MENU_MODEL(menu));
    g_object_unref(menu);
    adw_header_bar_pack_end(header, GTK_WIDGET(self->menu_button));
    adw_header_bar_pack_start(header, led_box(self));
    self->disk_led = gtk_image_new_from_icon_name("media-floppy-symbolic");
    gtk_widget_set_visible(self->disk_led, FALSE);
    adw_header_bar_pack_start(header, self->disk_led);

    self->display = PMD85_DISPLAY(pmd85_display_new(session));
    self->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(self->toasts, GTK_WIDGET(self->display));

    view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(view, GTK_WIDGET(header));
    adw_toolbar_view_set_content(view, GTK_WIDGET(self->toasts));
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self),
                                       GTK_WIDGET(view));

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), focus);
    g_signal_connect(self, "notify::is-active", G_CALLBACK(on_active_changed),
                     self);

    self->last_rom_loads = pmd85session_rom_loads(session, NULL);
    self->status_id = g_timeout_add(250, status_tick, self);
    status_tick(self);

    pmd85_window_apply_display(self);
    show_start_warnings(self);
    gtk_widget_grab_focus(GTK_WIDGET(self->display));

    /* Developer affordances: open a window alongside the main one. */
    if (getenv("PMD85_OPEN_SETTINGS"))
        pmd85_prefs_show(self, session);
    if (getenv("PMD85_OPEN_DEBUGGER")) {
        pmd85_debugger_show(GTK_WINDOW(self), session);
        /* "run": open it, but leave the machine running */
        if (!strcmp(getenv("PMD85_OPEN_DEBUGGER"), "run"))
            pmd85debug_resume(pmd85session_debugger(session));
    }
    if (getenv("PMD85_OPEN_MENU"))   /* the main menu, at that submenu ("" = top) */
        g_timeout_add_seconds(2, open_menu_later, self);
    return GTK_WIDGET(self);
}
