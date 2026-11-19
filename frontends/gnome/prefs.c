/*
 * Preferences dialog: machine, display and FujiNet options. Values persist
 * through the shared settings store, so every frontend sees the same
 * configuration.
 *
 * Three kinds of option, by when they take effect:
 *
 *   display    how the widget paints (aspect, scaling) -- at once;
 *   machine    model, ROM module, peripherals -- the machine is rebuilt and
 *              cold-started at once (FujiNet stays up, the link reconnects);
 *              the colour profile applies without a restart;
 *   session    the FujiNet and audio choices decide what the session starts,
 *              so they apply when the dialog closes, by restarting it.
 *
 * Shape from fujinet-go-coco-desktop's prefs.c.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include <string.h>

#include "display.h"

static const char *const aspect_names[] = {"TV (4:3)", "Square pixels",
                                           "Integer scale", NULL};
/* in Pmd85Scaling order */
static const char *const scaling_names[] = {"Sharp", "Nearest", "Smooth", NULL};

typedef struct {
    Pmd85Window *window;
    pmd85session *session;
    gboolean session_dirty;  /* a session option changed: restart on close */
    GtkWidget *addr_row;
    GtkWidget *module_row;   /* the module combo */
    GtkWidget *file_row;     /* the module file and its Choose button */
    AdwPreferencesGroup *pads;   /* the gamepads, one row each */
    GPtrArray *pad_rows;
    unsigned pad_gen;
    guint pad_timer;
    gboolean pad_filling;    /* rows being (re)built: their combos are not the user's */
} PrefsState;

/* ---- row helpers ----------------------------------------------------------- */

static GtkStringList *string_list(const char *(*name_at)(int))
{
    GtkStringList *l = gtk_string_list_new(NULL);
    const char *s;
    for (int i = 0; (s = name_at(i)) != NULL; i++)
        gtk_string_list_append(l, s);
    return l;
}

static const char *aspect_name(int i)
{
    return (i >= 0 && i < 3) ? aspect_names[i] : NULL;
}

static const char *scaling_name(int i)
{
    return (i >= 0 && i < 3) ? scaling_names[i] : NULL;
}

static GtkWidget *combo_row(const char *title, const char *subtitle,
                            const char *(*name_at)(int), int selected,
                            GCallback changed, gpointer data)
{
    GtkWidget *row = adw_combo_row_new();
    GtkStringList *model = string_list(name_at);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    g_object_unref(model);
    adw_combo_row_set_selected(ADW_COMBO_ROW(row), (guint)selected);
    g_signal_connect(row, "notify::selected", changed, data);
    return row;
}

static GtkWidget *switch_row(const char *title, const char *subtitle,
                             gboolean active, const char *key,
                             GCallback changed, gpointer data)
{
    GtkWidget *row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row), active);
    g_object_set_data(G_OBJECT(row), "pmd85-key", (gpointer)key);
    g_signal_connect(row, "notify::active", changed, data);
    return row;
}

static int selected(GObject *row)
{
    return (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
}

static gboolean active(GObject *row)
{
    return adw_switch_row_get_active(ADW_SWITCH_ROW(row));
}

/* ---- handlers ------------------------------------------------------------ */

static void model_changed(GObject *row, GParamSpec *p, gpointer ud)
{
    PrefsState *st = ud;
    (void)p;
    if (selected(row) == pmd85session_model(st->session)) return;
    pmd85session_set_model(st->session, selected(row));
    pmd85_window_sync_menu(st->window);
}

/* The module rows, from the session. The chooser can finish after the
 * dialog closed, so this takes the rows (kept alive by the job), not st. */
typedef struct {
    pmd85session *session;
    GtkWidget *module_row, *file_row;
} ModuleRows;

static void module_rows_refresh(const ModuleRows *r)
{
    const char *path = pmd85session_module_file(r->session);
    g_autofree char *base = *path ? g_path_get_basename(path) : NULL;
    adw_combo_row_set_selected(ADW_COMBO_ROW(r->module_row),
                               (guint)pmd85session_module(r->session));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(r->file_row),
        !base ? "None chosen: .rmm (32 KB ROM pack) or .mrm (MegaModule)"
        : pmd85session_module_file_ok(r->session) ? base
        : "Missing: choose it again");
}

