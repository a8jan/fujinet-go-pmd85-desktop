/*
 * The tape browser: GPMD85Emulator's tape dialog (gui/TapeDialog.cpp) for
 * GNOME. The tape's files as a list -- the position (where Play starts) and
 * the STOP marker beside them -- with Play/Stop, previous/next file and
 * Rewind over it. Double-click (or Enter) on a file moves the position
 * there; the context menu also sets the marker, deletes and moves files.
 * Flash load and tape sound are toggles beside them; Load/Append/Save As/
 * Eject, auto stop and hex/dec are in the header bar.
 *
 * The deck is the emulator thread's: everything here goes through the
 * session, and the view follows the deck's published state (100 ms).
 *
 * Copyright (C) 2026 FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tape.h"

#include <string.h>

/* ---- a row: one block, with what changes while it is shown --------------- */

#define PMD85_TYPE_TAPE_ROW (pmd85_tape_row_get_type())
G_DECLARE_FINAL_TYPE(Pmd85TapeRow, pmd85_tape_row, PMD85, TAPE_ROW, GObject)

struct _Pmd85TapeRow {
    GObject parent;
    pmd85session_tape_block block;
    char *marker;   /* an icon name: the position, the STOP marker, or "" */
    char *start, *length;
};

enum { ROW_PROP_0, ROW_PROP_MARKER, ROW_PROP_START, ROW_PROP_LENGTH, ROW_N_PROPS };
static GParamSpec *row_props[ROW_N_PROPS];

G_DEFINE_FINAL_TYPE(Pmd85TapeRow, pmd85_tape_row, G_TYPE_OBJECT)

