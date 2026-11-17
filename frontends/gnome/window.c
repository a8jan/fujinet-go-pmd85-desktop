/*
 * Pmd85Window: main window of the GNOME frontend. Header bar + menu over the
 * emulator display; keyboard capture routes every key to the PMD 85 except
 * F12 (the debugger) and Ctrl+Shift+F (fullscreen).
 *
 * The PMD keeps more of the keyboard than the other family apps: its function
 * keys K0..K10 are F1..F11 (FujiNet CONFIG boots with K10 = F11), so neither
 * F10 (GNOME's menu key) nor F11 (fullscreen) can be the window's. No
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
#include "webview.h"

struct _Pmd85Window {
    AdwApplicationWindow parent_instance;

    pmd85session *session;
    Pmd85Display *display;
    AdwToastOverlay *toasts;
    AdwWindowTitle *title;
    GtkMenuButton *menu_button;
    GtkWidget *led[3];
    guint status_id;
    int last_rom_loads;
    gboolean ctlf_down;   /* Ctrl+Shift+F went down: swallow its release */
};

G_DEFINE_FINAL_TYPE(Pmd85Window, pmd85_window, ADW_TYPE_APPLICATION_WINDOW)

/* ---- helpers ------------------------------------------------------------ */

void pmd85_window_toast(Pmd85Window *self, const char *message)
{
    AdwToast *t = adw_toast_new(message);
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

    if (page >= 0)
        sub = g_strdup_printf("%s \xc2\xb7 %s \xc2\xb7 MegaModule page %d",
                              pmd85session_model_name(pmd85session_model(s)),
                              link, page);
    else
        sub = g_strdup_printf("%s \xc2\xb7 %s",
                              pmd85session_model_name(pmd85session_model(s)),
                              link);
    adw_window_title_set_subtitle(self->title, sub);

    for (i = 0; i < 3; i++)
        gtk_widget_set_opacity(self->led[i], (leds & (1 << i)) ? 1.0 : 0.18);

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

static gboolean window_key(Pmd85Window *self, guint keyval,
                           GdkModifierType state, gboolean down)
{
    const gboolean f_key = keyval == GDK_KEY_F || keyval == GDK_KEY_f;

    if (!f_key)
        self->ctlf_down = FALSE;
    if (f_key && down && (state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK)) {
        if (!self->ctlf_down) {
            self->ctlf_down = TRUE;
            if (gtk_window_is_fullscreen(GTK_WINDOW(self)))
                gtk_window_unfullscreen(GTK_WINDOW(self));
            else
                gtk_window_fullscreen(GTK_WINDOW(self));
        }
        return TRUE;
    }
    if (f_key && !down && self->ctlf_down) {
        self->ctlf_down = FALSE;
        return TRUE;
    }

    if (keyval == GDK_KEY_F12) {
        if (down)
            pmd85_debugger_toggle(GTK_WINDOW(self), self->session);
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

static void action_module(GSimpleAction *action, GVariant *param,
                          gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    pmd85session_set_module(self->session, g_variant_get_int32(param));
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

static void import_done(GObject *source, GAsyncResult *result,
                        gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    g_autoptr(GFile) file =
        gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
    char dest[1024];
    g_autofree char *path = NULL;
    g_autofree char *msg = NULL;
    g_autofree char *lower = NULL;
    const char *base;

    if (!file)
        return;
    path = g_file_get_path(file);
    if (!path)
        return;
    if (pmd85session_import_media(self->session, path, dest, sizeof(dest)) != 0) {
        pmd85_window_toast(self, pmd85session_last_error(self->session));
        return;
    }
    base = strrchr(dest, '/') ? strrchr(dest, '/') + 1 : dest;
    lower = g_ascii_strdown(base, -1);
    if (g_str_has_suffix(lower, ".rom"))
        msg = g_strdup_printf("Copied %s to the ROM folder", base);
    else
        msg = g_strdup_printf("Copied to FujiNet SD: %s. Mount it from FujiNet "
                              "CONFIG.", base);
    pmd85_window_toast(self, msg);
}

static void action_import_media(GSimpleAction *action, GVariant *param,
                                gpointer user_data)
{
    Pmd85Window *self = PMD85_WINDOW(user_data);
    GtkFileDialog *dialog = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *media = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    static const char *const exts[] = { "rmm", "mrm", "p32", "ptp", "pmd",
                                        "rom", NULL };
    int i;
    (void)action;
    (void)param;

    gtk_file_filter_set_name(media, "PMD 85 media (ROM modules, disks, tapes)");
    for (i = 0; exts[i]; i++)
        gtk_file_filter_add_suffix(media, exts[i]);
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, media);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dialog, "Import Media");
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dialog, GTK_WINDOW(self), NULL, import_done, self);
    g_object_unref(media);
    g_object_unref(all);
    g_object_unref(filters);
    g_object_unref(dialog);
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

static GMenu *build_menu(void)
{
    GMenu *menu = g_menu_new();
    GMenu *machine = g_menu_new();
    GMenu *view = g_menu_new();
    GMenu *media = g_menu_new();
    GMenu *fujinet = g_menu_new();
    GMenu *tail = g_menu_new();
    GMenu *sub;

    sub = radio_menu("win.model", pmd85session_model_name);
    g_menu_append_submenu(machine, "Model", G_MENU_MODEL(sub));
    g_object_unref(sub);
    sub = radio_menu("win.module", pmd85session_module_name);
    g_menu_append_submenu(machine, "ROM Module", G_MENU_MODEL(sub));
    g_object_unref(sub);
    g_menu_append(machine, "RESET", "win.reset");
    g_menu_append(machine, "Cold Restart", "win.cold");
    g_menu_append_section(menu, "Machine", G_MENU_MODEL(machine));

    sub = radio_menu("win.color", pmd85session_color_name);
    g_menu_append_submenu(view, "Colours", G_MENU_MODEL(sub));
    g_object_unref(sub);
    g_menu_append(view, "Fullscreen (Ctrl+Shift+F)", "win.fullscreen");
    g_menu_append_section(menu, "View", G_MENU_MODEL(view));

    g_menu_append(media, "Import Media\xe2\x80\xa6", "win.import-media");
    g_menu_append_section(menu, "Media", G_MENU_MODEL(media));

    g_menu_append(fujinet, "FujiNet Configuration\xe2\x80\xa6", "win.fujinet-config");
    g_menu_append(fujinet, "FujiNet Console Log\xe2\x80\xa6", "win.fujinet-log");
    g_menu_append_section(menu, "FujiNet", G_MENU_MODEL(fujinet));

    g_menu_append(tail, "Debugger (F12)", "win.debugger");
    g_menu_append(tail, "Preferences\xe2\x80\xa6", "win.preferences");
    g_menu_append(tail, "About FujiNet Go PMD 85", "win.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(tail));

    g_object_unref(machine);
    g_object_unref(view);
    g_object_unref(media);
    g_object_unref(fujinet);
    g_object_unref(tail);
    return menu;
}

static const GActionEntry win_actions[] = {
    {.name = "reset", .activate = action_reset},
    {.name = "cold", .activate = action_cold},
    {.name = "debugger", .activate = action_debugger},
    {.name = "fullscreen", .activate = action_fullscreen},
    {.name = "import-media", .activate = action_import_media},
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

static void pmd85_window_dispose(GObject *object)
{
    Pmd85Window *self = PMD85_WINDOW(object);
    if (self->status_id) {
        g_source_remove(self->status_id);
        self->status_id = 0;
    }
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
    menu = build_menu();
    gtk_menu_button_set_menu_model(self->menu_button, G_MENU_MODEL(menu));
    g_object_unref(menu);
    adw_header_bar_pack_end(header, GTK_WIDGET(self->menu_button));
    adw_header_bar_pack_start(header, led_box(self));

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
    return GTK_WIDGET(self);
}