static void module_rows_done(gpointer data, gboolean chosen)
{
    (void)chosen;
    module_rows_refresh(data);
}

static void module_rows_free(gpointer data)
{
    ModuleRows *r = data;
    g_object_unref(r->module_row);
    g_object_unref(r->file_row);
    g_free(r);
}

static void choose_module_file(PrefsState *st)
{
    ModuleRows *r = g_new0(ModuleRows, 1);
    r->session = st->session;
    r->module_row = g_object_ref(st->module_row);
    r->file_row = g_object_ref(st->file_row);
    pmd85_window_choose_module_file(st->window, module_rows_done, r, module_rows_free);
}

static void module_changed(GObject *row, GParamSpec *p, gpointer ud)
{
    PrefsState *st = ud;
    (void)p;
    if (selected(row) == pmd85session_module(st->session)) return;
    if (selected(row) == PMD85SESSION_MODULE_FILE &&
        !pmd85session_module_file_ok(st->session)) {
        choose_module_file(st);   /* switches, or puts the combo back */
        return;
    }
    pmd85session_set_module(st->session, selected(row));
    pmd85_window_sync_menu(st->window);
}

/* the two tape switches carry their setting's key; both are written */
static void tape_changed(GObject *row, GParamSpec *p, gpointer ud)
{
    PrefsState *st = ud;
    int flash = pmd85session_get_int(st->session, "tape_flash", 1);
    int sound = pmd85session_get_int(st->session, "tape_sound", 0);
    const char *key = g_object_get_data(row, "pmd85-key");
    (void)p;
    if (!strcmp(key, "tape_flash")) flash = active(row);
    else sound = active(row);
    pmd85session_set_tape_options(st->session, flash, sound);
}

static void module_file_clicked(GtkButton *button, gpointer ud)
{
    (void)button;
    choose_module_file(ud);
}

static void color_changed(GObject *row, GParamSpec *p, gpointer ud)
{
    PrefsState *st = ud;
    (void)p;
    pmd85session_set_color_profile(st->session, selected(row));
    pmd85_window_sync_menu(st->window);
}

static void aspect_changed(GObject *row, GParamSpec *p, gpointer ud)
{
    PrefsState *st = ud;
    (void)p;
    pmd85session_set_int(st->session, "aspect_mode", selected(row));
    pmd85_window_apply_display(st->window);
}

static void scaling_changed(GObject *row, GParamSpec *p, gpointer ud)
{
    PrefsState *st = ud;
    (void)p;
    pmd85session_set_int(st->session, "scaling", selected(row));
    pmd85_window_apply_display(st->window);
}

/* A peripheral or machine option: stored, and the machine rebuilt. */
static void peripheral_changed(GObject *row, GParamSpec *p, gpointer ud)
{
    PrefsState *st = ud;
    const char *key = g_object_get_data(row, "pmd85-key");
    (void)p;
    pmd85session_set_peripheral(st->session, key, active(row));
}

/* Keyboard options: read when the machine is built. Stored, and applied by a
 * cold restart, which is what changing them on the real machine's software
 * would need anyway. */
static void keyboard_changed(GObject *row, GParamSpec *p, gpointer ud)
{
    PrefsState *st = ud;
    const char *key = g_object_get_data(row, "pmd85-key");
    (void)p;
    pmd85session_set_int(st->session, key, active(row));
    pmd85session_hard_reset(st->session);
}

/* A session option: stored, applied on close. */
static void session_switch_changed(GObject *row, GParamSpec *p, gpointer ud)
{
    PrefsState *st = ud;
    const char *key = g_object_get_data(row, "pmd85-key");
    (void)p;
    pmd85session_set_int(st->session, key, active(row));
    st->session_dirty = TRUE;
    if (!strcmp(key, "fujinet_external") && st->addr_row)
        gtk_widget_set_sensitive(st->addr_row, active(row));
}