static void row_get_property(GObject *obj, guint id, GValue *value, GParamSpec *pspec)
{
    Pmd85TapeRow *r = PMD85_TAPE_ROW(obj);
    switch (id) {
    case ROW_PROP_MARKER: g_value_set_string(value, r->marker); break;
    case ROW_PROP_START: g_value_set_string(value, r->start); break;
    case ROW_PROP_LENGTH: g_value_set_string(value, r->length); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void row_finalize(GObject *obj)
{
    Pmd85TapeRow *r = PMD85_TAPE_ROW(obj);
    g_free(r->marker);
    g_free(r->start);
    g_free(r->length);
    G_OBJECT_CLASS(pmd85_tape_row_parent_class)->finalize(obj);
}

static void pmd85_tape_row_class_init(Pmd85TapeRowClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->get_property = row_get_property;
    oc->finalize = row_finalize;
    row_props[ROW_PROP_MARKER] = g_param_spec_string("marker", NULL, NULL, "",
                                                     G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    row_props[ROW_PROP_START] = g_param_spec_string("start", NULL, NULL, "",
                                                    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    row_props[ROW_PROP_LENGTH] = g_param_spec_string("length", NULL, NULL, "",
                                                     G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties(oc, ROW_N_PROPS, row_props);
}

static void pmd85_tape_row_init(Pmd85TapeRow *r)
{
    r->marker = g_strdup("");
    r->start = g_strdup("");
    r->length = g_strdup("");
}

/* takes text; tells the bound cells only when it changed */
static void row_set(Pmd85TapeRow *r, int prop, char *text)
{
    char **field = prop == ROW_PROP_MARKER ? &r->marker
                 : prop == ROW_PROP_START ? &r->start : &r->length;
    if (!strcmp(*field, text)) {
        g_free(text);
        return;
    }
    g_free(*field);
    *field = text;
    g_object_notify_by_pspec(G_OBJECT(r), row_props[prop]);
}

/* ---- the window ----------------------------------------------------------- */

typedef struct {
    Pmd85Window *main;
    pmd85session *session;
    GtkWindow *win;
    AdwWindowTitle *title;
    AdwToastOverlay *toasts;
    GtkStack *stack;
    GtkWidget *play, *prev, *next, *rewind, *flash, *sound;
    GtkProgressBar *progress;
    GtkColumnView *view;
    GListStore *store;
    GtkMultiSelection *sel;
    GtkWidget *popover;
    GSimpleActionGroup *actions;
    pmd85session_tape_info info;
    gboolean have_list;
    int hex;
    int menu_row;          /* the row the context menu is for, -1 none */
    guint timer;
} TapeState;

static TapeState *g_tape;   /* the one open */

static void refresh(TapeState *st);

static void toast(TapeState *st, const char *text)
{
    AdwToast *t = adw_toast_new(text);
    adw_toast_set_timeout(t, 3);
    adw_toast_overlay_add_toast(st->toasts, t);
}

static void result(TapeState *st, int rc)
{
    if (rc != 0)
        toast(st, pmd85session_last_error(st->session));
    refresh(st);
}

static char *fmt_start(const TapeState *st, int v)
{
    if (v < 0) return g_strdup("");
    return st->hex ? g_strdup_printf("%04X", v) : g_strdup_printf("%d", v);
}

static char *fmt_length(const TapeState *st, int v)
{
    return st->hex ? g_strdup_printf("%04X", v) : g_strdup_printf("%d", v);
}

static guint n_rows(const TapeState *st)
{
    return g_list_model_get_n_items(G_LIST_MODEL(st->store));
}

static Pmd85TapeRow *row_at(const TapeState *st, guint i)
{
    /* the store keeps it; the caller only borrows */
    Pmd85TapeRow *r = g_list_model_get_item(G_LIST_MODEL(st->store), i);
    if (r) g_object_unref(r);
    return r;
}

/* the selected rows, ascending; *n of them (g_free the array) */
static int *selected_rows(const TapeState *st, int *n)
{
    GtkBitset *set = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(st->sel));
    const guint64 count = gtk_bitset_get_size(set);
    int *rows = g_new(int, count ? count : 1);
    GtkBitsetIter it;
    guint pos;
    int k = 0;
    if (gtk_bitset_iter_init_first(&it, set, &pos)) {
        do rows[k++] = (int)pos;
        while (gtk_bitset_iter_next(&it, &pos));
    }
    gtk_bitset_unref(set);
    *n = k;
    return rows;
}

static void select_range(TapeState *st, int first, int last)
{
    GtkSelectionModel *m = GTK_SELECTION_MODEL(st->sel);
    gtk_selection_model_unselect_all(m);
    if (first >= 0 && last >= first && (guint)last < n_rows(st))
        gtk_selection_model_select_range(m, (guint)first, (guint)(last - first + 1), TRUE);
}

/* The rows anew from the deck's list. The selection stays when the list
 * kept its length (an edit that moved blocks re-selects them itself). */
static void rebuild(TapeState *st)
{
    int n = pmd85session_tape_blocks(st->session, NULL, 0, NULL);
    pmd85session_tape_block *b = g_new0(pmd85session_tape_block, n > 0 ? n : 1);
    GPtrArray *rows = g_ptr_array_new_with_free_func(g_object_unref);
    const guint old = n_rows(st);
    int sel_n = 0;
    g_autofree int *sel = selected_rows(st, &sel_n);

    n = pmd85session_tape_blocks(st->session, b, n, NULL);
    for (int i = 0; i < n; i++) {
        Pmd85TapeRow *r = g_object_new(PMD85_TYPE_TAPE_ROW, NULL);
        r->block = b[i];
        g_free(r->start);
        g_free(r->length);
        r->start = fmt_start(st, b[i].start);
        r->length = fmt_length(st, b[i].length);
        g_ptr_array_add(rows, r);
    }
    g_list_store_splice(st->store, 0, old, rows->pdata, rows->len);
    if ((guint)n == old)
        for (int i = 0; i < sel_n; i++)
            gtk_selection_model_select_item(GTK_SELECTION_MODEL(st->sel), (guint)sel[i], FALSE);
    g_ptr_array_unref(rows);
    g_free(b);
    st->have_list = TRUE;
}

static void set_enabled(TapeState *st, const char *name, gboolean on)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(st->actions), name);
    if (a) g_simple_action_set_enabled(G_SIMPLE_ACTION(a), on);
}

/* the row the menu's position/marker items are for */
static int target_row(TapeState *st)
{
    int n = 0;
    g_autofree int *sel = NULL;
    if (st->menu_row >= 0 && (guint)st->menu_row < n_rows(st))
        return st->menu_row;
    sel = selected_rows(st, &n);
    return n > 0 ? sel[0] : -1;
}

static void update_actions(TapeState *st)
{
    const pmd85session_tape_info *t = &st->info;
    const gboolean have = t->blocks > 0, idle = have && !t->playing;
    const int row = target_row(st);
    int n = 0;
    g_autofree int *sel = selected_rows(st, &n);
    const int first = n > 0 ? sel[0] : row, last = n > 0 ? sel[n - 1] : row;
    const gboolean run = n == 0 || last - first + 1 == n;

    set_enabled(st, "seek", idle && row >= 0 && row != t->current);
    set_enabled(st, "mark", idle && row > t->current && row != t->marker);
    set_enabled(st, "unmark", idle && t->marker >= 0);
    set_enabled(st, "delete", idle && (n > 0 || row >= 0));
    set_enabled(st, "up", idle && run && first > 0);
    set_enabled(st, "down", idle && run && last >= 0 && last < t->blocks - 1);
    set_enabled(st, "select-all", have);
    set_enabled(st, "save", have);
    set_enabled(st, "eject", have);
    gtk_widget_set_sensitive(st->play, have);
    gtk_widget_set_sensitive(st->prev, pmd85_window_tape_file(st->main, -1) >= 0);
    gtk_widget_set_sensitive(st->next, pmd85_window_tape_file(st->main, 1) >= 0);
    gtk_widget_set_sensitive(st->rewind, have);
}

/* the option buttons as the settings are (Preferences may change them) */
static void sync_options(TapeState *st)
{
    static const char *const keys[] = { "flash", "sound" };
    for (int i = 0; i < 2; i++) {
        GAction *a = g_action_map_lookup_action(G_ACTION_MAP(st->actions), keys[i]);
        const gboolean on = pmd85session_get_int(st->session, i ? "tape_sound" : "tape_flash",
                                                 i ? 0 : 1) != 0;
        g_autoptr(GVariant) state = g_action_get_state(a);
        if (g_variant_get_boolean(state) != on)
            g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_boolean(on));
        if (i == 1)
            gtk_button_set_icon_name(GTK_BUTTON(st->sound), on ? "audio-volume-high-symbolic"
                                                               : "audio-volume-muted-symbolic");
    }
}

static void refresh(TapeState *st)
{
    pmd85session_tape_info t;
    const char *name;
    g_autofree char *sub = NULL, *text = NULL;

    pmd85session_tape_get_info(st->session, &t);
    if (!st->have_list || t.list_serial != st->info.list_serial)
        rebuild(st);
    st->info = t;

    for (guint i = 0; i < n_rows(st); i++)
        row_set(row_at(st, i), ROW_PROP_MARKER,
                g_strdup((int)i == t.current ? (t.playing ? "media-playback-start-symbolic"
                                                          : "go-next-symbolic")
                         : (int)i == t.marker ? "media-playback-stop-symbolic" : ""));

    gtk_stack_set_visible_child_name(st->stack, t.blocks > 0 ? "list" : "empty");

    name = strrchr(t.path, '/');
    name = name ? name + 1 : t.path;
    sub = g_strdup_printf("%s%s", t.blocks == 0 ? "No tape" : *name ? name : "New tape",
                          t.recorded ? " \xe2\x80\xa2 not saved" : "");
    adw_window_title_set_subtitle(st->title, sub);

    gtk_button_set_icon_name(GTK_BUTTON(st->play), t.playing ? "media-playback-stop-symbolic"
                                                             : "media-playback-start-symbolic");
    gtk_widget_set_tooltip_text(st->play, t.playing ? "Stop" : "Play");
    if (t.playing && t.progress_max > 0) {
        gtk_progress_bar_set_fraction(st->progress, (double)t.progress / t.progress_max);
        text = st->hex ? g_strdup_printf("%s  %04X / %04X", t.block, t.progress, t.progress_max)
                       : g_strdup_printf("%s  %d / %d", t.block, t.progress, t.progress_max);
    } else {
        gtk_progress_bar_set_fraction(st->progress, 0);
        text = t.blocks > 0 ? g_strdup_printf("%s  (%d of %d)", t.block, t.current + 1, t.blocks)
                            : g_strdup("");
    }
    gtk_progress_bar_set_text(st->progress, text);
    sync_options(st);
    update_actions(st);
}

static gboolean tick(gpointer data)
{
    refresh(data);
    return G_SOURCE_CONTINUE;
}

/* ---- transport ------------------------------------------------------------ */

static void on_play(GtkButton *b, gpointer data)
{
    TapeState *st = data;
    (void)b;
    result(st, st->info.playing ? pmd85session_tape_stop(st->session)
                                : pmd85session_tape_play(st->session));
}

static void on_prev(GtkButton *b, gpointer data)
{
    TapeState *st = data;
    (void)b;
    result(st, pmd85_window_tape_step(st->main, -1));
}

static void on_next(GtkButton *b, gpointer data)
{
    TapeState *st = data;
    (void)b;
    result(st, pmd85_window_tape_step(st->main, 1));
}

static void on_rewind(GtkButton *b, gpointer data)
{
    TapeState *st = data;
    (void)b;
    result(st, pmd85session_tape_rewind(st->session));
}

/* double-click or Enter: the tape's position to that file */
static void on_activate(GtkColumnView *view, guint pos, gpointer data)
{
    TapeState *st = data;
    (void)view;
    if (st->info.playing)
        pmd85session_tape_stop(st->session);
    result(st, pmd85session_tape_seek(st->session, (int)pos));
}

/* ---- actions (menus and keys) -------------------------------------------- */

#define ACTION(name) static void act_##name(GSimpleAction *a, GVariant *p, gpointer data)

ACTION(seek)
{
    TapeState *st = data;
    (void)a; (void)p;
    result(st, pmd85session_tape_seek(st->session, target_row(st)));
}

ACTION(mark)
{
    TapeState *st = data;
    (void)a; (void)p;
    result(st, pmd85session_tape_mark(st->session, target_row(st)));
}

ACTION(unmark)
{
    TapeState *st = data;
    (void)a; (void)p;
    result(st, pmd85session_tape_mark(st->session, -1));
}

ACTION(delete)
{
    TapeState *st = data;
    int n = 0, rc;
    g_autofree int *sel = selected_rows(st, &n);
    const int row = target_row(st);
    (void)a; (void)p;
    rc = pmd85session_tape_delete(st->session, row, sel, n);
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(st->sel));
    result(st, rc);
    if (rc == 0) {
        /* the row after the deleted ones, ready for the next */
        const int at = n > 0 ? sel[0] : row;
        const int rows = (int)n_rows(st);
        if (rows > 0)
            select_range(st, at < rows ? at : rows - 1, at < rows ? at : rows - 1);
    }
}

static void move(TapeState *st, gboolean up)
{
    int n = 0, rc, first, last;
    g_autofree int *sel = selected_rows(st, &n);
    const int row = target_row(st);
    first = n > 0 ? sel[0] : row;
    last = n > 0 ? sel[n - 1] : row;
    rc = pmd85session_tape_move(st->session, up, row, sel, n);
    result(st, rc);
    if (rc == 0) {
        const int d = up ? -1 : 1;
        select_range(st, first + d, last + d);
        st->menu_row = -1;
        gtk_column_view_scroll_to(st->view, (guint)(up ? first + d : last + d), NULL,
                                  GTK_LIST_SCROLL_FOCUS, NULL);
    }
}

ACTION(up)
{
    (void)a; (void)p;
    move(data, TRUE);
}

ACTION(down)
{
    (void)a; (void)p;
    move(data, FALSE);
}

ACTION(select_all)
{
    TapeState *st = data;
    (void)a; (void)p;
    gtk_selection_model_select_all(GTK_SELECTION_MODEL(st->sel));
}

ACTION(load)
{
    TapeState *st = data;
    (void)a; (void)p;
    pmd85_window_tape_open(st->main, st->win, FALSE);
}

ACTION(append)
{
    TapeState *st = data;
    (void)a; (void)p;
    pmd85_window_tape_open(st->main, st->win, TRUE);
}