static void addr_changed(GtkEditable *entry, gpointer ud)
{
    PrefsState *st = ud;
    const char *text = gtk_editable_get_text(entry);
    char host[64];
    int port;

    if (pmd85session_parse_hostport(text, host, sizeof host, &port) != 0) {
        gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
        return;
    }
    gtk_widget_remove_css_class(GTK_WIDGET(entry), "error");
    if (g_strcmp0(pmd85session_get_str(st->session, "fujinet_external_addr", ""),
                  text) == 0)
        return;
    pmd85session_set_str(st->session, "fujinet_external_addr", text);
    if (pmd85session_get_int(st->session, "fujinet_external", 0))
        st->session_dirty = TRUE;
}

static void prefs_closed(AdwDialog *dialog, gpointer ud)
{
    PrefsState *st = ud;
    (void)dialog;
    if (st->pad_timer)
        g_source_remove(st->pad_timer);
    if (st->pad_rows)
        g_ptr_array_unref(st->pad_rows);
    pmd85session_settings_flush(st->session);
    if (st->session_dirty)
        pmd85_window_restart_session(st->window);
    g_free(st);
}

/* ---- pages -------------------------------------------------------------- */

static AdwPreferencesPage *machine_page(PrefsState *st)
{
    pmd85session *s = st->session;
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *machine = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *periph = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *keyb = ADW_PREFERENCES_GROUP(adw_preferences_group_new());

    adw_preferences_page_set_title(page, "Machine");
    adw_preferences_page_set_icon_name(page, "computer-symbolic");

    adw_preferences_group_set_title(machine, "Machine");
    adw_preferences_group_set_description(machine,
        "Changing these restarts the PMD (FujiNet keeps running).");
    adw_preferences_group_add(machine, combo_row("Model", NULL,
        pmd85session_model_name, pmd85session_model(s),
        G_CALLBACK(model_changed), st));
    st->module_row = combo_row("ROM module",
        "FujiNet boots FujiNet CONFIG on a PMD 85-2 or 85-2A; other models "
        "start it from the monitor",
        pmd85session_module_name, pmd85session_module(s),
        G_CALLBACK(module_changed), st);
    adw_preferences_group_add(machine, st->module_row);
    {
        GtkWidget *choose = gtk_button_new_with_label("Choose\xe2\x80\xa6");
        ModuleRows r;
        gtk_widget_set_valign(choose, GTK_ALIGN_CENTER);
        g_signal_connect(choose, "clicked", G_CALLBACK(module_file_clicked), st);
        st->file_row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->file_row), "Module file");
        adw_action_row_add_suffix(ADW_ACTION_ROW(st->file_row), choose);
        adw_preferences_group_add(machine, st->file_row);
        r.session = s;
        r.module_row = st->module_row;
        r.file_row = st->file_row;
        module_rows_refresh(&r);
    }
    adw_preferences_group_add(machine, switch_row("256 KB RAM expansion",
        "PMD 85-2A and 85-3", pmd85session_get_int(s, "ram256k", 0), "ram256k",
        G_CALLBACK(peripheral_changed), st));
    adw_preferences_group_add(machine, switch_row("PMD 85-2 compatibility",
        "PMD 85-3: starts in the 85-2 mode, which auto-runs the ROM module",
        pmd85session_get_int(s, "compat_mode", 0), "compat_mode",
        G_CALLBACK(peripheral_changed), st));
    adw_preferences_page_add(page, machine);

    adw_preferences_group_set_title(periph, "Peripherals");
    adw_preferences_group_add(periph, switch_row("MIF 85 music interface",
        "SAA1099 sound on ports ECh-EFh", pmd85session_get_int(s, "mif85", 0),
        "mif85", G_CALLBACK(peripheral_changed), st));
    adw_preferences_group_add(periph, switch_row("PMD 32 disk drive",
        "On the GPIO port (takes joystick port A)",
        pmd85session_get_int(s, "pmd32", 0), "pmd32",
        G_CALLBACK(peripheral_changed), st));
    adw_preferences_page_add(page, periph);

    {
        AdwPreferencesGroup *tape = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(tape, "Tape");
        adw_preferences_group_add(tape, switch_row("Flash load",
            "A program's body goes into memory at once; off loads in real "
            "time at 1200 Bd", pmd85session_get_int(s, "tape_flash", 1),
            "tape_flash", G_CALLBACK(tape_changed), st));
        adw_preferences_group_add(tape, switch_row("Tape sound",
            "Hear the tape while it plays", pmd85session_get_int(s, "tape_sound", 0),
            "tape_sound", G_CALLBACK(tape_changed), st));
        adw_preferences_page_add(page, tape);
    }

    adw_preferences_group_set_title(keyb, "Keyboard");
    adw_preferences_group_set_description(keyb,
        "F1\xe2\x80\x93" "F12 are K0\xe2\x80\x93" "K11, Esc and Ctrl are STOP, "
        "Tab is C-D, ` is WRK, PgUp/PgDn are RCL/CLR. The app's keys: "
        "Alt+F fullscreen, Alt+C colors, Alt+T tape browser, Alt+P tape play/stop, "
        "Alt+F5 RESET, Alt+Shift+F5 cold restart, Alt+F12 debugger.");
    adw_preferences_group_add(keyb, switch_row("Swap Z and Y",
        "For QWERTZ keyboards", pmd85session_get_int(s, "exchange_zy", 1),
        "exchange_zy", G_CALLBACK(keyboard_changed), st));
    adw_preferences_group_add(keyb, switch_row("Numeric keypad",
        "Type digits and operators from the keypad",
        pmd85session_get_int(s, "numpad", 1), "numpad",
        G_CALLBACK(keyboard_changed), st));
    adw_preferences_page_add(page, keyb);
    return page;
}

static AdwPreferencesPage *display_page(PrefsState *st)
{
    pmd85session *s = st->session;
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());

    adw_preferences_page_set_title(page, "Display");
    adw_preferences_page_set_icon_name(page, "video-display-symbolic");
    adw_preferences_group_add(g, combo_row("Colors",
        "Black & White is the TV picture (white, half bright, blinking), Green the "
        "same on a green monitor; RGBM and ColorACE are the color modifications "
        "(Alt+C)", pmd85session_color_name,
        pmd85session_color_profile(s), G_CALLBACK(color_changed), st));
    adw_preferences_group_add(g, combo_row("Aspect", NULL, aspect_name,
        pmd85session_get_int(s, "aspect_mode", PMD85_ASPECT_TV_4_3),
        G_CALLBACK(aspect_changed), st));
    adw_preferences_group_add(g, combo_row("Scaling",
        "Sharp: even, crisp pixels at any size. Nearest: crisp, uneven. "
        "Smooth: soft. (Integer scale is always crisp.)",
        scaling_name, pmd85session_get_int(s, "scaling", PMD85_SCALING_SHARP),
        G_CALLBACK(scaling_changed), st));
    adw_preferences_page_add(page, g);
    return page;
}

static AdwPreferencesPage *fujinet_page(PrefsState *st)
{
    pmd85session *s = st->session;
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *a = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    GtkWidget *addr;
    g_autofree char *desc = NULL;

    adw_preferences_page_set_title(page, "FujiNet");
    adw_preferences_page_set_icon_name(page, "network-workgroup-symbolic");

    desc = g_strdup_printf("The PMD talks DriveWire to FujiNet through a "
                           "Becker port, now %s. These apply when this "
                           "dialog closes.",
                           pmd85session_becker_endpoint(s));
    adw_preferences_group_set_title(g, "FujiNet");
    adw_preferences_group_set_description(g, desc);
    adw_preferences_group_add(g, switch_row("Built-in FujiNet", NULL,
        pmd85session_get_int(s, "enable_fujinet", 1), "enable_fujinet",
        G_CALLBACK(session_switch_changed), st));
    adw_preferences_group_add(g, switch_row("Use an external FujiNet",
        "A FujiNet running elsewhere, e.g. fujinet-pc on another machine",
        pmd85session_get_int(s, "fujinet_external", 0), "fujinet_external",
        G_CALLBACK(session_switch_changed), st));
    addr = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(addr),
                                  "External FujiNet (host:port)");
    gtk_editable_set_text(GTK_EDITABLE(addr),
        pmd85session_get_str(s, "fujinet_external_addr", ""));
    gtk_widget_set_sensitive(addr, pmd85session_get_int(s, "fujinet_external", 0));
    g_signal_connect(addr, "changed", G_CALLBACK(addr_changed), st);
    st->addr_row = addr;
    adw_preferences_group_add(g, addr);
    adw_preferences_page_add(page, g);

    adw_preferences_group_set_title(a, "Sound");
    adw_preferences_group_add(a, switch_row("Sound", NULL,
        pmd85session_get_int(s, "enable_audio", 1), "enable_audio",
        G_CALLBACK(session_switch_changed), st));
    adw_preferences_page_add(page, a);
    return page;
}