ACTION(save)
{
    TapeState *st = data;
    (void)a; (void)p;
    pmd85_window_tape_save(st->main, st->win);
}

ACTION(eject)
{
    TapeState *st = data;
    (void)a; (void)p;
    pmd85_window_tape_eject(st->main, GTK_WIDGET(st->win));
}

static void on_hex(GSimpleAction *a, GVariant *v, gpointer data)
{
    TapeState *st = data;
    st->hex = g_variant_get_boolean(v);
    g_simple_action_set_state(a, v);
    pmd85session_set_int(st->session, "tape_hex", st->hex);
    for (guint i = 0; i < n_rows(st); i++) {
        Pmd85TapeRow *r = row_at(st, i);
        row_set(r, ROW_PROP_START, fmt_start(st, r->block.start));
        row_set(r, ROW_PROP_LENGTH, fmt_length(st, r->block.length));
    }
    refresh(st);
}

static void on_autostop(GSimpleAction *a, GVariant *v, gpointer data)
{
    TapeState *st = data;
    g_simple_action_set_state(a, v);
    pmd85session_set_tape_autostop(st->session, g_variant_get_int32(v));
}

static void on_option(GSimpleAction *a, GVariant *v, gpointer data)
{
    TapeState *st = data;
    const gboolean on = g_variant_get_boolean(v);
    int flash = pmd85session_get_int(st->session, "tape_flash", 1);
    int sound = pmd85session_get_int(st->session, "tape_sound", 0);
    g_simple_action_set_state(a, v);
    if (!strcmp(g_action_get_name(G_ACTION(a)), "flash"))
        flash = on;
    else
        sound = on;
    pmd85session_set_tape_options(st->session, flash, sound);
    sync_options(st);
}

/* ---- the list's cells ----------------------------------------------------- */

static void cell_pressed(GtkGestureClick *g, int n_press, double x, double y, gpointer data)
{
    TapeState *st = data;
    GtkWidget *cell = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    GtkListItem *item = g_object_get_data(G_OBJECT(cell), "pmd85-item");
    graphene_point_t at;
    guint pos;
    (void)n_press;

    if (!item || (pos = gtk_list_item_get_position(item)) == GTK_INVALID_LIST_POSITION)
        return;
    if (!gtk_selection_model_is_selected(GTK_SELECTION_MODEL(st->sel), pos))
        gtk_selection_model_select_item(GTK_SELECTION_MODEL(st->sel), pos, TRUE);
    st->menu_row = (int)pos;
    update_actions(st);
    if (!gtk_widget_compute_point(cell, GTK_WIDGET(st->view),
                                  &GRAPHENE_POINT_INIT((float)x, (float)y), &at))
        return;
    gtk_popover_set_pointing_to(GTK_POPOVER(st->popover),
                                &(GdkRectangle){ (int)at.x, (int)at.y, 1, 1 });
    gtk_popover_popup(GTK_POPOVER(st->popover));
}

static void popover_closed(GtkPopover *p, gpointer data)
{
    TapeState *st = data;
    (void)p;
    st->menu_row = -1;
    update_actions(st);
}

typedef enum { COL_MARKER, COL_NAME, COL_START, COL_LENGTH, COL_BAD } Column;

static void cell_setup(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
    TapeState *st = data;
    const Column col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(f), "pmd85-col"));
    GtkWidget *w;
    GtkGesture *g;

    if (col == COL_MARKER || col == COL_BAD) {
        w = gtk_image_new();
    } else {
        w = gtk_label_new(NULL);
        gtk_label_set_xalign(GTK_LABEL(w), col == COL_NAME ? 0.0f : 1.0f);
        gtk_widget_add_css_class(w, "monospace");
    }
    gtk_widget_set_hexpand(w, TRUE);
    g_object_set_data(G_OBJECT(w), "pmd85-item", item);
    g = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), GDK_BUTTON_SECONDARY);
    g_signal_connect(g, "pressed", G_CALLBACK(cell_pressed), st);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(g));
    gtk_list_item_set_child(item, w);
}

static void cell_bind(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
    const Column col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(f), "pmd85-col"));
    Pmd85TapeRow *r = gtk_list_item_get_item(item);
    GtkWidget *w = gtk_list_item_get_child(item);
    GBinding *b = NULL;
    (void)data;

    switch (col) {
    case COL_MARKER:
        b = g_object_bind_property(r, "marker", w, "icon-name", G_BINDING_SYNC_CREATE);
        break;
    case COL_NAME:
        gtk_label_set_text(GTK_LABEL(w), r->block.name);
        break;
    case COL_START:
        b = g_object_bind_property(r, "start", w, "label", G_BINDING_SYNC_CREATE);
        break;
    case COL_LENGTH:
        b = g_object_bind_property(r, "length", w, "label", G_BINDING_SYNC_CREATE);
        break;
    case COL_BAD:
        gtk_image_set_from_icon_name(GTK_IMAGE(w), r->block.bad ? "dialog-warning-symbolic" : NULL);
        gtk_widget_set_tooltip_text(w, r->block.bad ? "CRC or length error" : NULL);
        break;
    }
    g_object_set_data(G_OBJECT(w), "pmd85-binding", b);
}

static void cell_unbind(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
    GtkWidget *w = gtk_list_item_get_child(item);
    GBinding *b = g_object_get_data(G_OBJECT(w), "pmd85-binding");
    (void)f;
    (void)data;
    if (b) g_binding_unbind(b);
    g_object_set_data(G_OBJECT(w), "pmd85-binding", NULL);
}

static void add_column(TapeState *st, const char *title, Column col, gboolean expand)
{
    GtkListItemFactory *f = gtk_signal_list_item_factory_new();
    GtkColumnViewColumn *c;
    g_object_set_data(G_OBJECT(f), "pmd85-col", GINT_TO_POINTER(col));
    g_signal_connect(f, "setup", G_CALLBACK(cell_setup), st);
    g_signal_connect(f, "bind", G_CALLBACK(cell_bind), st);
    g_signal_connect(f, "unbind", G_CALLBACK(cell_unbind), st);
    c = gtk_column_view_column_new(title, f);
    gtk_column_view_column_set_expand(c, expand);
    gtk_column_view_append_column(st->view, c);
    g_object_unref(c);
}