/* ---- input: the gamepads ------------------------------------------------- */

static const char *const pad_choices[] = { "Automatic", "Joystick 1", "Joystick 2" };

static const char *pad_choice(int i)
{
    return (i >= 0 && i < 3) ? pad_choices[i] : NULL;
}

static void pad_assign_changed(GObject *row, GParamSpec *p, gpointer ud)
{
    PrefsState *st = ud;
    (void)p;
    if (st->pad_filling)
        return;
    /* Automatic = -1, Joystick N = N - 1 */
    pmd85session_gamepad_assign(st->session,
        GPOINTER_TO_INT(g_object_get_data(row, "pmd85-pad")), selected(row) - 1);
}

/* The rows anew from the session's pads (on open and whenever the pads or
 * their joysticks changed). */
static void pads_fill(PrefsState *st)
{
    const int n = pmd85session_gamepad_count(st->session);
    const unsigned avail = pmd85session_joysticks(st->session);

    st->pad_filling = TRUE;
    st->pad_gen = pmd85session_gamepad_generation(st->session);
    for (guint i = 0; i < st->pad_rows->len; i++)
        adw_preferences_group_remove(st->pads, g_ptr_array_index(st->pad_rows, i));
    g_ptr_array_set_size(st->pad_rows, 0);

    if (n == 0) {
        GtkWidget *row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "No gamepads connected");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
            "Plug one in: it takes the first free joystick");
        gtk_widget_add_css_class(row, "dim-label");
        adw_preferences_group_add(st->pads, row);
        g_ptr_array_add(st->pad_rows, row);
    }
    for (int i = 0; i < n; i++) {
        char name[96];
        int port = pmd85session_gamepad_effective_port(st->session, i);
        g_autofree char *sub = port >= 0
            ? g_strdup_printf("Drives joystick %d", port + 1)
            : g_strdup(avail ? "Not used: no joystick free" : "Not used: the joystick interface is off");
        GtkWidget *row;
        pmd85session_gamepad_name(st->session, i, name, sizeof name);
        row = combo_row(name, sub, pad_choice,
                        pmd85session_gamepad_assignment(st->session, i) + 1,
                        G_CALLBACK(pad_assign_changed), st);
        g_object_set_data(G_OBJECT(row), "pmd85-pad", GINT_TO_POINTER(i));
        adw_preferences_group_add(st->pads, row);
        g_ptr_array_add(st->pad_rows, row);
    }
    st->pad_filling = FALSE;
}

static gboolean pads_tick(gpointer ud)
{
    PrefsState *st = ud;
    if (pmd85session_gamepad_generation(st->session) != st->pad_gen)
        pads_fill(st);
    return G_SOURCE_CONTINUE;
}

/* The joystick interface and the mouse: peripherals, the machine rebuilt;
 * the gamepads' rows follow (which joysticks there are changed). */