/* ---- building it ---------------------------------------------------------- */

static GMenuModel *header_menu(void)
{
    GMenu *menu = g_menu_new(), *file = g_menu_new(), *view = g_menu_new(),
          *stop = g_menu_new();
    GMenuItem *it;

    g_menu_append(file, "Append Tape\xe2\x80\xa6", "tape.append");
    g_menu_append(file, "Save Tape As\xe2\x80\xa6", "tape.save");
    g_menu_append(file, "Eject", "tape.eject");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(file));

    it = g_menu_item_new("At the Next File", NULL);
    g_menu_item_set_action_and_target(it, "tape.autostop", "i", PMD85SESSION_TAPE_STOP_HEADER);
    g_menu_append_item(stop, it);
    g_object_unref(it);
    it = g_menu_item_new("At the STOP Marker", NULL);
    g_menu_item_set_action_and_target(it, "tape.autostop", "i", PMD85SESSION_TAPE_STOP_MARKER);
    g_menu_append_item(stop, it);
    g_object_unref(it);
    it = g_menu_item_new("At the Tape's End", NULL);
    g_menu_item_set_action_and_target(it, "tape.autostop", "i", PMD85SESSION_TAPE_STOP_END);
    g_menu_append_item(stop, it);
    g_object_unref(it);
    g_menu_append_submenu(view, "Playing Stops", G_MENU_MODEL(stop));
    g_menu_append(view, "Hexadecimal Numbers", "tape.hex");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(view));


    g_object_unref(file);
    g_object_unref(view);
    g_object_unref(stop);
    return G_MENU_MODEL(menu);
}

static GMenuModel *row_menu(void)
{
    GMenu *menu = g_menu_new(), *pos = g_menu_new(), *edit = g_menu_new(),
          *sel = g_menu_new();
    g_menu_append(pos, "Set Tape Position", "tape.seek");
    g_menu_append(pos, "Set STOP Marker", "tape.mark");
    g_menu_append(pos, "Clear STOP Marker", "tape.unmark");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(pos));
    g_menu_append(edit, "Move Up", "tape.up");
    g_menu_append(edit, "Move Down", "tape.down");
    g_menu_append(edit, "Delete", "tape.delete");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(edit));
    g_menu_append(sel, "Select All", "tape.select-all");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(sel));
    g_object_unref(pos);
    g_object_unref(edit);
    g_object_unref(sel);
    return G_MENU_MODEL(menu);
}

static void add_shortcut(GtkShortcutController *c, const char *accel, const char *action)
{
    gtk_shortcut_controller_add_shortcut(c,
        gtk_shortcut_new(gtk_shortcut_trigger_parse_string(accel),
                         gtk_named_action_new(action)));
}

static GtkWidget *icon_button(const char *icon, const char *tip, GCallback cb, TapeState *st)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon);
    gtk_widget_set_tooltip_text(b, tip);
    g_signal_connect(b, "clicked", cb, st);
    return b;
}

static void destroyed(GtkWidget *w, gpointer data)
{
    TapeState *st = data;
    (void)w;
    g_source_remove(st->timer);
    if (st->popover)
        gtk_widget_unparent(st->popover);
    g_object_unref(st->actions);
    if (g_tape == st)
        g_tape = NULL;
    g_free(st);
}

void pmd85_tape_window_show(Pmd85Window *main, pmd85session *session)
{
    TapeState *st;
    GtkWidget *tb, *header, *box, *bar, *scroll, *empty, *menu_button, *load;
    GtkShortcutController *keys;
    GMenuModel *model;
    const GActionEntry entries[] = {
        { .name = "seek", .activate = act_seek },
        { .name = "mark", .activate = act_mark },
        { .name = "unmark", .activate = act_unmark },
        { .name = "delete", .activate = act_delete },
        { .name = "up", .activate = act_up },
        { .name = "down", .activate = act_down },
        { .name = "select-all", .activate = act_select_all },
        { .name = "load", .activate = act_load },
        { .name = "append", .activate = act_append },
        { .name = "save", .activate = act_save },
        { .name = "eject", .activate = act_eject },
    };

    if (g_tape) {
        gtk_window_present(g_tape->win);
        return;
    }
    st = g_new0(TapeState, 1);
    g_tape = st;
    st->main = main;
    st->session = session;
    st->menu_row = -1;
    st->hex = pmd85session_get_int(session, "tape_hex", 1) != 0;   /* as the monitor */

    st->win = GTK_WINDOW(adw_window_new());
    gtk_window_set_title(st->win, "Tape");
    gtk_window_set_default_size(st->win, 440, 560);
    gtk_window_set_transient_for(st->win, GTK_WINDOW(main));
    gtk_window_set_destroy_with_parent(st->win, TRUE);
    gtk_window_set_application(st->win, gtk_window_get_application(GTK_WINDOW(main)));

    st->actions = g_simple_action_group_new();
    g_action_map_add_action_entries(G_ACTION_MAP(st->actions), entries, G_N_ELEMENTS(entries), st);
    {
        GSimpleAction *a;
        a = g_simple_action_new_stateful("hex", NULL, g_variant_new_boolean(st->hex));
        g_signal_connect(a, "change-state", G_CALLBACK(on_hex), st);
        g_action_map_add_action(G_ACTION_MAP(st->actions), G_ACTION(a));
        g_object_unref(a);
        a = g_simple_action_new_stateful("autostop", G_VARIANT_TYPE_INT32,
                g_variant_new_int32(pmd85session_tape_autostop(session)));
        g_signal_connect(a, "change-state", G_CALLBACK(on_autostop), st);
        g_action_map_add_action(G_ACTION_MAP(st->actions), G_ACTION(a));
        g_object_unref(a);
        a = g_simple_action_new_stateful("flash", NULL,
                g_variant_new_boolean(pmd85session_get_int(session, "tape_flash", 1) != 0));
        g_signal_connect(a, "change-state", G_CALLBACK(on_option), st);
        g_action_map_add_action(G_ACTION_MAP(st->actions), G_ACTION(a));
        g_object_unref(a);
        a = g_simple_action_new_stateful("sound", NULL,
                g_variant_new_boolean(pmd85session_get_int(session, "tape_sound", 0) != 0));
        g_signal_connect(a, "change-state", G_CALLBACK(on_option), st);
        g_action_map_add_action(G_ACTION_MAP(st->actions), G_ACTION(a));
        g_object_unref(a);
    }
    gtk_widget_insert_action_group(GTK_WIDGET(st->win), "tape", G_ACTION_GROUP(st->actions));

    /* header: Load, the title (the tape's file), the menu */
    header = adw_header_bar_new();
    st->title = ADW_WINDOW_TITLE(adw_window_title_new("Tape", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(st->title));
    load = gtk_button_new_from_icon_name("document-open-symbolic");
    gtk_widget_set_tooltip_text(load, "Load Tape\xe2\x80\xa6 (Ctrl+O)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(load), "tape.load");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), load);
    menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
    model = header_menu();
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), model);
    g_object_unref(model);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_button);

    /* transport: Play/Stop, previous/next file, Rewind, the progress */
    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(bar, 12);
    gtk_widget_set_margin_end(bar, 12);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
    st->play = icon_button("media-playback-start-symbolic", "Play", G_CALLBACK(on_play), st);
    gtk_widget_add_css_class(st->play, "pmd85-tape-play");
    {   /* the PMD's key blue, as the debugger's Run (white on it at 4.6:1) */
        static gboolean css;
        if (!css) {
            GtkCssProvider *p = gtk_css_provider_new();
            gtk_css_provider_load_from_string(p,
                "button.pmd85-tape-play { background-image: none; background-color: #557992;"
                " color: #ffffff; }"
                " button.pmd85-tape-play:hover { background-color: #6894b2; }"
                " button.pmd85-tape-play:disabled { background-color: alpha(#557992, 0.5); }");
            gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(p);
            css = TRUE;
        }
    }
    st->rewind = icon_button("media-seek-backward-symbolic", "Rewind to the first file",
                             G_CALLBACK(on_rewind), st);
    st->prev = icon_button("media-skip-backward-symbolic", "Previous file (data blocks passed over)",
                           G_CALLBACK(on_prev), st);
    st->next = icon_button("media-skip-forward-symbolic", "Next file (data blocks passed over)",
                           G_CALLBACK(on_next), st);
    {
        GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(nav, "linked");
        gtk_box_append(GTK_BOX(nav), st->rewind);
        gtk_box_append(GTK_BOX(nav), st->prev);
        gtk_box_append(GTK_BOX(nav), st->next);
        gtk_box_append(GTK_BOX(bar), st->play);
        gtk_box_append(GTK_BOX(bar), nav);
    }
    st->progress = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_progress_bar_set_show_text(st->progress, TRUE);
    gtk_progress_bar_set_ellipsize(st->progress, PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(GTK_WIDGET(st->progress), TRUE);
    gtk_widget_set_valign(GTK_WIDGET(st->progress), GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(st->progress));
    {   /* the tape options, as toggles */
        GtkWidget *opts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(opts, "linked");
        st->flash = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(st->flash), "thunderbolt-symbolic");
        gtk_widget_set_tooltip_text(st->flash, "Flash load: a file's body goes into memory "
                                    "at once (its header still plays in real time)");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(st->flash), "tape.flash");
        st->sound = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(st->sound), "audio-volume-muted-symbolic");
        gtk_widget_set_tooltip_text(st->sound, "Tape sound: the tape's signal on the speaker "
                                    "while it plays");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(st->sound), "tape.sound");
        gtk_box_append(GTK_BOX(opts), st->flash);
        gtk_box_append(GTK_BOX(opts), st->sound);
        gtk_box_append(GTK_BOX(bar), opts);
    }

    /* the files */
    /* each takes the one before (transfer full); the view owns them all */
    st->store = g_list_store_new(PMD85_TYPE_TAPE_ROW);
    st->sel = gtk_multi_selection_new(G_LIST_MODEL(st->store));
    st->view = GTK_COLUMN_VIEW(gtk_column_view_new(GTK_SELECTION_MODEL(st->sel)));
    gtk_column_view_set_show_column_separators(st->view, FALSE);
    gtk_widget_add_css_class(GTK_WIDGET(st->view), "data-table");
    add_column(st, "", COL_MARKER, FALSE);
    add_column(st, "File", COL_NAME, TRUE);
    add_column(st, "Start", COL_START, FALSE);
    add_column(st, "Length", COL_LENGTH, FALSE);
    add_column(st, "", COL_BAD, FALSE);
    g_signal_connect(st->view, "activate", G_CALLBACK(on_activate), st);
    model = row_menu();
    st->popover = gtk_popover_menu_new_from_model(model);
    g_object_unref(model);
    gtk_widget_set_parent(st->popover, GTK_WIDGET(st->view));
    gtk_popover_set_has_arrow(GTK_POPOVER(st->popover), FALSE);
    gtk_widget_set_halign(st->popover, GTK_ALIGN_START);
    g_signal_connect(st->popover, "closed", G_CALLBACK(popover_closed), st);
    g_signal_connect_swapped(st->sel, "selection-changed", G_CALLBACK(update_actions), st);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(st->view));
    gtk_widget_set_vexpand(scroll, TRUE);

    empty = adw_status_page_new();
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "media-tape-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(empty), "No Tape");
    adw_status_page_set_description(ADW_STATUS_PAGE(empty),
        "Load a .ptp or .pmd tape, or save to tape on the PMD (MGSV) to record one.");
    {
        GtkWidget *b = gtk_button_new_with_mnemonic("_Load Tape\xe2\x80\xa6");
        gtk_widget_add_css_class(b, "pill");
        gtk_widget_set_halign(b, GTK_ALIGN_CENTER);
        gtk_actionable_set_action_name(GTK_ACTIONABLE(b), "tape.load");
        adw_status_page_set_child(ADW_STATUS_PAGE(empty), b);
    }

    st->stack = GTK_STACK(gtk_stack_new());
    gtk_stack_add_named(st->stack, scroll, "list");
    gtk_stack_add_named(st->stack, empty, "empty");
    gtk_widget_set_vexpand(GTK_WIDGET(st->stack), TRUE);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(st->stack));
    st->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(st->toasts, box);

    tb = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tb), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tb), GTK_WIDGET(st->toasts));
    adw_window_set_content(ADW_WINDOW(st->win), tb);

    keys = GTK_SHORTCUT_CONTROLLER(gtk_shortcut_controller_new());
    gtk_shortcut_controller_set_scope(keys, GTK_SHORTCUT_SCOPE_MANAGED);
    add_shortcut(keys, "Delete", "tape.delete");
    add_shortcut(keys, "<Alt>Up", "tape.up");
    add_shortcut(keys, "<Alt>Down", "tape.down");
    add_shortcut(keys, "<Control>o", "tape.load");
    add_shortcut(keys, "<Control>s", "tape.save");
    add_shortcut(keys, "Escape", "window.close");
    gtk_widget_add_controller(GTK_WIDGET(st->win), GTK_EVENT_CONTROLLER(keys));

    g_signal_connect(st->win, "destroy", G_CALLBACK(destroyed), st);
    refresh(st);
    st->timer = g_timeout_add(100, tick, st);
    gtk_window_present(st->win);
}