static void input_peripheral_changed(GObject *row, GParamSpec *p, gpointer ud)
{
    PrefsState *st = ud;
    (void)p;
    pmd85session_set_peripheral(st->session, g_object_get_data(row, "pmd85-key"), active(row));
    if (st->pads)
        pads_fill(st);
}

/* A setting read where it is used, so at once. */
static void plain_switch_changed(GObject *row, GParamSpec *p, gpointer ud)
{
    PrefsState *st = ud;
    (void)p;
    pmd85session_set_int(st->session, g_object_get_data(row, "pmd85-key"), active(row));
}

/* The left stick: read by the pad thread on every poll, so at once. */
static void analog_changed(GObject *row, GParamSpec *p, gpointer ud)
{
    PrefsState *st = ud;
    (void)p;
    pmd85session_set_int(st->session, "analog_stick", active(row));
}

static AdwPreferencesPage *input_page(PrefsState *st)
{
    pmd85session *s = st->session;
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());

    adw_preferences_page_set_title(page, "Input");
    adw_preferences_page_set_name(page, "input");
    adw_preferences_page_set_icon_name(page, "input-gaming-symbolic");

    st->pads = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(st->pads, "Gamepads");
    adw_preferences_group_set_description(st->pads,
        "Each drives a 4004/482 joystick: the D-pad steers, A, B, the shoulders "
        "and the triggers fire. Joystick 1 is GPIO port A (not while the PMD 32 "
        "is connected), joystick 2 port B.");
    adw_preferences_group_add(st->pads, switch_row("Joystick 4004/482",
        "The interface on the GPIO ports (the machine restarts)",
        pmd85session_get_int(s, "joystick", 1), "joystick",
        G_CALLBACK(input_peripheral_changed), st));
    st->pad_rows = g_ptr_array_new();
    pads_fill(st);
    adw_preferences_page_add(page, st->pads);

    adw_preferences_group_add(g, switch_row("Left stick steers",
        "The analog stick works as the joystick too, besides the D-pad",
        pmd85session_get_int(s, "analog_stick", 1), "analog_stick",
        G_CALLBACK(analog_changed), st));
    adw_preferences_page_add(page, g);

    {
        AdwPreferencesGroup *m = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
        adw_preferences_group_set_title(m, "Mouse 602");
        adw_preferences_group_set_description(m,
            "The pointer over the PMD's screen moves the mouse and its buttons are the "
            "mouse's (the middle one presses both). PMD 85-1, -2, -2A, -3 and Consul 2717.");
        adw_preferences_group_add(m, switch_row("Mouse 602",
            "On port 8Ch (the machine restarts)",
            pmd85session_get_int(s, "mouse602", 0), "mouse602",
            G_CALLBACK(input_peripheral_changed), st));
        adw_preferences_group_add(m, switch_row("Hide the pointer over the screen",
            "PMD software draws its own", pmd85session_get_int(s, "mouse_hide_pointer", 1),
            "mouse_hide_pointer", G_CALLBACK(plain_switch_changed), st));
        adw_preferences_page_add(page, m);
    }

    st->pad_timer = g_timeout_add(500, pads_tick, st);
    return page;
}

void pmd85_prefs_show(Pmd85Window *parent, pmd85session *session)
{
    AdwPreferencesDialog *dlg =
        ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    PrefsState *st = g_new0(PrefsState, 1);

    st->window = parent;
    st->session = session;
    adw_preferences_dialog_add(dlg, machine_page(st));
    adw_preferences_dialog_add(dlg, display_page(st));
    adw_preferences_dialog_add(dlg, input_page(st));
    adw_preferences_dialog_add(dlg, fujinet_page(st));
    g_signal_connect(dlg, "closed", G_CALLBACK(prefs_closed), st);
    {   /* PMD85_PREFS_PAGE=input: open on that page (development) */
        const char *pg = g_getenv("PMD85_PREFS_PAGE");
        if (pg && *pg)
            adw_preferences_dialog_set_visible_page_name(dlg, pg);
    }
    adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(parent));
}
