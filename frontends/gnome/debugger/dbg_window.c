/*
 * Debugger window (GTK4/libadwaita) over the session's 8080 debugger engine,
 * core/include/pmd85debug.h. Derived from fujinet-go-coco-desktop's window
 * (the family's shape), with the PMD's I/O in place of the CoCo's.
 *
 * Tabs (PMD85_DEBUGGER_TAB takes the number, or the start of the name):
 *   0 Console          the engine's command set, Tab completion, history
 *   1 Disassembly      breakpoint gutter, follow-PC, jump box, selectable row;
 *                      Intel 8080 or Z80 mnemonics, hex as C000h/$C000/0xC000/#C000
 *   2 Memory           the CPU's 64K as mapped now, go-to (a range is
 *                      highlighted), region labels, search (hex or text), poke
 *   3 Breakpoints      why it stopped; execute, read, write, access, IN, OUT,
 *                      IN/OUT on ports, conditions, hit counts
 *   4 Watches          addresses, symbols and ranges with their live values
 *   5 I/O              the memory map, the 8255s, the 8253, the 8251, the
 *                      modules and the Becker link; the I/O log beside it
 *   6 Trace            the instruction ring, live while the machine runs
 *   7 History          the same ring, read when the machine stops
 *   8 Symbols          every known symbol (the monitor's, and loaded ones)
 *   9 FujiNet          every DriveWire packet, with the Becker port's state
 * The registers (PC SP A F BC DE HL, the flags S Z AC P CY) sit under the
 * tabs on every tab and are editable while the machine is stopped. Any tab
 * can be popped out into a window of its own (Pop Out, then Dock; closing
 * that window docks it); one tab always stays in the main window. Keys: F5
 * run/stop, F7 step, F8 step over, Shift+F8 step out, F9 breakpoint on the
 * selected line, F12 close.
 *
 * The engine is engaged only while this window is showing: showing it
 * engages and stops the machine, hiding it releases the engine and lets the
 * machine run on at full speed. The window polls the engine's generation
 * counter on a short timer and refreshes only the tabs in front.
 * Environment hooks read at creation: PMD85_DEBUGGER_TAB, PMD85_WATCH,
 * PMD85_WATCH_BREAK, PMD85_MEM_GOTO, PMD85_HISTORY, PMD85_POPOUT.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbg_window.h"

#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>

#include "pmd85debug.h"
#include "pmd85memtools.h"
#include "../window.h"

/* The PMD has one address space: the CoCo window's PHYS controls exist but
 * stay hidden (the engine gives a second space no size). */
#define PMD85DEBUG_SPACE_PHYS 1
#define PMD85DEBUG_BP_PHYS 0

/* The blue of the PMD 85's K0-K11 keys (#6894B2, as in the app's icon) with
 * white on it, as on the keys -- dimmed to 82 % so the white reads at 4.6:1
 * in either theme. KEY_RGB itself is the accent as text on a dark theme. */
#define ACCENT_RGB 0x557992u
#define ACCENT_INK_RGB 0xFFFFFFu
#define KEY_RGB 0x6894B2u

#define DISASM_WINDOW 48
#define MEM_ROWS 32
#define MEM_PAGE (MEM_ROWS * 16)
#define POKE_MAX 64
#define TRACE_MAX 512
#define HIST_SHOWN 2000   /* lines on a Trace or History page */
#define HIST_READ 65536   /* the ring */
#define NTABS 10
#define NREGS 7          /* the registers shown: PC SP A F BC DE HL */
#define NFLAGS 5
#define IOLOG_READ 4096
#define SYMBOLS_SHOWN 2000   /* rows in the Symbols list */

typedef struct {
    GtkWindow *win;
    pmd85session *session;
    pmd85debug *dbg;
    unsigned seen_generation;
    gboolean was_stopped;
    gboolean closing;         /* the window is being destroyed: touch nothing */
    int running_ticks;
    guint timer;

    AdwWindowTitle *title;    /* "Debugger", with the stop reason under it */
    GtkButton *run_btn;
    GtkDropDown *syntax, *hexstyle;
    AdwToastOverlay *toasts;
    GtkNotebook *notebook;

    /* console */
    GtkTextView *console_out;
    GtkEntry *console_in;
    GPtrArray *history;
    guint history_pos;

    /* the registers strip */
    GtkEntry *reg[NREGS];
    GtkWidget *reg_box[NREGS];
    GtkCheckButton *flag[NFLAGS]; /* S Z AC P CY */
    GtkLabel *machine;
    gboolean flag_sync;           /* the boxes are being set from the engine */
    gboolean force_entries;       /* a refresh may overwrite a focused entry */

    /* memory */
    GtkTextView *mem_view;
    GtkEntry *mem_goto, *mem_write;
    GtkDropDown *mem_space;
    GtkWidget *mem_space_box;
    uint32_t mem_base[2];
    gboolean mem_hl;          /* a range is highlighted (Go to, a search hit, a watch) */
    int mem_hl_space;
    uint32_t mem_hl_lo, mem_hl_hi;
    GtkDropDown *srch_kind;
    GtkEntry *srch_entry;
    GtkLabel *srch_note;

    /* tabs: the page widget of each, and the window it is popped out into */
    GtkWidget *pages[NTABS];
    GtkWindow *detached[NTABS];
    uint64_t seen_serial;     /* the stop serial last acted on */

    /* disassembly */
    GtkTextView *disasm;
    GtkCheckButton *follow_pc;
    GtkEntry *jump;
    int disasm_top;           /* address of the first instruction */
    int line_addr[DISASM_WINDOW * 2];   /* per buffer line; -1 for a label line */
    int line_count;
    int selected;             /* the selected row's address, -1 none */

    /* breakpoints */
    GtkListBox *bp_list;
    GtkDropDown *bp_type;
    GtkCheckButton *bp_phys;
    GtkEntry *bp_start, *bp_end, *bp_cond;
    GtkEntry *break_at;       /* the toolbar's "Break at" box */
    GtkLabel *bp_msg;
    GtkLabel *bp_stop;        /* "Stopped: ..." / "Running." */
    GString *bp_sig;          /* what the list shows, to skip a needless rebuild */
    gboolean bp_sig_reset;    /* the hex style changed: rebuild it */

    /* watches */
    GtkEntry *wt_entry;
    GtkDropDown *wt_space;
    GtkListBox *wt_list;
    GPtrArray *wt_labels;     /* a GtkLabel per row, rewritten in place */
    unsigned wt_gen;
    gboolean wt_built;

    /* i/o */
    GtkTextView *io_text, *io_log;
    GtkCheckButton *io_on;
    GtkEntry *io_filter;
    GtkLabel *io_note;
    gboolean io_sync;
    uint64_t io_sig;

    /* symbols */
    GtkEntry *sym_filter;
    GtkListBox *sym_list;
    GtkLabel *sym_note;
    GtkLabel *sym_info;       /* which monitor's symbols are built in */
    GString *sym_sig;         /* what the list shows, to skip a needless rebuild */

    /* trace (0) and history (1): the same ring, two views */
    GtkToggleButton *h_rec[2];
    GtkDropDown *h_filter[2];
    GtkEntry *h_range[2];
    GtkLabel *h_note[2];
    GtkTextView *h_text[2];
    uint64_t h_sig[2];        /* what each page last drew; ~0 forces a redraw */
    gboolean h_sync;          /* the Record toggles are being set from the engine */
    int trace_ticks;

    /* fujinet */
    GtkLabel *fuji_note;
    GtkTextView *fuji_text, *fuji_state;
    GtkCheckButton *fuji_hide_ready;
    uint64_t fuji_last;
    int fuji_count;
} DbgWin;

static DbgWin *g_win;

/* The register entries: the engine's register, its name and hex digits. */
static const struct { const char *name; int digits; int reg; } k_regs[NREGS] = {
    { "PC", 4, PMD85DEBUG_REG_PC }, { "SP", 4, PMD85DEBUG_REG_SP }, { "A", 2, PMD85DEBUG_REG_A },
    { "F", 2, PMD85DEBUG_REG_F }, { "BC", 4, PMD85DEBUG_REG_BC }, { "DE", 4, PMD85DEBUG_REG_DE },
    { "HL", 4, PMD85DEBUG_REG_HL },
};

/* The F bits as the strip shows them. */
static const struct { const char *name; unsigned mask; } k_flags[NFLAGS] = {
    { "S", PMD85DEBUG_F_S }, { "Z", PMD85DEBUG_F_Z }, { "AC", PMD85DEBUG_F_AC },
    { "P", PMD85DEBUG_F_P }, { "CY", PMD85DEBUG_F_CY },
};

/* The tabs, in order; PMD85_DEBUGGER_TAB takes a number or the start of a name. */
static const char *const k_tab_names[NTABS] = {
    "Console", "Disassembly", "Memory", "Breakpoints", "Watches", "I/O", "Trace", "History",
    "Symbols", "FujiNet",
};
enum { TAB_CONSOLE, TAB_DISASM, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_IO, TAB_TRACE,
       TAB_HISTORY, TAB_SYMBOLS, TAB_FUJINET };

/* The Memory tab's drop-down, and the engine's spaces behind it. */
static const char *const k_space_labels[3] = { "CPU", "PHYS", NULL };

/* ---- helpers -------------------------------------------------------------- */

/* Engine text for a text buffer: valid UTF-8, control bytes dropped. */
static char *clean_text(const char *text)
{
    char *value = g_utf8_make_valid(text, -1), *source, *dest;
    for (source = dest = value; *source; source++)
        if ((unsigned char)*source >= 0x20 || *source == '\n' || *source == '\t') *dest++ = *source;
    *dest = '\0';
    return value;
}

static GtkAdjustment *vadj(GtkTextView *view)
{
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(view));
    return GTK_IS_SCROLLED_WINDOW(parent)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent)) : NULL;
}

/* A log (the console, the FujiNet packets) follows its end while its
 * reader is there. New text grows the view only once it is laid out (or its
 * tab first shows), so the end is pinned whenever the view's height
 * changes; whether to follow is decided as text arrives. */
static gboolean following(GtkTextView *view)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), "follow")) != 0;
}

static void on_log_resized(GtkAdjustment *adj, gpointer view)
{
    if (following(view))
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) -
                                      gtk_adjustment_get_page_size(adj));
}

static void make_log(GtkTextView *view)
{
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    /* tied to the view: the adjustment can outlive it while the window dies */
    g_signal_connect_object(vadj(view), "changed", G_CALLBACK(on_log_resized), view, 0);
}

/* Before new text: follow if the reader is at the end (or never looked). */
static void log_note_reader(GtkTextView *view)
{
    GtkAdjustment *adj = vadj(view);
    g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(
        gtk_adjustment_get_page_size(adj) <= 0.0 ||
        gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >=
            gtk_adjustment_get_upper(adj) - 2.0));
}

/* Replaces a view's text where the reader left it: a refresh (a step, or
 * the live values twice a second) must not throw a scrolled dump back to
 * its top. The top line is noted and scrolled back to once the new text is
 * laid out (scroll_to_mark waits for that). A log that is following goes
 * to its end instead. */
static void set_text_at(GtkTextView *view, const char *text, gboolean follow)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
    GtkAdjustment *adj = vadj(view);
    char *clean = clean_text(text);
    GtkTextIter iter;
    int top = 0;

    if (!follow && adj && gtk_adjustment_get_value(adj) > 0.0) {
        gtk_text_view_get_line_at_y(view, &iter, (int)gtk_adjustment_get_value(adj), NULL);
        top = gtk_text_iter_get_line(&iter);
    }
    {
        GtkTextIter iter_start, iter_end;
        char *old;
        gboolean same;

        /* unchanged text is not set again: it would reset the selection and
         * relayout for nothing */
        gtk_text_buffer_get_bounds(buffer, &iter_start, &iter_end);
        old = gtk_text_buffer_get_text(buffer, &iter_start, &iter_end, FALSE);
        same = strcmp(old, clean) == 0;
        g_free(old);
        if (same) {
            g_free(clean);
            if (follow) {
                g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
                if (adj) on_log_resized(adj, view);
            }
            return;
        }
    }
    gtk_text_buffer_set_text(buffer, clean, -1);
    g_free(clean);
    if (follow) {
        g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
        if (adj) on_log_resized(adj, view);
        return;
    }
    if (top > 0) {
        GtkTextMark *mark;
        gtk_text_buffer_get_iter_at_line(buffer, &iter, top);
        mark = gtk_text_buffer_create_mark(buffer, NULL, &iter, TRUE);
        gtk_text_view_scroll_to_mark(view, mark, 0.0, TRUE, 0.0, 0.0);
        gtk_text_buffer_delete_mark(buffer, mark);
    }
}

static void set_text(GtkTextView *view, const char *text)
{
    set_text_at(view, text, FALSE);
}

static void append_text(GtkTextView *view, const char *text, gboolean force_follow)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
    GtkTextIter end;
    char *clean = clean_text(text);
    log_note_reader(view);
    if (force_follow)
        g_object_set_data(G_OBJECT(view), "follow", GINT_TO_POINTER(1));
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, clean, -1);
    g_free(clean);
}

/* A number as typed: hex by default; $, #, 0x or a trailing h say so too. */
static int parse_num(const char *text, long *out)
{
    char buf[64], *end;
    size_t count;
    long value;
    while (*text == ' ') text++;
    g_strlcpy(buf, text, sizeof buf);
    count = strlen(buf);
    while (count && buf[count - 1] == ' ') buf[--count] = '\0';
    if (!count) return 0;
    if (buf[0] == '$') value = strtol(buf + 1, &end, 16);
    else if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) value = strtol(buf + 2, &end, 16);
    else if (buf[0] == '#') value = strtol(buf + 1, &end, 16);
    else if (buf[count - 1] == 'h' || buf[count - 1] == 'H') { buf[count - 1] = '\0'; value = strtol(buf, &end, 16); }
    else value = strtol(buf, &end, 16);
    if (end == buf || *end) return 0;
    *out = value;
    return 1;
}

/* An address box: a symbol (as the engine does, a name wins over a hex
 * literal), bare hex, or any expression the console takes ($A000+4, pc, x,
 * [s], label+2). `space` is the engine's PMD85DEBUG_SPACE_*: a PHYS address is
 * up to six hex digits and has no symbols. 0 with the reason in err. */
static int parse_addr(DbgWin *state, const char *text, int space, long *out, char *err, int errsz)
{
    char buf[128];
    long value, limit = space == PMD85DEBUG_SPACE_PHYS
        ? (long)pmd85debug_space_size(state->dbg, space) - 1 : 0xFFFF;
    uint16_t sym;
    g_strlcpy(buf, text, sizeof buf);
    g_strstrip(buf);
    if (!buf[0]) {
        g_strlcpy(err, "Enter an address, a symbol or an expression", errsz);
        return 0;
    }
    if (space == PMD85DEBUG_SPACE_CPU && pmd85debug_symbol_find(state->dbg, buf, &sym)) {
        *out = sym;
        return 1;
    }
    if (parse_num(buf, &value) || (pmd85debug_eval(state->dbg, buf, &value, err, errsz) == 0)) {
        if (value < 0 || value > limit) {
            g_snprintf(err, errsz, "Out of range (0 to %lXh)", limit);
            return 0;
        }
        *out = value;
        return 1;
    }
    if (!err[0])
        g_strlcpy(err, "Not an address, a symbol or an expression", errsz);
    return 0;
}

/* A refresh does not overwrite a value being typed: a focused entry whose
 * text is not what was last put there keeps it, until Enter (force_entries)
 * says to take the engine's. */
static void set_entry(DbgWin *state, GtkEntry *entry, const char *text)
{
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(entry));
    GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
    const char *shown = g_object_get_data(G_OBJECT(entry), "shown");
    if (focus && !state->force_entries && shown &&
        (focus == GTK_WIDGET(entry) || gtk_widget_is_ancestor(focus, GTK_WIDGET(entry))) &&
        strcmp(gtk_editable_get_text(GTK_EDITABLE(entry)), shown) != 0)
        return;
    gtk_editable_set_text(GTK_EDITABLE(entry), text);
    g_object_set_data_full(G_OBJECT(entry), "shown", g_strdup(text), g_free);
}

static GtkWidget *mono_view(GtkTextView **out, gboolean wrap)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_hexpand(scroll, TRUE);
    *out = GTK_TEXT_VIEW(view);
    return scroll;
}

static GtkWidget *labeled(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

/* A heading over a block (the FujiNet pane). */
static GtkWidget *headed(const char *text, GtkWidget *child)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_add_css_class(label, "heading");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), child);
    return box;
}

static GtkWidget *padded(GtkWidget *widget)
{
    gtk_widget_set_margin_top(widget, 8);
    gtk_widget_set_margin_bottom(widget, 8);
    gtk_widget_set_margin_start(widget, 8);
    gtk_widget_set_margin_end(widget, 8);
    return widget;
}

/* A line marked in the accent: white on blue, on either theme. */
static void accent_tag(GtkTextView *view, const char *name)
{
    char rgb[16], ink[16];
    g_snprintf(rgb, sizeof rgb, "#%06x", ACCENT_RGB);
    g_snprintf(ink, sizeof ink, "#%06x", ACCENT_INK_RGB);
    gtk_text_buffer_create_tag(gtk_text_view_get_buffer(view), name,
                               "paragraph-background", rgb, "foreground", ink, NULL);
}

/* Messages from Load Symbols and Save land in the console's transcript and,
 * since the console may not be the tab showing, in a toast. */
static void notice(DbgWin *state, const char *msg)
{
    char line[700];
    g_snprintf(line, sizeof line, "%s\n", msg);
    append_text(state->console_out, line, FALSE);
    adw_toast_overlay_add_toast(state->toasts, adw_toast_new(msg));
}

static void toast(DbgWin *state, const char *msg)
{
    adw_toast_overlay_add_toast(state->toasts, adw_toast_new(msg));
}

/* The step controls wait a moment for the engine to come to rest, so a
 * step does not flash "Running" before the status catches up. */
static void settle(DbgWin *state, gboolean want_stopped)
{
    int index;
    for (index = 0; index < 20 && (pmd85debug_is_paused(state->dbg) != 0) != want_stopped; index++)
        g_usleep(2000);
}

/* ---- refresh -------------------------------------------------------------- */

static void refresh_cpu(DbgWin *state)
{
    pmd85debug_regs regs;
    char buf[200];
    int index;
    const gboolean stopped = pmd85debug_regs_get(state->dbg, &regs) != 0;

    if (stopped) {
        const unsigned vals[NREGS] = {
            regs.pc, regs.sp, regs.a, regs.f, (unsigned)(regs.b << 8 | regs.c),
            (unsigned)(regs.d << 8 | regs.e), (unsigned)(regs.h << 8 | regs.l) };
        for (index = 0; index < NREGS; index++) {
            g_snprintf(buf, sizeof buf, "%0*X", k_regs[index].digits, vals[index]);
            set_entry(state, state->reg[index], buf);
        }
        state->flag_sync = TRUE;
        for (index = 0; index < NFLAGS; index++)
            gtk_check_button_set_active(state->flag[index], (regs.f & k_flags[index].mask) != 0);
        state->flag_sync = FALSE;
        g_snprintf(buf, sizeof buf, "%s, MHB8080A   %s%s",
                   pmd85session_model_name(pmd85session_model(state->session)),
                   regs.iff ? "EI" : "DI", regs.halt ? "   HALT" : "");
        gtk_label_set_text(state->machine, buf);
    } else {
        for (index = 0; index < NREGS; index++)
            set_entry(state, state->reg[index], "");
    }
    for (index = 0; index < NREGS; index++)
        gtk_widget_set_sensitive(GTK_WIDGET(state->reg[index]), stopped);
    for (index = 0; index < NFLAGS; index++)
        gtk_widget_set_sensitive(GTK_WIDGET(state->flag[index]), stopped);
}

/* A tab that reads the machine says so while it runs. */
static gboolean running_note(DbgWin *state, GtkTextView *view, const char *what)
{
    char text[200];
    if (pmd85debug_is_paused(state->dbg)) return FALSE;
    g_snprintf(text, sizeof text, "The machine is running: stop it (F5) to read %s.", what);
    set_text(view, text);
    return TRUE;
}

static int mem_space_now(DbgWin *state)
{
    return gtk_drop_down_get_selected(state->mem_space) == 1 ? PMD85DEBUG_SPACE_PHYS
                                                         : PMD85DEBUG_SPACE_CPU;
}

/* PHYS is offered only on a CoCo 3 (the engine gives it no size otherwise). */
static void sync_space_choice(DbgWin *state)
{
    const gboolean phys = pmd85debug_space_size(state->dbg, PMD85DEBUG_SPACE_PHYS) > 0;
    gtk_widget_set_visible(state->mem_space_box, phys);
    gtk_widget_set_visible(GTK_WIDGET(state->bp_phys), phys);
    gtk_widget_set_visible(GTK_WIDGET(state->wt_space), phys);
    if (!phys) {
        gtk_drop_down_set_selected(state->mem_space, 0);
        gtk_drop_down_set_selected(state->wt_space, 0);
        gtk_check_button_set_active(state->bp_phys, FALSE);
    }
}

/* The dump: a row is the address, then 3 columns per byte, then the text. The
 * highlighted range gets the accent background on its bytes (hex and text),
 * with the gap between two highlighted bytes filled so it reads as a bar. */
static void refresh_mem(DbgWin *state)
{
    uint8_t data[MEM_PAGE];
    pmd85debug_region map[64];
    char region[64];
    GString *str;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(state->mem_view);
    int regions, row, col, count, space, digits, hexoff;
    uint32_t base, size;

    if (running_note(state, state->mem_view, "memory")) return;
    sync_space_choice(state);
    space = mem_space_now(state);
    size = pmd85debug_space_size(state->dbg, space);
    digits = space == PMD85DEBUG_SPACE_PHYS ? 6 : 4;
    hexoff = digits + 2;
    if (state->mem_base[space] > size) state->mem_base[space] = 0;
    base = state->mem_base[space];
    regions = space == PMD85DEBUG_SPACE_CPU ? pmd85debug_memory_map(state->dbg, map, 64) : 0;
    memset(data, 0, sizeof data);
    count = pmd85debug_mem_read(state->dbg, space, base, data, MEM_PAGE);
    str = g_string_new(NULL);
    g_string_append_printf(str, "%*s0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F\n", hexoff, "");
    for (row = 0; row < MEM_ROWS; row++) {
        g_string_append_printf(str, "%0*X  ", digits, base + row * 16);
        for (col = 0; col < 16; col++) {
            if (row * 16 + col < count) g_string_append_printf(str, "%02X ", data[row * 16 + col]);
            else g_string_append(str, "-- ");
        }
        g_string_append_c(str, ' ');
        for (col = 0; col < 16; col++) {
            uint8_t cell_height = data[row * 16 + col];
            g_string_append_c(str, (row * 16 + col < count && cell_height >= 0x20 && cell_height <= 0x7E) ? (char)cell_height : '.');
        }
        /* the hardware answering at the row's first byte, from one map fetch */
        if (space == PMD85DEBUG_SPACE_CPU) {
            pmd85mem_region_name(map, regions, (uint16_t)(base + row * 16), region, sizeof region);
            if (region[0]) g_string_append_printf(str, "  %s", region);
        }
        g_string_append_c(str, '\n');
    }
    set_text(state->mem_view, str->str);
    g_string_free(str, TRUE);

    {   /* unchanged text keeps its tags: clear the old highlight first */
        GtkTextIter iter_start, iter_end;
        gtk_text_buffer_get_bounds(buffer, &iter_start, &iter_end);
        gtk_text_buffer_remove_tag_by_name(buffer, "mark", &iter_start, &iter_end);
    }
    if (state->mem_hl && state->mem_hl_space == space) {
        uint32_t address;
        for (address = base; address < base + MEM_PAGE; address++) {
            GtkTextIter iter_start, iter_end;
            int line, off;
            gboolean next;
            if (address < state->mem_hl_lo || address > state->mem_hl_hi) continue;
            line = (int)(address - base) / 16 + 1;
            col = (int)(address - base) % 16;
            next = address < state->mem_hl_hi && col < 15;
            gtk_text_buffer_get_iter_at_line_offset(buffer, &iter_start, line, hexoff + col * 3);
            gtk_text_buffer_get_iter_at_line_offset(buffer, &iter_end, line, hexoff + col * 3 + (next ? 3 : 2));
            gtk_text_buffer_apply_tag_by_name(buffer, "mark", &iter_start, &iter_end);
            off = hexoff + 48 + 1 + col;
            gtk_text_buffer_get_iter_at_line_offset(buffer, &iter_start, line, off);
            gtk_text_buffer_get_iter_at_line_offset(buffer, &iter_end, line, off + 1);
            gtk_text_buffer_apply_tag_by_name(buffer, "mark", &iter_start, &iter_end);
        }
    }
}

static void refresh_disasm(DbgWin *state)
{
    static pmd85debug_line lines[DISASM_WINDOW];
    int dim_from[DISASM_WINDOW * 2];    /* per buffer line: grey from this column, -1 none */
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(state->disasm);
    GString *str;
    GtkTextIter iter_start, iter_end;
    int count, pc_line, index, line = 0, sel_line = -1, pc_buf = -1;
    gsize head;
    pmd85debug_regs regs;

    if (!pmd85debug_regs_get(state->dbg, &regs)) {
        set_text(state->disasm, "The machine is running: stop it (F5) to read the program.");
        state->line_count = 0;
        return;
    }
    str = g_string_new(NULL);
    /* Following, the PC sits a third of the way down the window. */
    if (gtk_check_button_get_active(state->follow_pc)) {
        state->disasm_top = pmd85debug_row_address(state->dbg, regs.pc, -(DISASM_WINDOW / 3));
        if (state->disasm_top > regs.pc) state->disasm_top = 0;   /* near $0000 it wraps: start at the bottom */
    }

    count = pmd85debug_disassemble_ex(state->dbg, (uint16_t)state->disasm_top, lines, DISASM_WINDOW, &pc_line);
    for (index = 0; index < count; index++) {
        /* a label gets a line of its own above its instruction */
        if (lines[index].label[0]) {
            g_string_append_printf(str, "      %s:\n", lines[index].label);
            dim_from[line] = 0;
            state->line_addr[line++] = -1;
        }
        head = str->len;
        /* a filled dot for an enabled breakpoint, a hollow one for a disabled one */
        g_string_append_printf(str, "%s%c %04X  %-11s %-18s",
                               lines[index].has_breakpoint ? "\xe2\x97\x8f"
                                   : lines[index].bp_disabled ? "\xe2\x97\x8b" : " ",
                               lines[index].is_pc ? '>' : ' ',
                               lines[index].address, lines[index].bytes, lines[index].disasm);
        dim_from[line] = -1;
        if (lines[index].comment[0]) {
            dim_from[line] = (int)g_utf8_strlen(str->str + head, -1);
            g_string_append_printf(str, "; %s", lines[index].comment);
        }
        g_string_append_c(str, '\n');
        if (lines[index].address == state->selected) sel_line = line;
        if (index == pc_line) pc_buf = line;
        state->line_addr[line++] = lines[index].address;
    }
    state->line_count = line;
    if (count == 0) g_string_append(str, "(no disassembly)\n");
    gtk_text_buffer_set_text(buffer, str->str, -1);
    g_string_free(str, TRUE);

    /* labels and comments in grey */
    for (index = 0; index < line; index++) {
        if (dim_from[index] < 0) continue;
        gtk_text_buffer_get_iter_at_line(buffer, &iter_start, index);
        if (dim_from[index] > 0) gtk_text_iter_set_line_offset(&iter_start, dim_from[index]);
        iter_end = iter_start;
        gtk_text_iter_forward_to_line_end(&iter_end);
        gtk_text_buffer_apply_tag_by_name(buffer, "dim", &iter_start, &iter_end);
    }
    if (sel_line >= 0) {
        gtk_text_buffer_get_iter_at_line(buffer, &iter_start, sel_line);
        gtk_text_buffer_get_iter_at_line(buffer, &iter_end, sel_line + 1);
        gtk_text_buffer_apply_tag_by_name(buffer, "sel", &iter_start, &iter_end);
    }
    /* the PC line in the accent colour (over the selection) */
    if (pc_buf >= 0) {
        gtk_text_buffer_get_iter_at_line(buffer, &iter_start, pc_buf);
        gtk_text_buffer_get_iter_at_line(buffer, &iter_end, pc_buf + 1);
        gtk_text_buffer_apply_tag_by_name(buffer, "pc", &iter_start, &iter_end);
    }
    /* the gutter marker: red while the breakpoint is enabled, grey off (it
     * keeps its colour on the PC line) */
    for (index = 0, line = 0; index < count; index++) {
        if (lines[index].label[0]) line++;
        if (lines[index].has_breakpoint || lines[index].bp_disabled) {
            gtk_text_buffer_get_iter_at_line(buffer, &iter_start, line);
            iter_end = iter_start;
            gtk_text_iter_forward_char(&iter_end);
            gtk_text_buffer_apply_tag_by_name(buffer, lines[index].has_breakpoint ? "bp_on" : "bp_off",
                                              &iter_start, &iter_end);
        }
        line++;
    }
}

/* ---- I/O: the chips, the memory map, the I/O log ----------------------- */

static void append_pio(GString *text, const char *name, const pmd85debug_pio *pio)
{
    if (!pio->valid) return;
    g_string_append_printf(text,
        "  %-8s CWR %02X   out PA %02X  PB %02X  PC %02X\n"
        "  %-8s INTE %X    in  PA %02X  PB %02X  PC %02X\n",
        name, pio->cwr, pio->a, pio->b, pio->c, "", pio->inte, pio->in_a, pio->in_b, pio->in_c);
}

/* The model and the memory map, then the chips. */
static void refresh_io(DbgWin *state)
{
    pmd85debug_io chips;
    pmd85debug_region regions[64];
    GString *text;
    int index, region_count;

    if (running_note(state, state->io_text, "the chips")) return;
    text = g_string_new(NULL);
    if (!pmd85debug_io_get(state->dbg, &chips)) {
        set_text(state->io_text, "(no machine)");
        g_string_free(text, TRUE);
        return;
    }
    g_string_append_printf(text, "%s   LEDs %02X%s%s%s\n\nMemory map (as mapped now)\n",
                           pmd85session_model_name(chips.model), chips.led,
                           chips.allram ? "   ALLRAM" : "", chips.remapped ? "   remapped" : "",
                           chips.in_reset ? "   (reset map)" : "");
    region_count = pmd85debug_memory_map(state->dbg, regions, 64);
    for (index = 0; index < region_count; index++)
        g_string_append_printf(text, "  %04X-%04X  %s  %s\n", regions[index].lo, regions[index].hi,
                               regions[index].writable ? "RW" : "R-", regions[index].name);
    g_string_append(text, "\n8255 PIOs\n");
    append_pio(text, "SYS PIO", &chips.sys);
    append_pio(text, "GPIO", &chips.gpio);
    append_pio(text, "ROM PIO", &chips.rom);
    if (chips.pit[0].valid) {
        g_string_append(text, "\n8253 timer\n");
        for (index = 0; index < 3; index++)
            g_string_append_printf(text, "  CT%d  CWR %02X  init %04X  count %04X  G%d O%d%s\n",
                                   index, chips.pit[index].cwr, chips.pit[index].init,
                                   chips.pit[index].value, chips.pit[index].gate, chips.pit[index].out,
                                   chips.pit[index].counting ? "  run" : "");
    }
    if (chips.usart_valid)
        g_string_append_printf(text, "\n8251 USART (tape)\n  mode %02X  command %02X  status %02X\n",
                               chips.usart_mode, chips.usart_command, chips.usart_status);
    g_string_append(text, "\nModules and expansions\n");
    if (chips.mega_page >= 0)
        g_string_append_printf(text, "  MegaModule        page %d\n", chips.mega_page);
    else
        g_string_append(text, "  MegaModule        -\n");
    if (chips.mm256 >= 0)
        g_string_append_printf(text, "  256K mapper       page %02X\n", chips.mm256);
    else
        g_string_append(text, "  256K mapper       -\n");
    g_string_append_printf(text, "  MIF 85            %s\n  PMD 32            %s\n",
                           chips.mif85 ? "fitted" : "-", chips.pmd32 ? "fitted" : "-");
    if (chips.becker)
        g_string_append_printf(text, "\nBecker (DriveWire) link   %s   %d byte%s waiting for the CPU\n",
                               chips.becker_link ? "up" : "down", chips.becker_rx,
                               chips.becker_rx == 1 ? "" : "s");
    else
        g_string_append(text, "\nBecker port: not fitted (FujiNet is off)\n");
    set_text(state->io_text, text->str);
    g_string_free(text, TRUE);
}

/* The newest accesses, oldest first; a port filter (a port or lo-hi) narrows
 * it. Rebuilt only when the log moved on. */
static void refresh_iolog(DbgWin *state, gboolean force)
{
    static pmd85debug_io_access log[IOLOG_READ];
    const char *ftext = gtk_editable_get_text(GTK_EDITABLE(state->io_filter));
    long lo = 0, hi = 255;
    int count, index, shown = 0;
    uint64_t sig;
    GString *text;
    char note[160];

    state->io_sync = TRUE;
    gtk_check_button_set_active(state->io_on, pmd85debug_io_log_enabled(state->dbg) != 0);
    state->io_sync = FALSE;
    if (ftext && *ftext) {
        char buf[64], *dash;
        g_strlcpy(buf, ftext, sizeof buf);
        dash = strchr(buf + 1, '-');
        if (dash) *dash = '\0';
        if (!parse_num(buf, &lo) || (dash && !parse_num(dash + 1, &hi)) || lo < 0 || lo > 255 ||
            hi < 0 || hi > 255) {
            gtk_label_set_text(state->io_note, "Filter: a port (3E) or a range (84-87)");
            return;
        }
        if (!dash) hi = lo;
    }
    count = pmd85debug_io_log(state->dbg, log, IOLOG_READ);
    sig = ((uint64_t)count << 48) ^ (count ? log[0].tstates : 0) ^ (uint64_t)(lo << 8 | hi);
    if (!force && sig == state->io_sig) return;
    state->io_sig = sig;
    text = g_string_new(NULL);
    for (index = count - 1; index >= 0; index--) {
        const char *name;
        char port[16], value[16];
        if (log[index].port < lo || log[index].port > hi) continue;
        name = pmd85debug_port_name(state->dbg, log[index].port);
        pmd85debug_format_hex(state->dbg, log[index].port, 2, port, sizeof port);
        pmd85debug_format_hex(state->dbg, log[index].value, 2, value, sizeof value);
        g_string_append_printf(text, "%14llu  %04X  %-3s %-5s = %-5s  %s\n",
                               (unsigned long long)log[index].tstates, log[index].pc,
                               log[index].out ? "OUT" : "IN", port, value, name ? name : "");
        shown++;
    }
    if (!shown)
        g_string_append(text, pmd85debug_io_log_enabled(state->dbg)
                                  ? "(nothing yet: it records while the debugger is open)"
                                  : "The I/O log is off.");
    set_text_at(state->io_log, text->str, TRUE);
    g_string_free(text, TRUE);
    g_snprintf(note, sizeof note, "%d access%s (the newest %d kept)", shown, shown == 1 ? "" : "es",
               IOLOG_READ);
    gtk_label_set_text(state->io_note, note);
}

/* An exchange's bytes as the Becker port's own debug dump has them: a line
 * per 16 bytes in one direction, OUT: (PMD to FujiNet) or IN: (back), a gap
 * after the eighth, then the bytes as text. A direction up to 16 bytes past
 * PMD85_FUJI_DIR (a 256-byte payload and its command) is shown whole; a
 * longer one stops there with a count of the rest. */
static void dump_packet(GString *text, const pmd85_fuji_packet *p)
{
    const int in_total = p->length > 0 ? p->length : 0;
    const int total[2] = { in_total, p->nbytes - in_total };   /* [1]: out */
    int limit[2], shown[2] = { 0, 0 }, at = 0, d;

    for (d = 0; d < 2; d++)
        limit[d] = total[d] <= PMD85_FUJI_KEEP ? total[d] : PMD85_FUJI_DIR;
#define OUT_AT(i) ((p->out_bits[(i) / 8] >> ((i) % 8)) & 1)
    while (at < p->nkept) {
        const int out = OUT_AT(at);
        int run = 0, col;
        uint8_t line[16];
        char ascii[17];

        /* one line: up to 16 bytes that went the same way */
        while (at < p->nkept && run < 16 && OUT_AT(at) == out) {
            if (shown[out] < limit[out]) {
                line[run++] = p->data[at];
                shown[out]++;
            }
            at++;
            if (shown[out] == limit[out])
                break;
        }
        if (run) {
            g_string_append(text, out ? "  OUT: " : "  IN:  ");
            for (col = 0; col < 16; col++) {
                if (col == 8) g_string_append_c(text, ' ');
                if (col < run) {
                    g_string_append_printf(text, "%02X ", line[col]);
                    ascii[col] = (line[col] >= 0x20 && line[col] < 0x7F) ? (char)line[col] : '.';
                } else {
                    g_string_append(text, "   ");   /* keeps the text column in line */
                }
            }
            ascii[run] = '\0';
            g_string_append_printf(text, " %s\n", ascii);
        }
        if (shown[out] == limit[out] && total[out] > limit[out]) {
            g_string_append_printf(text, "        \xe2\x80\xa6 %d more %s byte%s\n",
                                   total[out] - limit[out], out ? "OUT" : "IN",
                                   total[out] - limit[out] == 1 ? "" : "s");
            limit[out] = -1;   /* said once; nothing more of it is shown */
        }
    }
#undef OUT_AT
}

/* The packet log is rebuilt only when it changed: a new packet, or a
 * repeat (which moves the newest serial on). */
static void refresh_fujinet(DbgWin *dbg_state, gboolean force)
{
    static pmd85_fuji_packet packets[TRACE_MAX];
    static char state[2048];
    char note[200];
    GString *text;
    const gboolean hide = gtk_check_button_get_active(dbg_state->fuji_hide_ready);
    int count = pmd85debug_fujinet_trace(dbg_state->dbg, packets, TRACE_MAX), index, shown = 0;

    if (!pmd85session_fujinet_running(dbg_state->session) && !pmd85session_fujinet_external(dbg_state->session))
        g_strlcpy(note, "FujiNet is off (Preferences \xe2\x96\xb8 FujiNet).", sizeof note);
    else
        g_snprintf(note, sizeof note, "%s FujiNet on the Becker port %s, %s.",
                   pmd85session_fujinet_external(dbg_state->session) ? "External" : "Built-in",
                   pmd85session_becker_endpoint(dbg_state->session),
                   pmd85session_fujinet_link(dbg_state->session) ? "connected" : "not connected");
    gtk_label_set_text(dbg_state->fuji_note, note);
    pmd85debug_fujinet_state(dbg_state->dbg, state, sizeof state);
    set_text(dbg_state->fuji_state, state);

    if (!force && count == dbg_state->fuji_count && (count == 0 || packets[count - 1].serial == dbg_state->fuji_last))
        return;
    dbg_state->fuji_count = count;
    dbg_state->fuji_last = count > 0 ? packets[count - 1].serial : 0;
    log_note_reader(dbg_state->fuji_text);
    text = g_string_new(NULL);
    for (index = 0; index < count; index++) {
        if (hide && pmd85debug_fujinet_is_ready(&packets[index])) continue;
        g_string_append_printf(text, "%s%s", shown++ ? "\n" : "", packets[index].text);
        if (packets[index].repeat > 1) g_string_append_printf(text, " \xc3\x97%u", packets[index].repeat);
        g_string_append_c(text, '\n');
        dump_packet(text, &packets[index]);
    }
    if (shown == 0)
        g_string_append(text, count ? "(only READY probes since the trace was cleared)"
                             : "(no DriveWire traffic since the trace was cleared)");
    set_text_at(dbg_state->fuji_text, text->str, force || following(dbg_state->fuji_text));
    g_string_free(text, TRUE);
}

static void bp_enable_toggled(GtkCheckButton *button, gpointer user_data);
static void bp_remove_clicked(GtkButton *button, gpointer user_data);

static void refresh_bps(DbgWin *state)
{
    static pmd85debug_breakpoint bps[PMD85DEBUG_MAX_BREAKPOINTS];
    GtkWidget *child;
    GString *sig = g_string_new(NULL);
    int count = pmd85debug_breakpoints_list(state->dbg, bps, PMD85DEBUG_MAX_BREAKPOINTS), index;
    char line[256];
    pmd85debug_stop stop;

    pmd85debug_stop_snapshot(state->dbg, &stop);
    if (stop.paused) {
        if (!stop.reason[0] || strcmp(stop.reason, "stopped") == 0)
            g_strlcpy(line, "Stopped", sizeof line);
        else if (g_str_has_prefix(stop.reason, "stopped "))
            g_snprintf(line, sizeof line, "S%s", stop.reason + 1);
        else
            g_snprintf(line, sizeof line, "Stopped: %s", stop.reason);
    } else
        g_strlcpy(line, "Running.", sizeof line);
    gtk_label_set_text(state->bp_stop, line);

    /* the list is rebuilt only when what it shows changed (hit counts move
     * while the machine runs), so a click on a row is not pulled out from
     * under the pointer */
    for (index = 0; index < count; index++)
        g_string_append_printf(sig, "%d/%d/%u-%u/%d/%u/%s\n", bps[index].id, bps[index].type,
                               bps[index].start, bps[index].end, bps[index].enabled, bps[index].hits,
                               bps[index].condition);
    if (state->bp_sig_reset && state->bp_sig) {
        g_string_free(state->bp_sig, TRUE);
        state->bp_sig = NULL;
    }
    state->bp_sig_reset = FALSE;
    if (state->bp_sig && g_string_equal(sig, state->bp_sig)) {
        g_string_free(sig, TRUE);
        return;
    }
    if (state->bp_sig) g_string_free(state->bp_sig, TRUE);
    state->bp_sig = sig;

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(state->bp_list))) != NULL)
        gtk_list_box_remove(state->bp_list, child);
    if (count == 0) {
        GtkWidget *label = gtk_label_new("No breakpoints. Select a disassembly line and press F9, "
                                     "or add one below.");
        gtk_widget_add_css_class(label, "dim-label");
        gtk_list_box_append(state->bp_list, label);
        return;
    }
    for (index = 0; index < count; index++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *enable_box = gtk_check_button_new();
        GtkWidget *remove_button = gtk_button_new_from_icon_name("user-trash-symbolic");
        GtkWidget *label;
        char text[360], desc[256];
        pmd85bp_describe(state->dbg, &bps[index], desc, sizeof desc);
        g_snprintf(text, sizeof text, "#%-3d %s    hits %u", bps[index].id, desc, bps[index].hits);
        label = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_add_css_class(label, "monospace");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(enable_box), bps[index].enabled != 0);
        gtk_widget_set_tooltip_text(enable_box, "Enabled");
        g_object_set_data(G_OBJECT(enable_box), "bp", GINT_TO_POINTER(bps[index].id));
        g_object_set_data(G_OBJECT(remove_button), "bp", GINT_TO_POINTER(bps[index].id));
        g_signal_connect(enable_box, "toggled", G_CALLBACK(bp_enable_toggled), state);
        g_signal_connect(remove_button, "clicked", G_CALLBACK(bp_remove_clicked), state);
        gtk_widget_add_css_class(remove_button, "flat");
        gtk_widget_set_tooltip_text(remove_button, "Remove");
        gtk_box_append(GTK_BOX(row), enable_box);
        gtk_box_append(GTK_BOX(row), label);
        gtk_box_append(GTK_BOX(row), remove_button);
        gtk_list_box_append(state->bp_list, row);
    }
}

/* ---- tabs, and tabs popped out into windows of their own -------------------- */

static void refresh_now(DbgWin *state);
static void show_in_memory(DbgWin *state, int space, uint32_t first, uint32_t last);

static int page_tab(GtkWidget *page)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(page), "tab"));
}

/* The tab showing in the main window (-1 if the notebook is empty). */
static int current_tab(DbgWin *state)
{
    GtkWidget *page = gtk_notebook_get_nth_page(state->notebook,
                                                gtk_notebook_get_current_page(state->notebook));
    return page ? page_tab(page) : -1;
}


/* In front of the user: the current page, or a tab popped out. */
static gboolean tab_visible(DbgWin *state, int tab)
{
    return state->detached[tab] != NULL || current_tab(state) == tab;
}

static void show_tab(DbgWin *state, int tab)
{
    if (state->detached[tab])
        gtk_window_present(state->detached[tab]);
    else
        gtk_notebook_set_current_page(state->notebook,
                                      gtk_notebook_page_num(state->notebook, state->pages[tab]));
}

/* ---- symbols ------------------------------------------------------------------ */

/* A row opens the disassembly at its address. */
static void on_sym_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)box;
    if (!row) return;
    gtk_check_button_set_active(state->follow_pc, FALSE);
    state->disasm_top = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "addr"));
    show_tab(state, TAB_DISASM);
    refresh_disasm(state);
}

/* The index order is unstable (loading, clearing and lookups reorder it), so
 * the list is fetched through the filter every time, each row caches its
 * address, and the list is rebuilt only when what it would show changed. */
static void refresh_symbols(DbgWin *state)
{
    static int indices[SYMBOLS_SHOWN + 1];
    GString *sig = g_string_new(NULL);
    const char *filter = gtk_editable_get_text(GTK_EDITABLE(state->sym_filter));
    const int total = pmd85debug_symbols_count(state->dbg);
    const int matched = pmd85debug_symbols_filter(state->dbg, filter, indices, SYMBOLS_SHOWN + 1);
    const int shown = matched > SYMBOLS_SHOWN ? SYMBOLS_SHOWN : matched;
    static uint16_t addresses[SYMBOLS_SHOWN];
    static char names[SYMBOLS_SHOWN][64];
    int index;
    char note[160];

    for (index = 0; index < shown; index++) {
        if (!pmd85debug_symbols_get(state->dbg, indices[index], &addresses[index], names[index], sizeof names[index])) {
            addresses[index] = 0;
            names[index][0] = '\0';
        }
        g_string_append_printf(sig, "%04X/%s\n", addresses[index], names[index]);
    }
    g_string_append_printf(sig, "%d/%d/%d", matched, total, (int)pmd85debug_hex_style(state->dbg));
    if (state->sym_sig && g_string_equal(sig, state->sym_sig)) {
        g_string_free(sig, TRUE);
        return;
    }
    if (state->sym_sig) g_string_free(state->sym_sig, TRUE);
    state->sym_sig = sig;

    gtk_list_box_remove_all(state->sym_list);
    for (index = 0; index < shown; index++) {
        GtkWidget *row_label;
        GtkWidget *row = gtk_list_box_row_new();
        char text[160];
        char hex[16];
        pmd85debug_format_hex(state->dbg, addresses[index], 4, hex, sizeof hex);
        g_snprintf(text, sizeof text, "%-6s  %s", hex, names[index]);
        row_label = gtk_label_new(text);
        gtk_label_set_xalign(GTK_LABEL(row_label), 0);
        gtk_widget_add_css_class(row_label, "monospace");
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_label);
        g_object_set_data(G_OBJECT(row), "addr", GINT_TO_POINTER(addresses[index]));
        gtk_list_box_append(state->sym_list, row);
    }
    if (matched > SYMBOLS_SHOWN)
        g_snprintf(note, sizeof note, "%d of %d symbols match; the first %d are shown "
                   "(narrow the filter)", matched, total, SYMBOLS_SHOWN);
    else
        g_snprintf(note, sizeof note, "%d of %d symbol%s", matched, total, total == 1 ? "" : "s");
    {
        char info[160];
        pmd85debug_monitor_symbols_info(state->dbg, info, sizeof info);
        info[0] = (char)g_ascii_toupper(info[0]);
        gtk_label_set_text(state->sym_info, info);
    }
    gtk_label_set_text(state->sym_note, note);
}

/* a new filter, typed or cleared: show it now */
static void on_sym_filter_changed(GtkEditable *editable, gpointer user_data)
{
    (void)editable;
    refresh_symbols(user_data);
}

/* ---- watches ------------------------------------------------------------------ */

static void on_wt_remove(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    pmd85watch_remove(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "wt-idx")));
    refresh_now(state);
}

static void on_wt_memory(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    const int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "wt-idx"));
    uint32_t first, last;
    if (!pmd85watch_range(state->dbg, idx, &first, &last)) {
        toast(state, "That watch does not resolve to an address yet");
        return;
    }
    show_in_memory(state, pmd85watch_space(idx), first, last);
    show_tab(state, TAB_MEMORY);
}

/* Break when the watched bytes change: a write whose value differs from the
 * byte it replaced. */
static void break_on_change(DbgWin *state, int space, uint32_t first, uint32_t last)
{
    const int type = PMD85DEBUG_BP_WRITE | (space == PMD85DEBUG_SPACE_PHYS ? PMD85DEBUG_BP_PHYS : 0);
    if (pmd85debug_breakpoint_add(state->dbg, type, first, last, "value != old") < 0) {
        toast(state, "The breakpoint was refused");
        return;
    }
    toast(state, "Breaking when it changes");
}

static void on_wt_break(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    const int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "wt-idx"));
    uint32_t first, last;
    if (!pmd85watch_range(state->dbg, idx, &first, &last)) {
        toast(state, "That watch does not resolve to an address yet");
        return;
    }
    break_on_change(state, pmd85watch_space(idx), first, last);
    refresh_now(state);
}

static void wt_rebuild(DbgWin *state)
{
    const int count = pmd85watch_count();
    int index;

    gtk_list_box_remove_all(state->wt_list);
    g_ptr_array_set_size(state->wt_labels, 0);
    if (count == 0) {
        GtkWidget *placeholder = gtk_label_new("No watches. Type an address, a symbol, symbol+3, {ptr} "
                                     "or a range (C000-C03F) above.");
        gtk_widget_add_css_class(placeholder, "dim-label");
        gtk_list_box_append(state->wt_list, placeholder);
    }
    for (index = 0; index < count; index++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *label = gtk_label_new(NULL);
        static const struct { const char *text, *tip; GCallback callback; } btns[] = {
            { "Memory", "Show it in the Memory tab", G_CALLBACK(on_wt_memory) },
            { "Break on change", "Stop when a write changes it", G_CALLBACK(on_wt_break) },
            { "Remove", "Forget this watch", G_CALLBACK(on_wt_remove) },
        };
        size_t slot;
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_add_css_class(label, "monospace");
        gtk_box_append(GTK_BOX(row), label);
        for (slot = 0; slot < G_N_ELEMENTS(btns); slot++) {
            GtkWidget *button = gtk_button_new_with_label(btns[slot].text);
            gtk_widget_add_css_class(button, "flat");
            gtk_widget_set_tooltip_text(button, btns[slot].tip);
            g_object_set_data(G_OBJECT(button), "wt-idx", GINT_TO_POINTER(index));
            g_signal_connect(button, "clicked", btns[slot].callback, state);
            gtk_box_append(GTK_BOX(row), button);
        }
        gtk_list_box_append(state->wt_list, row);
        g_ptr_array_add(state->wt_labels, label);
    }
}

/* The rows are rebuilt only when the list changed; the values are written
 * into the labels in place, so a click on a row's button is never pulled
 * from under the pointer. */
static void refresh_watches(DbgWin *state)
{
    const unsigned gen = pmd85watch_generation();
    int index;
    if (!state->wt_built || gen != state->wt_gen || pmd85watch_count() != (int)state->wt_labels->len) {
        wt_rebuild(state);
        state->wt_gen = gen;
        state->wt_built = TRUE;
    }
    for (index = 0; index < (int)state->wt_labels->len; index++) {
        char line[200];
        pmd85watch_format(state->dbg, index, line, sizeof line);
        gtk_label_set_text(g_ptr_array_index(state->wt_labels, index), line);
    }
}

static void on_wt_add(GtkWidget *wgt, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)wgt;
    if (pmd85watch_add(gtk_editable_get_text(GTK_EDITABLE(state->wt_entry)),
                      gtk_drop_down_get_selected(state->wt_space) == 1 ? PMD85DEBUG_SPACE_PHYS
                                                                  : PMD85DEBUG_SPACE_CPU) < 0) {
        toast(state, "Type an address, a symbol, an expression or a range");
        return;
    }
    gtk_editable_set_text(GTK_EDITABLE(state->wt_entry), "");
    refresh_now(state);
}

/* ---- trace and history -------------------------------------------------------- */

/* Both tabs read the one ring. Trace (0) follows it while the machine runs;
 * History (1) is read once the machine has stopped. The newest HIST_SHOWN
 * matches are shown, oldest first. */
static void refresh_hist(DbgWin *state, int idx, gboolean force)
{
    static pmd85debug_trace_entry buf[HIST_READ];
    static int hit[HIST_SHOWN];
    const gboolean recording = pmd85debug_trace_enabled(state->dbg) != 0;
    const gboolean stopped = pmd85debug_is_paused(state->dbg) != 0;
    const int count = pmd85debug_trace_count(state->dbg);
    uint32_t first = 0, last = 0xFFFF;
    int filter = PMD85DEBUG_TRACE_ALL, entry_count, index, shown = 0, matched = 0;
    uint64_t sig;
    pmd85debug_trace_entry newest;
    char note[200];
    GString *text;

    state->h_sync = TRUE;       /* the Record toggles follow the engine */
    gtk_toggle_button_set_active(state->h_rec[0], recording);
    gtk_toggle_button_set_active(state->h_rec[1], recording);
    state->h_sync = FALSE;

    if (idx == 1 && !stopped) {
        gtk_label_set_text(state->h_note[1], "Stop the machine to read its history.");
        state->h_sig[1] = ~(uint64_t)0;
        return;
    }
    memset(&newest, 0, sizeof newest);
    pmd85debug_trace_read(state->dbg, &newest, 1);
    sig = ((uint64_t)count << 40) ^ newest.tstates ^ (recording ? 1 : 0);
    if (!force && sig == state->h_sig[idx]) return;

    switch (gtk_drop_down_get_selected(state->h_filter[idx])) {
    case 1: filter = PMD85DEBUG_TRACE_BRANCHES; break;
    case 3: filter = PMD85DEBUG_TRACE_IO; break;
    case 2:
        if (!pmd85sym_parse_range(state->dbg, PMD85DEBUG_SPACE_CPU,
                                 gtk_editable_get_text(GTK_EDITABLE(state->h_range[idx])), &first, &last)) {
            gtk_label_set_text(state->h_note[idx], "Type a range: 8000-8FFF, a symbol, symbol..symbol");
            return;
        }
        filter = PMD85DEBUG_TRACE_RANGE;
        break;
    default: break;
    }
    state->h_sig[idx] = sig;
    entry_count = pmd85debug_trace_read(state->dbg, buf, filter == PMD85DEBUG_TRACE_ALL ? HIST_SHOWN : HIST_READ);
    for (index = 0; index < entry_count; index++)             /* newest first: take the newest matches */
        if (pmd85debug_trace_match(&buf[index], filter, (uint16_t)first, (uint16_t)last)) {
            matched++;
            if (shown < HIST_SHOWN) hit[shown++] = index;
        }
    text = g_string_new(NULL);
    if (shown == 0)
        g_string_append(text, recording
                                ? (count > 0 ? "(no recorded instruction matches this filter)"
                                             : "(nothing recorded yet: let the machine run)")
                              : "Recording is off. Press Record, then let the machine run.");
    for (index = shown - 1; index >= 0; index--) {  /* oldest first */
        char line[256];
        pmd85debug_trace_format(state->dbg, &buf[hit[index]], line, sizeof line);
        g_string_append(text, line);
        g_string_append_c(text, '\n');
    }
    set_text_at(state->h_text[idx], text->str, TRUE);
    g_string_free(text, TRUE);
    g_snprintf(note, sizeof note, "%s, %d instruction%s%s", recording ? "recording" : "recording off",
               count, count == 1 ? "" : "s",
               matched > shown ? ", newest 2000 shown" : "");
    gtk_label_set_text(state->h_note[idx], note);
}

static void refresh_status(DbgWin *state)
{
    pmd85debug_stop stop;
    char text[256];
    pmd85debug_stop_snapshot(state->dbg, &stop);
    if (stop.paused) {
        const gboolean plain = !stop.reason[0] || strcmp(stop.reason, "stopped") == 0;
        const gboolean restated = g_str_has_prefix(stop.reason, "stopped ");   /* "stopped at $0412" */
        if (restated)
            g_snprintf(text, sizeof text, "S%s", stop.reason + 1);
        else
            g_snprintf(text, sizeof text, "Stopped%s%s", plain ? "" : ": ", plain ? "" : stop.reason);
    } else
        g_snprintf(text, sizeof text, "Running");
    adw_window_title_set_subtitle(state->title, text);
    gtk_button_set_label(state->run_btn, stop.paused ? "Run (F5)" : "Pause (F5)");
    if (stop.paused) gtk_widget_add_css_class(GTK_WIDGET(state->run_btn), "pmd85dbg-accent");
    else gtk_widget_remove_css_class(GTK_WIDGET(state->run_btn), "pmd85dbg-accent");
}

/* Only the tabs in front of the user are brought up to date (the one showing,
 * and any popped out); the others catch up when their tab is switched to. */
static void refresh_tab(DbgWin *state, int tab)
{
    if (state->closing) return;
    switch (tab) {
    case TAB_DISASM: refresh_disasm(state); break;
    case TAB_MEMORY: refresh_mem(state); break;
    case TAB_BREAKS: refresh_bps(state); break;
    case TAB_WATCHES: refresh_watches(state); break;
    case TAB_IO: refresh_io(state); refresh_iolog(state, FALSE); break;
    case TAB_SYMBOLS: refresh_symbols(state); break;
    case TAB_TRACE: refresh_hist(state, 0, FALSE); break;
    case TAB_HISTORY: refresh_hist(state, 1, FALSE); break;
    case TAB_FUJINET: refresh_fujinet(state, FALSE); break;
    default: break;
    }
}

/* The status, the registers and the tabs in front, and the generation they
 * now reflect. */
static void refresh_now(DbgWin *state)
{
    int tab;
    if (state->closing) return;
    state->seen_generation = pmd85debug_generation(state->dbg);
    state->was_stopped = pmd85debug_is_paused(state->dbg) != 0;
    refresh_status(state);
    refresh_cpu(state);
    for (tab = 0; tab < NTABS; tab++)
        if (tab_visible(state, tab)) refresh_tab(state, tab);
}

static void attach_and_refresh(DbgWin *state);

static void install_css(void)
{
    static gboolean done;
    GtkCssProvider *css;
    char buf[512];
    if (done) return;
    done = TRUE;
    css = gtk_css_provider_new();
    g_snprintf(buf, sizeof buf,
        "button.pmd85dbg-toggle:checked, togglebutton.pmd85dbg-toggle:checked,"
        " button.pmd85dbg-accent {"
        " background-image: none; background-color: #%06x; color: #%06x; }"
        /* the accent as text: a darker blue on light, the key blue on dark */
        " .pmd85dbg-accent-text { color: #2f5f80; }"
        " @media (prefers-color-scheme: dark) {"
        " .pmd85dbg-accent-text { color: #%06x; } }",
        ACCENT_RGB, ACCENT_INK_RGB, KEY_RGB);
    gtk_css_provider_load_from_string(css, buf);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* A new stop brings every view to the current spot: the disassembly to the PC
 * (done by the caller), the dump back to its top row, and the trace and
 * history to their newest line (they redraw and follow their end). */
static void recentre_views(DbgWin *state)
{
    GtkAdjustment *adj = vadj(state->mem_view);
    int index;
    if (adj) gtk_adjustment_set_value(adj, 0);
    for (index = 0; index < 2; index++) {
        state->h_sig[index] = ~(uint64_t)0;
        g_object_set_data(G_OBJECT(state->h_text[index]), "follow", GINT_TO_POINTER(1));
    }
    refresh_now(state);
}

static gboolean tick_refresh(gpointer user_data)
{
    DbgWin *state = user_data;
    unsigned gen;
    gboolean stopped;
    if (!gtk_widget_get_visible(GTK_WIDGET(state->win))) return G_SOURCE_CONTINUE;
    /* Showing across a session restart: the engine comes up with the machine. */
    if (!pmd85debug_is_engaged(state->dbg) && pmd85session_is_running(state->session)) {
        attach_and_refresh(state);
        return G_SOURCE_CONTINUE;
    }
    gen = pmd85debug_generation(state->dbg);
    stopped = pmd85debug_is_paused(state->dbg) != 0;
    {   /* a new stop brings the disassembly back to the PC, wherever it was scrolled */
        pmd85debug_stop stop;
        pmd85debug_stop_snapshot(state->dbg, &stop);
        if (stop.serial != state->seen_serial) {
            GtkAdjustment *adj = vadj(state->disasm);
            state->seen_serial = stop.serial;
            if (adj) gtk_adjustment_set_value(adj, 0);
            if (stop.paused && !gtk_check_button_get_active(state->follow_pc))
                gtk_check_button_set_active(state->follow_pc, TRUE);
            state->running_ticks = 0;
            recentre_views(state);
            return G_SOURCE_CONTINUE;
        }
    }
    if (!stopped && tab_visible(state, TAB_IO))
        refresh_iolog(state, FALSE);
    if (!stopped && tab_visible(state, TAB_TRACE) && ++state->trace_ticks >= 4) {
        state->trace_ticks = 0;     /* the trace follows the ring while the machine runs */
        refresh_hist(state, 0, FALSE);
    }
    if (gen != state->seen_generation || stopped != state->was_stopped) {
        state->running_ticks = 0;
        refresh_now(state);
    } else if (!stopped && ++state->running_ticks >= 5) {
        /* live values twice a second while the machine runs */
        state->running_ticks = 0;
        refresh_now(state);
    }
    return G_SOURCE_CONTINUE;
}

/* The refresh timer runs only while the window is mapped. */
static void on_win_map(GtkWidget *widget, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)widget;
    if (!state->timer && !state->closing)
        state->timer = g_timeout_add(100, tick_refresh, state);
}

static void on_win_unmap(GtkWidget *widget, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)widget;
    if (state->timer) g_source_remove(state->timer);
    state->timer = 0;
}

/* ---- handlers ------------------------------------------------------------- */

static void on_run(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)button;
    if (pmd85debug_is_paused(state->dbg)) {
        pmd85debug_resume(state->dbg);
    } else {
        pmd85debug_pause(state->dbg);
        settle(state, TRUE);
    }
    refresh_now(state);
}

/* A step: the machine is stopped, runs a moment, stops again. */
#define STEP_HANDLER(name, call) \
    static void name(GtkButton *button, gpointer user_data) \
    { \
        DbgWin *state = user_data; \
        (void)button; \
        call; \
        settle(state, TRUE); \
        refresh_now(state); \
    }
STEP_HANDLER(on_step, pmd85debug_step(state->dbg))
STEP_HANDLER(on_step_over, pmd85debug_step_over(state->dbg))
STEP_HANDLER(on_step_out, pmd85debug_step_out(state->dbg))
#undef STEP_HANDLER

static void on_run_to_cursor(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)button;
    if (state->selected < 0) {
        toast(state, "Select a disassembly line to run to");
        return;
    }
    pmd85debug_run_to(state->dbg, (uint16_t)state->selected);
    settle(state, TRUE);
    refresh_now(state);
}

/* The gutter and F9: a click on a breakpoint that is switched off (the hollow
 * dot) switches it back on; otherwise the breakpoint is set or removed. */
static void toggle_exec_breakpoint(DbgWin *state, uint16_t address)
{
    static pmd85debug_breakpoint list[PMD85DEBUG_MAX_BREAKPOINTS];
    const int count = pmd85debug_breakpoints_list(state->dbg, list, PMD85DEBUG_MAX_BREAKPOINTS);
    int index;
    for (index = 0; index < count; index++) {
        if (list[index].type == PMD85DEBUG_BP_EXEC && !list[index].enabled &&
            address >= list[index].start && address <= list[index].end) {
            pmd85debug_breakpoint_enable(state->dbg, list[index].id, 1);
            return;
        }
    }
    pmd85debug_breakpoint_toggle(state->dbg, address);
}

/* F9 and the gutter: an execute breakpoint on the selected line. */
static void toggle_selected(DbgWin *state)
{
    if (state->selected < 0) {
        toast(state, "Select a disassembly line first");
        return;
    }
    toggle_exec_breakpoint(state, (uint16_t)state->selected);
    refresh_now(state);
}

static void on_console(GtkEntry *entry, gpointer user_data)
{
    DbgWin *state = user_data;
    static char out[65536];
    const char *cmd = gtk_editable_get_text(GTK_EDITABLE(entry));
    char line[512];
    gboolean was_stopped = pmd85debug_is_paused(state->dbg) != 0;
    if (!cmd || !*cmd) return;
    /* the history keeps each command once in a row */
    if (state->history->len == 0 ||
        strcmp(g_ptr_array_index(state->history, state->history->len - 1), cmd) != 0)
        g_ptr_array_add(state->history, g_strdup(cmd));
    state->history_pos = state->history->len;
    g_snprintf(line, sizeof line, "> %s\n", cmd);
    append_text(state->console_out, line, TRUE);
    out[0] = '\0';
    pmd85debug_command(state->dbg, cmd, out, sizeof out);
    append_text(state->console_out, out, TRUE);
    if (out[0] && out[strlen(out) - 1] != '\n')
        append_text(state->console_out, "\n", TRUE);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    if (was_stopped) settle(state, TRUE);
    refresh_now(state);
}

static void history_show(DbgWin *state)
{
    const char *text = state->history_pos < state->history->len
                           ? g_ptr_array_index(state->history, state->history_pos) : "";
    gtk_editable_set_text(GTK_EDITABLE(state->console_in), text);
    gtk_editable_set_position(GTK_EDITABLE(state->console_in), -1);
}

/* Tab completes the current word against the commands, the registers and
 * the labels; several matches are listed instead. Up and Down walk the
 * command history. */
static gboolean on_console_key(GtkEventControllerKey *controller, guint keyval, guint code,
                               GdkModifierType modifiers, gpointer user_data)
{
    DbgWin *state = user_data;
    char comps[4096];
    const char *text;
    const char *word;
    int count;
    (void)controller; (void)code; (void)modifiers;
    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up) {
        if (state->history_pos > 0) {
            state->history_pos--;
            history_show(state);
        }
        return TRUE;
    }
    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down) {
        if (state->history_pos < state->history->len) {
            state->history_pos++;
            history_show(state);
        }
        return TRUE;
    }
    if (keyval != GDK_KEY_Tab) return FALSE;
    text = gtk_editable_get_text(GTK_EDITABLE(state->console_in));
    comps[0] = '\0';
    /* the engine completes the end of the whole line (a command, then its
     * arguments), so the whole line goes and the first word is replaced */
    count = pmd85debug_completions(state->dbg, text, comps, sizeof comps);
    word = strrchr(text, ' ');
    word = word ? word + 1 : text;
    if (count == 1) {
        char merged[600];
        char *newline = strchr(comps, '\n');
        if (newline) *newline = '\0';
        g_snprintf(merged, sizeof merged, "%.*s%s ", (int)(word - text), text, comps);
        gtk_editable_set_text(GTK_EDITABLE(state->console_in), merged);
        gtk_editable_set_position(GTK_EDITABLE(state->console_in), -1);
    } else if (count > 1) {
        /* the common start is filled in; the candidates are listed */
        char **cand = g_strsplit(comps, "\n", -1);
        size_t common = strlen(cand[0]), len = strlen(word);
        int index;
        for (index = 1; cand[index] && *cand[index]; index++) {
            size_t slot = 0;
            while (slot < common && cand[index][slot] == cand[0][slot]) slot++;
            common = slot;
        }
        if (common > len) {
            char merged[600];
            g_snprintf(merged, sizeof merged, "%.*s%.*s", (int)(word - text), text,
                       (int)common, cand[0]);
            gtk_editable_set_text(GTK_EDITABLE(state->console_in), merged);
            gtk_editable_set_position(GTK_EDITABLE(state->console_in), -1);
        }
        g_strfreev(cand);
        append_text(state->console_out, comps, FALSE);
        if (comps[0] && comps[strlen(comps) - 1] != '\n')
            append_text(state->console_out, "\n", FALSE);
    }
    return TRUE;
}

static void on_reg_activate(GtkEntry *entry, gpointer user_data)
{
    DbgWin *state = user_data;
    int index = k_regs[GPOINTER_TO_INT(g_object_get_data(G_OBJECT(entry), "reg"))].reg;
    long value;
    if (!pmd85debug_is_paused(state->dbg))
        toast(state, "Stop the machine to edit the registers");
    else if (parse_num(gtk_editable_get_text(GTK_EDITABLE(entry)), &value)) {
        if (!pmd85debug_regs_set(state->dbg, index, (unsigned)value))
            toast(state, "The register was not set");
    }
    /* Enter ends the edit: show the engine's value, or put the old one back */
    state->force_entries = TRUE;
    refresh_now(state);
    state->force_entries = FALSE;
}

/* A CC flag box: the engine takes the bit while the machine is stopped. */
static void on_flag_toggled(GtkCheckButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    const int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "flag"));
    if (state->flag_sync || state->closing) return;
    if (!pmd85debug_is_paused(state->dbg))
        toast(state, "Stop the machine to edit the flags");
    else
        pmd85debug_flag_set(state->dbg, k_flags[index].mask, gtk_check_button_get_active(button));
    state->force_entries = TRUE;
    refresh_now(state);
    state->force_entries = FALSE;
}

/* "address value...": bytes written into the space showing. */
static void on_mem_write(GtkEntry *entry, gpointer user_data)
{
    DbgWin *state = user_data;
    char **parts = g_strsplit_set(gtk_editable_get_text(GTK_EDITABLE(entry)), " \t:=,", -1);
    char err[128] = "";
    uint8_t bytes[POKE_MAX];
    const int space = mem_space_now(state);
    long address, value;
    int count = 0, index;
    gboolean valid;
    char *addr = NULL;
    for (index = 0; parts[index]; index++) {
        if (!*parts[index]) continue;
        if (!addr) addr = parts[index];
        else if (count < POKE_MAX && parse_num(parts[index], &value) && value >= 0 && value <= 255)
            bytes[count++] = (uint8_t)value;
        else { count = -1; break; }
    }
    valid = addr && count > 0 && parse_addr(state, addr, space, &address, err, sizeof err);
    if (valid && !pmd85debug_is_paused(state->dbg)) {
        toast(state, "Stop the machine to poke memory");
    } else if (valid) {
        if (pmd85debug_mem_write(state->dbg, space, (uint32_t)address, bytes, count) < count)
            toast(state, "Only RAM takes a poke: ROM was skipped");
        gtk_editable_set_text(GTK_EDITABLE(entry), "");
        refresh_now(state);
    } else {
        toast(state, err[0] ? err : "Poke takes an address and hex bytes, e.g. 0100 41 42");
    }
    g_strfreev(parts);
}

static void mem_set_base(DbgWin *state, long base)
{
    const int space = mem_space_now(state);
    const long size = (long)pmd85debug_space_size(state->dbg, space);
    if (base > size - MEM_PAGE) base = size - MEM_PAGE;
    if (base < 0) base = 0;
    state->mem_base[space] = (uint32_t)base & ~0xFu;
    refresh_mem(state);
}

/* The page holding a range, the range highlighted (a little context above it). */
static void show_in_memory(DbgWin *state, int space, uint32_t first, uint32_t last)
{
    state->mem_hl = TRUE;
    state->mem_hl_space = space;
    state->mem_hl_lo = first;
    state->mem_hl_hi = last;
    gtk_drop_down_set_selected(state->mem_space, space == PMD85DEBUG_SPACE_PHYS ? 1 : 0);
    mem_set_base(state, ((long)first & ~0xF) - 4 * 16);
}

/* Go to: an address, a symbol, an expression or a range, in the space showing. */
static void on_mem_goto(GtkEntry *entry, gpointer user_data)
{
    DbgWin *state = user_data;
    const int space = mem_space_now(state);
    uint32_t first, last;
    if (!pmd85sym_parse_range(state->dbg, space, gtk_editable_get_text(GTK_EDITABLE(entry)), &first, &last)) {
        toast(state, "Not an address, a symbol or a range in this space");
        return;
    }
    show_in_memory(state, space, first, last);
}

/* The space chooser changed: the other space has its own page and highlight. */
static void on_mem_space(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)object; (void)pspec;
    if (!state->closing) refresh_mem(state);
}

/* Find Next / Find Previous: from the highlight (else the top of the page). */
static void on_search(GtkWidget *button, gpointer user_data)
{
    DbgWin *state = user_data;
    const int backward = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "back"));
    const int space = mem_space_now(state);
    uint8_t pat[PMD85SEARCH_MAX], mask;
    int count, flags, result;
    uint32_t from, found;

    if (!pmd85search_parse((int)gtk_drop_down_get_selected(state->srch_kind),
                          gtk_editable_get_text(GTK_EDITABLE(state->srch_entry)),
                          pat, &count, &mask, &flags)) {
        gtk_label_set_text(state->srch_note, "Type hex bytes (86 41) or some text");
        return;
    }
    if (!pmd85debug_is_paused(state->dbg)) {
        gtk_label_set_text(state->srch_note, "Stop the machine first");
        return;
    }
    if (state->mem_hl && state->mem_hl_space == space)
        from = state->mem_hl_lo;
    else if (backward)
        from = state->mem_base[space];
    else
        from = state->mem_base[space] ? state->mem_base[space] - 1 : pmd85debug_space_size(state->dbg, space) - 1;
    result = pmd85debug_search_next(state->dbg, space, pat, count, mask, flags, from, backward, &found);
    if (result > 0) {
        char msg[64];
        char hex[16];
        pmd85debug_format_hex(state->dbg, found, 4, hex, sizeof hex);
        g_snprintf(msg, sizeof msg, "Found at %s", hex);
        show_in_memory(state, space, found, found + count - 1);
        gtk_label_set_text(state->srch_note, msg);
    } else if (result == 0) {
        gtk_label_set_text(state->srch_note, "Not found");
    } else {
        gtk_label_set_text(state->srch_note, "Stop the machine first");
    }
}

static void on_search_activate(GtkEntry *entry, gpointer user_data)
{
    on_search(g_object_get_data(G_OBJECT(entry), "next-button"), user_data);
}

static void on_mem_page(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    mem_set_base(state, (long)state->mem_base[mem_space_now(state)] +
                     GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "delta")) * MEM_PAGE);
}

/* Click selects a row; a click on the gutter or a double-click toggles its
 * breakpoint (a label line has no address, so it selects nothing). The
 * address comes from the row, never from the text. */
static void on_disasm_click(GtkGestureClick *gesture, int count, double position_x, double position_y, gpointer user_data)
{
    DbgWin *state = user_data;
    GtkTextIter iter, third;
    GdkRectangle rect;
    int buffer_x, buffer_y, line, top, height;
    gboolean gutter;
    (void)gesture;
    gtk_text_view_window_to_buffer_coords(state->disasm, GTK_TEXT_WINDOW_WIDGET, (int)position_x, (int)position_y, &buffer_x, &buffer_y);
    /* by line, not by hit-test: the margin left of the gutter is the gutter */
    gtk_text_view_get_line_at_y(state->disasm, &iter, buffer_y, NULL);
    gtk_text_view_get_line_yrange(state->disasm, &iter, &top, &height);
    if (buffer_y < top || buffer_y >= top + height) return;
    line = gtk_text_iter_get_line(&iter);
    if (line < 0 || line >= state->line_count || state->line_addr[line] < 0) return;
    third = iter;
    gtk_text_iter_set_line_offset(&third, 2);
    gtk_text_view_get_iter_location(state->disasm, &third, &rect);
    gutter = buffer_x < rect.x;
    state->selected = state->line_addr[line];
    if ((count == 1 && gutter) || (count == 2 && !gutter))
        toggle_exec_breakpoint(state, (uint16_t)state->selected);
    refresh_disasm(state);
}

static void on_follow_toggled(GtkCheckButton *button, gpointer user_data)
{
    (void)button;
    refresh_disasm(user_data);
}

static void on_jump(GtkEntry *entry, gpointer user_data)
{
    DbgWin *state = user_data;
    char err[128] = "";
    long address;
    if (!parse_addr(state, gtk_editable_get_text(GTK_EDITABLE(entry)), PMD85DEBUG_SPACE_CPU, &address, err, sizeof err)) {
        toast(state, err);
        return;
    }
    gtk_check_button_set_active(state->follow_pc, FALSE);
    state->disasm_top = (int)address;
    refresh_disasm(state);
}

/* The wheel scrolls by instruction. */
static gboolean on_disasm_scroll(GtkEventControllerScroll *controller, double delta_x, double delta_y, gpointer user_data)
{
    DbgWin *state = user_data;
    int rows = (int)(delta_y * 3);
    (void)controller; (void)delta_x;
    if (!rows) rows = delta_y > 0 ? 1 : -1;
    gtk_check_button_set_active(state->follow_pc, FALSE);
    state->disasm_top = pmd85debug_row_address(state->dbg, (uint16_t)state->disasm_top, rows);
    refresh_disasm(state);
    return TRUE;
}

static void bp_enable_toggled(GtkCheckButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    pmd85debug_breakpoint_enable(state->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "bp")),
                                gtk_check_button_get_active(button));
    refresh_now(state);
}

static void bp_remove_clicked(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    pmd85debug_breakpoint_remove(state->dbg, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "bp")));
    refresh_now(state);
}

static void on_bp_add(GtkWidget *wgt, gpointer user_data)
{
    DbgWin *state = user_data;
    static const int types[7] = {
        PMD85DEBUG_BP_EXEC, PMD85DEBUG_BP_READ, PMD85DEBUG_BP_WRITE, PMD85DEBUG_BP_ACCESS,
        PMD85DEBUG_BP_IN, PMD85DEBUG_BP_OUT, PMD85DEBUG_BP_IO };
    guint sel = gtk_drop_down_get_selected(state->bp_type);
    const gboolean phys = gtk_widget_get_visible(GTK_WIDGET(state->bp_phys)) &&
                          gtk_check_button_get_active(state->bp_phys);
    const int space = phys ? PMD85DEBUG_SPACE_PHYS : PMD85DEBUG_SPACE_CPU;
    long start, finish;
    int breakpoint_id, type;
    char err[256] = "";
    const char *end = gtk_editable_get_text(GTK_EDITABLE(state->bp_end));
    (void)wgt;
    if (sel >= G_N_ELEMENTS(types)) sel = 0;
    type = types[sel];
    if (phys && type == PMD85DEBUG_BP_EXEC) {
        gtk_label_set_text(state->bp_msg, "PHYS applies to read, write and access breakpoints");
        return;
    }
    if (!parse_addr(state, gtk_editable_get_text(GTK_EDITABLE(state->bp_start)), space, &start, err, sizeof err)) {
        gtk_label_set_text(state->bp_msg, err);
        return;
    }
    finish = start;
    if (end && *end && !parse_addr(state, end, space, &finish, err, sizeof err)) {
        gtk_label_set_text(state->bp_msg, err);
        return;
    }
    if (finish < start) {
        gtk_label_set_text(state->bp_msg, "The end comes before the start");
        return;
    }
    breakpoint_id = pmd85debug_breakpoint_add(state->dbg, type | (phys ? PMD85DEBUG_BP_PHYS : 0), (uint32_t)start,
                                  (uint32_t)finish, gtk_editable_get_text(GTK_EDITABLE(state->bp_cond)));
    if (breakpoint_id < 0) {
        gtk_label_set_text(state->bp_msg, (type & PMD85DEBUG_BP_IO)
            ? "The breakpoint was refused (a port is 0-FF; does the condition parse?)"
            : "The breakpoint was refused (does the condition parse?)");
        return;
    }
    gtk_label_set_text(state->bp_msg, "");
    gtk_editable_set_text(GTK_EDITABLE(state->bp_start), "");
    gtk_editable_set_text(GTK_EDITABLE(state->bp_end), "");
    gtk_editable_set_text(GTK_EDITABLE(state->bp_cond), "");
    refresh_now(state);
}

/* "Break at": a symbol or a hex address, then Enter, sets an execute
 * breakpoint there (once). */
static void on_break_at(GtkEntry *entry, gpointer user_data)
{
    DbgWin *state = user_data;
    char err[128] = "";
    long address;
    if (!parse_addr(state, gtk_editable_get_text(GTK_EDITABLE(entry)), PMD85DEBUG_SPACE_CPU, &address, err, sizeof err)) {
        toast(state, err);
        return;
    }
    if (!pmd85debug_breakpoint_is_set(state->dbg, (uint16_t)address))
        pmd85debug_breakpoint_toggle(state->dbg, (uint16_t)address);
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    refresh_now(state);
}

static void on_bp_clear(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)button;
    pmd85debug_breakpoint_clear_all(state->dbg);
    refresh_now(state);
}

static void on_bp_clear_hits(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)button;
    pmd85debug_breakpoint_clear_hits(state->dbg);
    refresh_now(state);
}

static void load_symbols(DbgWin *state, const char *path)
{
    char msg[600];
    const int count = pmd85debug_symbols_load(state->dbg, path);
    if (count < 0)
        g_snprintf(msg, sizeof msg, "Could not open the symbol file %s", path);
    else
        g_snprintf(msg, sizeof msg, "Loaded %d symbol%s from %s", count, count == 1 ? "" : "s", path);
    notice(state, msg);
    refresh_now(state);
}

static void on_clear_symbols(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)button;
    pmd85debug_symbols_clear(state->dbg);
    notice(state, "Symbols cleared (the built-ins stay)");
    refresh_now(state);
}

static void on_symbols_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    DbgWin *state = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    if (!file || state->closing) return;
    path = g_file_get_path(file);
    if (path) load_symbols(state, path);
}

static void on_symbols(GtkWidget *button, gpointer user_data)
{
    DbgWin *state = user_data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *syms = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    (void)button;

    gtk_file_filter_set_name(syms, "Symbol files (*.sym, *.map, *.txt)");
    gtk_file_filter_add_suffix(syms, "sym");
    gtk_file_filter_add_suffix(syms, "map");
    gtk_file_filter_add_suffix(syms, "txt");
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, syms);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, "Load Symbols (NAME EQU addr, z88dk .map, or NAME ADDR lines)");
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dlg, state->win, NULL, on_symbols_chosen, state);
    g_object_unref(syms);
    g_object_unref(all);
    g_object_unref(filters);
    g_object_unref(dlg);
}

/* Break at the symbol row picked in the Symbols tab. */
static void on_sym_break(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    GtkListBoxRow *row = gtk_list_box_get_selected_row(state->sym_list);
    (void)button;
    if (!row) {
        toast(state, "Select a symbol first");
        return;
    }
    {
        const uint16_t address = (uint16_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "addr"));
        if (!pmd85debug_breakpoint_is_set(state->dbg, address)) pmd85debug_breakpoint_toggle(state->dbg, address);
    }
    refresh_now(state);
}

static void on_io_on(GtkCheckButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    if (state->io_sync) return;
    pmd85debug_io_log_enable(state->dbg, gtk_check_button_get_active(button));
    refresh_iolog(state, TRUE);
}

static void on_io_clear(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)button;
    pmd85debug_io_log_clear(state->dbg);
    refresh_iolog(state, TRUE);
}

static void on_io_filter(GtkEditable *editable, gpointer user_data)
{
    (void)editable;
    refresh_iolog(user_data, TRUE);
}

/* How the disassembly reads: the mnemonics and the hex style, both kept by
 * the engine in the settings. */
static void on_syntax(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)pspec;
    if (state->closing) return;
    pmd85debug_set_syntax(state->dbg, (i8080_syntax)gtk_drop_down_get_selected(GTK_DROP_DOWN(object)));
    state->h_sig[0] = state->h_sig[1] = ~(uint64_t)0;
    state->io_sig = ~(uint64_t)0;
    state->bp_sig_reset = TRUE;
    refresh_now(state);
}

static void on_hexstyle(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)pspec;
    if (state->closing) return;
    pmd85debug_set_hex_style(state->dbg, (i8080_hex)gtk_drop_down_get_selected(GTK_DROP_DOWN(object)));
    state->h_sig[0] = state->h_sig[1] = ~(uint64_t)0;
    state->io_sig = ~(uint64_t)0;
    state->bp_sig_reset = TRUE;
    if (state->sym_sig) { g_string_free(state->sym_sig, TRUE); state->sym_sig = NULL; }
    refresh_now(state);
}

static void on_fuji_clear(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)button;
    pmd85debug_fujinet_trace_clear(state->dbg);
    refresh_fujinet(state, TRUE);
}

static void on_fuji_hide_ready(GtkCheckButton *button, gpointer user_data)
{
    (void)button;
    refresh_fujinet(user_data, TRUE);
}

/* The notebook also switches pages as it is taken apart with the window
 * (main window closing), before the window's "destroy": only a mapped window
 * has a tab to refresh, a hidden one refreshes when it is shown. */
static void on_switch_page(GtkNotebook *notebook, GtkWidget *page, guint num, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)notebook; (void)num;
    if (state->closing) return;
    if (gtk_widget_get_mapped(GTK_WIDGET(state->win))) {
        refresh_tab(state, page_tab(page));
    }
}

/* Record on either page is the one recorder. */
static void on_hist_rec(GtkToggleButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    if (state->h_sync) return;
    pmd85debug_trace_enable(state->dbg, gtk_toggle_button_get_active(button));
    state->h_sig[0] = state->h_sig[1] = ~(uint64_t)0;
    refresh_now(state);
}

static void on_hist_clear(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)button;
    pmd85debug_trace_clear(state->dbg);
    state->h_sig[0] = state->h_sig[1] = ~(uint64_t)0;
    refresh_now(state);
}

static void on_hist_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)object; (void)pspec;
    state->h_sig[0] = state->h_sig[1] = ~(uint64_t)0;
    refresh_now(state);
}

static void on_hist_activate(GtkEntry *entry, gpointer user_data)
{
    on_hist_changed(NULL, NULL, user_data);
    (void)entry;
}

static gboolean on_key(GtkEventControllerKey *controller, guint keyval, guint code,
                       GdkModifierType modifiers, gpointer user_data);

/* Puts a popped-out tab back in the notebook, at its place in the order. */
static void dock_tab(DbgWin *state, int tab, gboolean destroy)
{
    GtkWindow *win = state->detached[tab];
    GtkWidget *page = state->pages[tab], *vbox;
    int pos = 0, tab_index;

    if (!win) return;
    vbox = g_object_get_data(G_OBJECT(win), "vbox");
    g_object_ref(page);
    gtk_box_remove(GTK_BOX(vbox), page);
    state->detached[tab] = NULL;
    for (tab_index = 0; tab_index < tab; tab_index++)
        if (!state->detached[tab_index]) pos++;
    gtk_notebook_insert_page(state->notebook, page, gtk_label_new(k_tab_names[tab]), pos);
    g_object_unref(page);
    gtk_notebook_set_current_page(state->notebook, pos);
    if (destroy) gtk_window_destroy(win);
    if (!state->closing) refresh_now(state);
}

static void on_dock_button(GtkButton *button, gpointer user_data)
{
    dock_tab(user_data, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "tab")), TRUE);
}

static gboolean on_detached_close(GtkWindow *win, gpointer user_data)
{
    dock_tab(user_data, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(win), "tab")), FALSE);
    return FALSE;     /* let it close */
}

/* Any tab can move to a window of its own; one always stays. */
static void pop_out_tab(DbgWin *state, int tab)
{
    GtkWidget *page, *vbox, *header, *dock;
    GtkWindow *win;
    GtkEventController *keys;
    char title[96];

    if (tab < 0 || tab >= NTABS || state->detached[tab]) return;
    if (gtk_notebook_get_n_pages(state->notebook) <= 1) {
        toast(state, "One tab always stays in the debugger");
        return;
    }
    page = state->pages[tab];
    g_object_ref(page);
    gtk_notebook_remove_page(state->notebook, gtk_notebook_page_num(state->notebook, page));

    win = GTK_WINDOW(adw_window_new());
    g_snprintf(title, sizeof title, "PMD 85 Debugger - %s", k_tab_names[tab]);
    gtk_window_set_title(win, title);
    gtk_window_set_icon_name(win, pmd85_icon_name());
    gtk_window_set_default_size(win, 800, 560);
    gtk_window_set_transient_for(win, state->win);
    header = adw_header_bar_new();
    dock = gtk_button_new_with_label("Dock");
    gtk_widget_set_tooltip_text(dock, "Put this tab back in the debugger");
    g_object_set_data(G_OBJECT(dock), "tab", GINT_TO_POINTER(tab));
    g_signal_connect(dock, "clicked", G_CALLBACK(on_dock_button), state);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), dock);
    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(vbox), header);
    gtk_box_append(GTK_BOX(vbox), page);
    gtk_widget_set_vexpand(page, TRUE);
    adw_window_set_content(ADW_WINDOW(win), vbox);
    g_object_unref(page);
    g_object_set_data(G_OBJECT(win), "vbox", vbox);
    g_object_set_data(G_OBJECT(win), "tab", GINT_TO_POINTER(tab));
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), state);
    gtk_widget_add_controller(GTK_WIDGET(win), keys);
    g_signal_connect(win, "close-request", G_CALLBACK(on_detached_close), state);
    state->detached[tab] = win;
    gtk_window_present(win);
    refresh_now(state);
}

static void on_pop_out(GtkButton *button, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)button;
    pop_out_tab(state, current_tab(state));
}

/* Closing hides: the machine runs on, and the window is back on F12. */
static void hide_window(DbgWin *state)
{
    int tab;
    for (tab = 0; tab < NTABS; tab++)
        dock_tab(state, tab, TRUE);       /* popped-out tabs go home with the window */
    pmd85debug_set_engaged(state->dbg, 0);     /* releasing resumes the machine */
    gtk_widget_set_visible(GTK_WIDGET(state->win), FALSE);
}

static gboolean on_key(GtkEventControllerKey *controller, guint keyval, guint code,
                       GdkModifierType modifiers, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)code; (void)controller;
    switch (keyval) {
    case GDK_KEY_F5: on_run(NULL, state); return TRUE;
    case GDK_KEY_F7: on_step(NULL, state); return TRUE;
    case GDK_KEY_F8:
        if (modifiers & GDK_SHIFT_MASK) on_step_out(NULL, state); else on_step_over(NULL, state);
        return TRUE;
    case GDK_KEY_F9: toggle_selected(state); return TRUE;
    case GDK_KEY_F12: hide_window(state); return TRUE;
    default: return FALSE;
    }
}

static gboolean on_close(GtkWindow *win, gpointer user_data)
{
    (void)win;
    hide_window(user_data);
    return TRUE;
}

/* ---- construction --------------------------------------------------------- */

/* A horizontal row; flow_add appends to it. */
static GtkWidget *flow_row(void)
{
    return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
}

static void flow_add(GtkWidget *row, GtkWidget *child)
{
    gtk_box_append(GTK_BOX(row), child);
}

static GtkWidget *build_toolbar(DbgWin *state)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *stepping = flow_row();
    GtkWidget *tools = flow_row();
    GtkWidget *button;

    gtk_widget_set_margin_start(bar, 8);
    gtk_widget_set_margin_end(bar, 8);
    gtk_widget_set_margin_top(bar, 6);
    gtk_widget_set_margin_bottom(bar, 6);
#define TB(label, tip, callback, var) do { button = gtk_button_new_with_label(label); \
    gtk_widget_set_tooltip_text(button, tip); \
    g_signal_connect(button, "clicked", G_CALLBACK(callback), state); flow_add(stepping, button); \
    var = button; } while (0)
    GtkWidget *unused;
    state->run_btn = GTK_BUTTON(gtk_button_new_with_label("Pause (F5)"));
    g_signal_connect(state->run_btn, "clicked", G_CALLBACK(on_run), state);
    flow_add(stepping, GTK_WIDGET(state->run_btn));
    TB("Step (F7)", "One instruction", on_step, unused);
    TB("Over (F8)", "Over a subroutine call", on_step_over, unused);
    TB("Out (\xe2\x87\xa7""F8)", "Until this routine returns", on_step_out, unused);
    TB("To Cursor", "Run to the selected disassembly line", on_run_to_cursor, unused);
    TB("Clear BPs", "Remove every breakpoint", on_bp_clear, unused);
#undef TB
    (void)unused;

    /* Break at: a symbol or a hex address, then Enter */
    state->break_at = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->break_at, "symbol or hex");
    gtk_editable_set_width_chars(GTK_EDITABLE(state->break_at), 14);
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->break_at),
                                "Set an execute breakpoint at a symbol, a hex address or an expression");
    g_signal_connect(state->break_at, "activate", G_CALLBACK(on_break_at), state);
    flow_add(tools, labeled("Break at", GTK_WIDGET(state->break_at)));

    button = gtk_button_new_with_label("Symbols\xe2\x80\xa6");
    gtk_widget_set_tooltip_text(button, "Load a symbol file (NAME EQU addr, a z88dk .map, or NAME ADDR lines)");
    g_signal_connect(button, "clicked", G_CALLBACK(on_symbols), state);
    flow_add(tools, button);
    button = gtk_button_new_with_label("Clear Symbols");
    gtk_widget_set_tooltip_text(button, "Forget every loaded symbol; the monitor's stay");
    g_signal_connect(button, "clicked", G_CALLBACK(on_clear_symbols), state);
    flow_add(tools, button);
    {
        static const char *const syntaxes[] = { "Intel 8080", "Z80", NULL };
        static const char *const styles[] = { "C000h", "$C000", "0xC000", "#C000", NULL };
        state->syntax = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(syntaxes));
        gtk_widget_set_tooltip_text(GTK_WIDGET(state->syntax), "The mnemonics: Intel's own, or the Z80 spelling");
        gtk_drop_down_set_selected(state->syntax, (guint)pmd85debug_syntax(state->dbg));
        g_signal_connect(state->syntax, "notify::selected", G_CALLBACK(on_syntax), state);
        flow_add(tools, labeled("Mnemonics", GTK_WIDGET(state->syntax)));
        state->hexstyle = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(styles));
        gtk_widget_set_tooltip_text(GTK_WIDGET(state->hexstyle),
                                    "How hex numbers are shown (any of them is accepted when typed)");
        gtk_drop_down_set_selected(state->hexstyle, (guint)pmd85debug_hex_style(state->dbg));
        g_signal_connect(state->hexstyle, "notify::selected", G_CALLBACK(on_hexstyle), state);
        flow_add(tools, labeled("Hex", GTK_WIDGET(state->hexstyle)));
    }

    button = gtk_button_new_with_label("Pop Out");
    gtk_widget_set_tooltip_text(button, "Move the tab showing into a window of its own");
    gtk_widget_set_halign(button, GTK_ALIGN_END);
    gtk_widget_set_hexpand(button, TRUE);
    g_signal_connect(button, "clicked", G_CALLBACK(on_pop_out), state);
    flow_add(tools, button);
    gtk_box_append(GTK_BOX(bar), stepping);
    gtk_box_append(GTK_BOX(bar), tools);
    return bar;
}

static GtkWidget *build_console(DbgWin *state)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = mono_view(&state->console_out, TRUE);
    GtkEventController *keys;

    make_log(state->console_out);
    state->console_in = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->console_in,
        "Command (help, step, break, bpw, bpout, print, mem, poke, disasm, io, iolog, ...) "
        "\xe2\x80\x94 Tab completes, Up/Down recall");
    gtk_widget_set_hexpand(GTK_WIDGET(state->console_in), TRUE);
    g_signal_connect(state->console_in, "activate", G_CALLBACK(on_console), state);
    /* ahead of the entry's own text handling, so Tab and Up/Down are ours */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_console_key), state);
    gtk_widget_add_controller(GTK_WIDGET(state->console_in), keys);
    state->history = g_ptr_array_new_with_free_func(g_free);

    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(state->console_in));
    set_text_at(state->console_out, "PMD 85 debugger console (MHB8080A). Type 'help' for every command. "
                                "Numbers: C000h, $C000, 0xC000, #C000, 49152.\n", TRUE);
    return padded(box);
}

/* Under the notebook, on every tab: the registers (Enter sets one while the
 * machine is stopped), the CC flags and the machine. */
static GtkWidget *reg_entry(DbgWin *state, int index)
{
    state->reg[index] = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_max_length(state->reg[index], 6);
    gtk_editable_set_width_chars(GTK_EDITABLE(state->reg[index]), k_regs[index].digits + 1);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(state->reg[index]), k_regs[index].digits + 1);
    gtk_widget_add_css_class(GTK_WIDGET(state->reg[index]), "monospace");
    g_object_set_data(G_OBJECT(state->reg[index]), "reg", GINT_TO_POINTER(index));
    g_signal_connect(state->reg[index], "activate", G_CALLBACK(on_reg_activate), state);
    state->reg_box[index] = labeled(k_regs[index].name, GTK_WIDGET(state->reg[index]));
    return state->reg_box[index];
}

/* PC SP A F BC DE HL, then the flags and the machine. */
static GtkWidget *build_regs(DbgWin *state)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *flags = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    int index;
    for (index = 0; index < NREGS; index++)
        gtk_box_append(GTK_BOX(box), reg_entry(state, index));
    for (index = 0; index < NFLAGS; index++) {
        state->flag[index] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(k_flags[index].name));
        g_object_set_data(G_OBJECT(state->flag[index]), "flag", GINT_TO_POINTER(index));
        g_signal_connect(state->flag[index], "toggled", G_CALLBACK(on_flag_toggled), state);
        gtk_box_append(GTK_BOX(flags), GTK_WIDGET(state->flag[index]));
    }
    gtk_widget_set_tooltip_text(flags, "F: Sign, Zero, Auxiliary carry, Parity, CarrY "
                                       "(edit while stopped)");
    gtk_box_append(GTK_BOX(box), flags);
    state->machine = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(state->machine), "dim-label");
    gtk_widget_set_hexpand(GTK_WIDGET(state->machine), TRUE);
    gtk_label_set_xalign(state->machine, 1.0f);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(state->machine));
    return padded(box);
}

/* Find Next / Previous over the dump, with a note for what happened. */
static GtkWidget *build_search_bar(DbgWin *state)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *next = gtk_button_new_with_label("Find Next");
    GtkWidget *prev = gtk_button_new_with_label("Find Previous");
    static const char *const kinds[] = { "Hex", "Text", NULL };

    g_object_set_data(G_OBJECT(prev), "back", GINT_TO_POINTER(1));
    g_signal_connect(next, "clicked", G_CALLBACK(on_search), state);
    g_signal_connect(prev, "clicked", G_CALLBACK(on_search), state);
    state->srch_kind = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(kinds));
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->srch_kind), "Hex bytes (86 41) or text (one byte per character)");
    state->srch_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->srch_entry, "search the space showing: 86 41, or text");
    gtk_widget_set_hexpand(GTK_WIDGET(state->srch_entry), TRUE);
    g_object_set_data(G_OBJECT(state->srch_entry), "next-button", next);
    g_signal_connect(state->srch_entry, "activate", G_CALLBACK(on_search_activate), state);
    state->srch_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(state->srch_note), "dim-label");
    gtk_label_set_ellipsize(state->srch_note, PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(state->srch_note, 18);
    gtk_label_set_max_width_chars(state->srch_note, 18);
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Search"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(state->srch_kind));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(state->srch_entry));
    gtk_box_append(GTK_BOX(bar), next);
    gtk_box_append(GTK_BOX(bar), prev);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(state->srch_note));
    return bar;
}

static GtkWidget *build_memory(DbgWin *state)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *mem = mono_view(&state->mem_view, FALSE);
    GtkWidget *edit = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *page_up, *down;

    state->mem_goto = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(state->mem_goto), 16);
    gtk_entry_set_placeholder_text(state->mem_goto, "C000h / symbol / lo-hi");
    g_signal_connect(state->mem_goto, "activate", G_CALLBACK(on_mem_goto), state);
    state->mem_write = GTK_ENTRY(gtk_entry_new());
    gtk_editable_set_width_chars(GTK_EDITABLE(state->mem_write), 14);
    gtk_entry_set_placeholder_text(state->mem_write, "0100 41 42");
    g_signal_connect(state->mem_write, "activate", G_CALLBACK(on_mem_write), state);
    page_up = gtk_button_new_with_label("Page Up");
    down = gtk_button_new_with_label("Page Down");
    g_object_set_data(G_OBJECT(page_up), "delta", GINT_TO_POINTER(-1));
    g_object_set_data(G_OBJECT(down), "delta", GINT_TO_POINTER(1));
    g_signal_connect(page_up, "clicked", G_CALLBACK(on_mem_page), state);
    g_signal_connect(down, "clicked", G_CALLBACK(on_mem_page), state);
    state->mem_space = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(k_space_labels));
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->mem_space),
        "CPU: the 64K the CPU sees. PHYS: a CoCo 3's physical RAM (the GIME's MMU banks)");
    g_signal_connect(state->mem_space, "notify::selected", G_CALLBACK(on_mem_space), state);
    state->mem_space_box = labeled("Space", GTK_WIDGET(state->mem_space));
    gtk_widget_set_visible(state->mem_space_box, FALSE);     /* PHYS exists on a CoCo 3 only */
    gtk_box_append(GTK_BOX(edit), state->mem_space_box);
    gtk_box_append(GTK_BOX(edit), labeled("Go to", GTK_WIDGET(state->mem_goto)));
    gtk_box_append(GTK_BOX(edit), page_up);
    gtk_box_append(GTK_BOX(edit), down);
    gtk_box_append(GTK_BOX(edit), labeled("Poke (address bytes)", GTK_WIDGET(state->mem_write)));

    gtk_box_append(GTK_BOX(box), edit);
    gtk_box_append(GTK_BOX(box), build_search_bar(state));
    gtk_box_append(GTK_BOX(box), mem);
    /* the highlight covers a few bytes, not a paragraph */
    {
        char rgb[16], ink[16];
        g_snprintf(rgb, sizeof rgb, "#%06x", ACCENT_RGB);
        g_snprintf(ink, sizeof ink, "#%06x", ACCENT_INK_RGB);
        gtk_text_buffer_create_tag(gtk_text_view_get_buffer(state->mem_view), "mark",
                                   "background", rgb, "foreground", ink, NULL);
    }
    state->mem_hl = FALSE;
    return padded(box);
}

static GtkWidget *build_disasm(DbgWin *state)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *scroll = mono_view(&state->disasm, FALSE);
    GtkGesture *click = gtk_gesture_click_new();
    GtkEventController *scrollc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(state->disasm);
    GtkWidget *hint;

    /* later tags win: the selection under the PC line, the gutter marker
     * over both */
    gtk_text_buffer_create_tag(buffer, "dim", "foreground", "#8a8a8a", NULL);
    gtk_text_buffer_create_tag(buffer, "sel", "paragraph-background", "rgba(128,128,128,0.35)", NULL);
    accent_tag(state->disasm, "pc");
    gtk_text_buffer_create_tag(buffer, "bp_on", "foreground", "#e01b24", NULL);
    gtk_text_buffer_create_tag(buffer, "bp_off", "foreground", "#8a8a8a", NULL);

    state->follow_pc = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Follow PC"));
    gtk_check_button_set_active(state->follow_pc, TRUE);
    g_signal_connect(state->follow_pc, "toggled", G_CALLBACK(on_follow_toggled), state);
    state->jump = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->jump, "8000h / symbol / expression");
    gtk_editable_set_width_chars(GTK_EDITABLE(state->jump), 22);
    g_signal_connect(state->jump, "activate", G_CALLBACK(on_jump), state);
    hint = gtk_label_new("Click selects a line; double-click, F9 or the gutter toggles its "
                         "breakpoint; scroll to browse");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(state->follow_pc));
    gtk_box_append(GTK_BOX(row), labeled("Jump to", GTK_WIDGET(state->jump)));
    gtk_box_append(GTK_BOX(row), hint);

    g_signal_connect(click, "pressed", G_CALLBACK(on_disasm_click), state);
    gtk_widget_add_controller(GTK_WIDGET(state->disasm), GTK_EVENT_CONTROLLER(click));
    g_signal_connect(scrollc, "scroll", G_CALLBACK(on_disasm_scroll), state);
    gtk_widget_add_controller(GTK_WIDGET(state->disasm), scrollc);
    /* the wheel is ours; EXTERNAL (unlike NEVER) does not ask for a window
     * tall enough to show every one of the DISASM_WINDOW lines */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_EXTERNAL);

    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), scroll);
    state->disasm_top = 0;
    state->selected = -1;
    return padded(box);
}

static GtkWidget *build_breaks(DbgWin *state)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *add, *clear, *hits, *head, *hint;
    static const char *const types[] = { "Execute", "Read", "Write", "Access", "IN port",
                                         "OUT port", "IN/OUT port", NULL };

    state->bp_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(state->bp_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(state->bp_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    state->bp_type = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(types));
    state->bp_start = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->bp_start, "C000h / symbol / port");
    gtk_editable_set_width_chars(GTK_EDITABLE(state->bp_start), 22);
    state->bp_end = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->bp_end, "(end)");
    gtk_editable_set_width_chars(GTK_EDITABLE(state->bp_end), 8);
    state->bp_cond = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->bp_cond, "condition, e.g. a == FFh && value != old "
                                               "\xe2\x80\x94 optional");
    gtk_widget_set_hexpand(GTK_WIDGET(state->bp_cond), TRUE);
    g_signal_connect(state->bp_start, "activate", G_CALLBACK(on_bp_add), state);
    g_signal_connect(state->bp_end, "activate", G_CALLBACK(on_bp_add), state);
    g_signal_connect(state->bp_cond, "activate", G_CALLBACK(on_bp_add), state);
    add = gtk_button_new_with_label("Add");
    g_signal_connect(add, "clicked", G_CALLBACK(on_bp_add), state);
    clear = gtk_button_new_with_label("Clear All");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_bp_clear), state);
    state->bp_phys = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("PHYS"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->bp_phys),
        "The range is a CoCo 3 physical RAM address (read, write and access only)");
    gtk_widget_set_visible(GTK_WIDGET(state->bp_phys), FALSE);   /* a CoCo 3 only */
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(state->bp_type));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(state->bp_phys));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(state->bp_start));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(state->bp_end));
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(state->bp_cond));
    gtk_box_append(GTK_BOX(row), add);
    gtk_box_append(GTK_BOX(row), clear);
    state->bp_msg = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(state->bp_msg), "pmd85dbg-accent-text");
    gtk_label_set_xalign(state->bp_msg, 0);
    gtk_label_set_wrap(state->bp_msg, TRUE);
    hint = gtk_label_new("Numbers are hex. A condition is an expression like the console's "
                         "(a, hl, zf, [C000h]); `value`, `addr` (or `port`) and `old` are the "
                         "access. A read, write or port breakpoint stops right after the "
                         "instruction that made the access. A disabled execute breakpoint shows "
                         "as a grey hollow dot.");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);

    state->bp_stop = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(state->bp_stop, 0);
    gtk_widget_set_hexpand(GTK_WIDGET(state->bp_stop), TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(state->bp_stop), "heading");
    hits = gtk_button_new_with_label("Clear Hits");
    gtk_widget_set_tooltip_text(hits, "Zero every hit count");
    g_signal_connect(hits, "clicked", G_CALLBACK(on_bp_clear_hits), state);
    head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(head), GTK_WIDGET(state->bp_stop));
    gtk_box_append(GTK_BOX(head), hits);

    gtk_box_append(GTK_BOX(box), head);
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), row);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(state->bp_msg));
    gtk_box_append(GTK_BOX(box), hint);
    return padded(box);
}

static GtkWidget *build_watches(DbgWin *state)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *form = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *add;

    state->wt_labels = g_ptr_array_new();
    state->wt_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->wt_entry, "address, symbol, symbol+3, {ptr}, C000-C03F");
    gtk_widget_set_hexpand(GTK_WIDGET(state->wt_entry), TRUE);
    g_signal_connect(state->wt_entry, "activate", G_CALLBACK(on_wt_add), state);
    add = gtk_button_new_with_label("Add");
    g_signal_connect(add, "clicked", G_CALLBACK(on_wt_add), state);
    state->wt_space = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(k_space_labels));
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->wt_space), "The space the address is in (PHYS on a CoCo 3)");
    gtk_widget_set_visible(GTK_WIDGET(state->wt_space), FALSE);
    gtk_box_append(GTK_BOX(form), gtk_label_new("Watch"));
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(state->wt_space));
    gtk_box_append(GTK_BOX(form), GTK_WIDGET(state->wt_entry));
    gtk_box_append(GTK_BOX(form), add);

    state->wt_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(state->wt_list, GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(state->wt_list));
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), form);
    gtk_box_append(GTK_BOX(box), scroll);
    return padded(box);
}

/* The chips and the map left, the I/O log right. */
static GtkWidget *build_io(DbgWin *state)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    GtkWidget *start, *end;

    state->io_on = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Log"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->io_on),
                                "Record every IN and OUT while the debugger is open");
    g_signal_connect(state->io_on, "toggled", G_CALLBACK(on_io_on), state);
    state->io_filter = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->io_filter, "port or lo-hi, e.g. 3E-3F");
    gtk_editable_set_width_chars(GTK_EDITABLE(state->io_filter), 14);
    g_signal_connect(state->io_filter, "changed", G_CALLBACK(on_io_filter), state);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_io_clear), state);
    state->io_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(state->io_note), "dim-label");
    gtk_widget_set_hexpand(GTK_WIDGET(state->io_note), TRUE);
    gtk_label_set_xalign(state->io_note, 1.0f);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(state->io_on));
    gtk_box_append(GTK_BOX(row), labeled("Ports", GTK_WIDGET(state->io_filter)));
    gtk_box_append(GTK_BOX(row), clear);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(state->io_note));
    gtk_box_append(GTK_BOX(right), row);
    gtk_box_append(GTK_BOX(right), mono_view(&state->io_log, FALSE));
    make_log(state->io_log);
    state->io_sig = ~(uint64_t)0;

    start = headed("Machine, memory and chips", mono_view(&state->io_text, FALSE));
    end = headed("I/O log (T-state, PC, access, port name)", right);
    gtk_paned_set_start_child(GTK_PANED(paned), start);
    gtk_paned_set_end_child(GTK_PANED(paned), end);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(start, 520, -1);
    gtk_widget_set_size_request(end, 440, -1);
    return padded(paned);
}

/* A filter over every known symbol, built-in and loaded; a row opens the
 * disassembly at its address. */
static GtkWidget *build_symbols(DbgWin *state)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *scroll = gtk_scrolled_window_new();

    state->sym_filter = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->sym_filter, "filter by name");
    gtk_widget_set_hexpand(GTK_WIDGET(state->sym_filter), TRUE);
    g_signal_connect(state->sym_filter, "changed", G_CALLBACK(on_sym_filter_changed), state);
    state->sym_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(state->sym_note), "dim-label");
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Filter"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(state->sym_filter));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(state->sym_note));
    {
        static const struct { const char *text; GCallback callback; } btns[] = {
            { "Break at", G_CALLBACK(on_sym_break) },
            { "Load\xe2\x80\xa6", G_CALLBACK(on_symbols) },
            { "Clear", G_CALLBACK(on_clear_symbols) },
        };
        size_t slot;
        for (slot = 0; slot < G_N_ELEMENTS(btns); slot++) {
            GtkWidget *button = gtk_button_new_with_label(btns[slot].text);
            g_signal_connect(button, "clicked", btns[slot].callback, state);
            gtk_box_append(GTK_BOX(bar), button);
        }
    }

    state->sym_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(state->sym_list, GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(state->sym_list, FALSE);
    g_signal_connect(state->sym_list, "row-activated", G_CALLBACK(on_sym_row_activated), state);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(state->sym_list));
    gtk_widget_set_vexpand(scroll, TRUE);
    state->sym_info = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(state->sym_info), "dim-label");
    gtk_label_set_xalign(state->sym_info, 0);
    gtk_label_set_ellipsize(state->sym_info, PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(state->sym_info));
    gtk_box_append(GTK_BOX(box), scroll);
    return padded(box);
}

/* Trace (idx 0) and History (idx 1): the same controls over the one ring. */
static GtkWidget *build_hist(DbgWin *state, int idx)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = mono_view(&state->h_text[idx], FALSE);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    static const char *const filters[] = { "All instructions", "Branches only", "In range",
                                           "IN/OUT only", NULL };

    state->h_rec[idx] = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Record"));
    gtk_widget_add_css_class(GTK_WIDGET(state->h_rec[idx]), "pmd85dbg-toggle");
    gtk_widget_set_tooltip_text(GTK_WIDGET(state->h_rec[idx]),
                                "Record every instruction the CPU runs (the machine runs slower)");
    g_signal_connect(state->h_rec[idx], "toggled", G_CALLBACK(on_hist_rec), state);
    state->h_filter[idx] = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(filters));
    g_signal_connect(state->h_filter[idx], "notify::selected", G_CALLBACK(on_hist_changed), state);
    state->h_range[idx] = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(state->h_range[idx], "range: 8000-8FFF or a symbol");
    g_signal_connect(state->h_range[idx], "activate", G_CALLBACK(on_hist_activate), state);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_hist_clear), state);
    state->h_note[idx] = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(state->h_note[idx]), "dim-label");
    gtk_widget_set_hexpand(GTK_WIDGET(state->h_note[idx]), TRUE);
    gtk_label_set_xalign(state->h_note[idx], 1.0f);
    make_log(state->h_text[idx]);
    state->h_sig[idx] = ~(uint64_t)0;

    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(state->h_rec[idx]));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(state->h_filter[idx]));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(state->h_range[idx]));
    gtk_box_append(GTK_BOX(bar), clear);
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(state->h_note[idx]));
    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), scroll);
    return padded(box);
}

/* The packets left (a note and Clear over them), the BoIP link and its
 * counts right. */
static GtkWidget *build_fujinet(DbgWin *state)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *clear = gtk_button_new_with_label("Clear");
    GtkWidget *hide = gtk_check_button_new_with_label("Hide READY probes");
    GtkWidget *start, *end;

    state->fuji_hide_ready = GTK_CHECK_BUTTON(hide);
    gtk_check_button_set_active(state->fuji_hide_ready, TRUE);
    gtk_widget_set_tooltip_text(hide, "FujiNet CONFIG asks E2 00 before every call");
    g_signal_connect(hide, "toggled", G_CALLBACK(on_fuji_hide_ready), state);
    state->fuji_note = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(state->fuji_note), "dim-label");
    gtk_label_set_xalign(state->fuji_note, 0);
    gtk_label_set_ellipsize(state->fuji_note, PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(GTK_WIDGET(state->fuji_note), TRUE);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_fuji_clear), state);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(state->fuji_note));
    gtk_box_append(GTK_BOX(row), hide);
    gtk_box_append(GTK_BOX(row), clear);
    gtk_box_append(GTK_BOX(left), row);
    gtk_box_append(GTK_BOX(left), mono_view(&state->fuji_text, FALSE));
    make_log(state->fuji_text);

    start = headed("DriveWire packets", left);
    end = headed("Becker port", mono_view(&state->fuji_state, FALSE));
    gtk_paned_set_start_child(GTK_PANED(paned), start);
    gtk_paned_set_end_child(GTK_PANED(paned), end);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), FALSE);
    gtk_widget_set_size_request(start, 560, -1);
    gtk_widget_set_size_request(end, 340, -1);
    state->fuji_count = -1;
    return padded(paned);
}

/* The state outlives the window: the widgets go before the window's
 * "destroy" and can still call back (the notebook's page switches), so it is
 * freed only once the window object itself is gone. */
static void on_win_finalized(gpointer data, GObject *where)
{
    DbgWin *state = data;
    (void)where;
    if (state->history) g_ptr_array_unref(state->history);
    if (state->bp_sig) g_string_free(state->bp_sig, TRUE);
    if (state->wt_labels) g_ptr_array_unref(state->wt_labels);
    if (state->sym_sig) g_string_free(state->sym_sig, TRUE);
    g_free(state);
}

static void on_destroy(GtkWidget *widget, gpointer user_data)
{
    DbgWin *state = user_data;
    (void)widget;
    state->closing = TRUE;
    if (state->timer) g_source_remove(state->timer);
    state->timer = 0;
    pmd85debug_set_engaged(state->dbg, 0);
    if (g_win == state) g_win = NULL;
}

static void attach_and_refresh(DbgWin *state)
{
    pmd85debug_set_engaged(state->dbg, 1);
    pmd85debug_pause(state->dbg);        /* a user opening a debugger expects it stopped */
    settle(state, TRUE);
    state->running_ticks = 0;
    if (state->bp_sig) { g_string_free(state->bp_sig, TRUE); state->bp_sig = NULL; }
    refresh_now(state);
}

/* A tab from an environment variable (COCO_DEBUGGER_TAB, COCO_POPOUT): a
 * number, or a tab's name or the start of one, any case. */
static int tab_from_env(const char *var)
{
    static const struct { const char *name; int tab; } aliases[] = {
        { "cpu", TAB_DISASM }, { "disasm", TAB_DISASM }, { "io", TAB_IO }, { "iolog", TAB_IO },
        { "breaks", TAB_BREAKS }, { "mem", TAB_MEMORY },
        { "regs", TAB_CONSOLE },
    };
    const char *tab = g_getenv(var);
    char *end;
    long count;
    unsigned index;
    if (!tab || !*tab) return -1;
    count = strtol(tab, &end, 10);
    if (end != tab && *end == '\0')
        return (count >= 0 && count < (long)G_N_ELEMENTS(k_tab_names)) ? (int)count : -1;
    for (index = 0; index < G_N_ELEMENTS(aliases); index++)
        if (g_ascii_strcasecmp(tab, aliases[index].name) == 0)
            return aliases[index].tab;
    for (index = 0; index < G_N_ELEMENTS(k_tab_names); index++)
        if (g_ascii_strncasecmp(tab, k_tab_names[index], strlen(tab)) == 0)
            return (int)index;
    for (index = 0; index < G_N_ELEMENTS(aliases); index++)
        if (g_ascii_strncasecmp(tab, aliases[index].name, strlen(tab)) == 0)
            return aliases[index].tab;
    return -1;
}

void pmd85_debugger_show(GtkWindow *parent, pmd85session *session)
{
    DbgWin *state;
    GtkWidget *toolbar, *header, *root, *notebook, *overlay;
    GtkEventController *keys;
    pmd85debug *dbg;
    int tab, index;

    if (g_win) {
        gboolean shown = gtk_widget_get_visible(GTK_WIDGET(g_win->win));
        gtk_window_present(g_win->win);
        if (!shown) attach_and_refresh(g_win);
        return;
    }
    dbg = pmd85session_debugger(session);
    if (!dbg) {
        AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
            "Debugger Unavailable", "The debugger could not be created for this session."));
        adw_alert_dialog_add_response(dialog, "close", "_Close");
        adw_alert_dialog_set_default_response(dialog, "close");
        adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
        return;
    }
    install_css();
    state = g_new0(DbgWin, 1);
    state->session = session;
    state->dbg = dbg;
    g_win = state;

    state->win = GTK_WINDOW(adw_window_new());
    g_object_weak_ref(G_OBJECT(state->win), on_win_finalized, state);
    gtk_window_set_title(state->win, "Debugger");
    gtk_window_set_icon_name(state->win, pmd85_icon_name());
    gtk_window_set_default_size(state->win, 1150, 800);
    gtk_window_set_transient_for(state->win, parent);
    gtk_window_set_application(state->win, gtk_window_get_application(parent));
    gtk_window_set_destroy_with_parent(state->win, TRUE);
    g_signal_connect(state->win, "close-request", G_CALLBACK(on_close), state);
    g_signal_connect(state->win, "destroy", G_CALLBACK(on_destroy), state);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), build_toolbar(state));
    notebook = gtk_notebook_new();
    state->notebook = GTK_NOTEBOOK(notebook);
    gtk_widget_set_vexpand(notebook, TRUE);
    state->pages[TAB_CONSOLE] = build_console(state);
    state->pages[TAB_DISASM] = build_disasm(state);
    state->pages[TAB_MEMORY] = build_memory(state);
    state->pages[TAB_BREAKS] = build_breaks(state);
    state->pages[TAB_WATCHES] = build_watches(state);
    state->pages[TAB_IO] = build_io(state);
    state->pages[TAB_TRACE] = build_hist(state, 0);
    state->pages[TAB_HISTORY] = build_hist(state, 1);
    state->pages[TAB_SYMBOLS] = build_symbols(state);
    state->pages[TAB_FUJINET] = build_fujinet(state);
    for (index = 0; index < NTABS; index++) {
        g_object_set_data(G_OBJECT(state->pages[index]), "tab", GINT_TO_POINTER(index));
        gtk_notebook_append_page(state->notebook, state->pages[index], gtk_label_new(k_tab_names[index]));
    }
    gtk_box_append(GTK_BOX(root), notebook);
    gtk_box_append(GTK_BOX(root), build_regs(state));
    tab = tab_from_env("PMD85_DEBUGGER_TAB");
    if (tab >= 0) gtk_notebook_set_current_page(state->notebook, tab);
    g_signal_connect(notebook, "switch-page", G_CALLBACK(on_switch_page), state);

    overlay = adw_toast_overlay_new();
    state->toasts = ADW_TOAST_OVERLAY(overlay);
    adw_toast_overlay_set_child(state->toasts, root);

    /* the status line -- why the machine stopped -- is the title's subtitle */
    header = adw_header_bar_new();
    state->title = ADW_WINDOW_TITLE(adw_window_title_new("Debugger", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(state->title));
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), overlay);
    adw_window_set_content(ADW_WINDOW(state->win), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), state);
    gtk_widget_add_controller(GTK_WIDGET(state->win), keys);

    attach_and_refresh(state);
    /* captures: a watch, a break-on-change, a Go to, recording, a popped-out tab */
    {
        const char *watch_text = g_getenv("PMD85_WATCH"), *watch_break = g_getenv("PMD85_WATCH_BREAK"),
                   *mem_goto_text = g_getenv("PMD85_MEM_GOTO");
        uint32_t first, last;
        if (watch_text && *watch_text) {        /* a comma-separated list */
            char **entries = g_strsplit(watch_text, ",", -1);
            int entry;
            for (entry = 0; entries[entry]; entry++) {
                const char *text = g_strstrip(entries[entry]);
                if (*text) pmd85watch_add(text, PMD85DEBUG_SPACE_CPU);
            }
            g_strfreev(entries);
        }
        if (watch_break && *watch_break &&
            pmd85sym_parse_range(state->dbg, PMD85DEBUG_SPACE_CPU, watch_break, &first, &last))
            break_on_change(state, PMD85DEBUG_SPACE_CPU, first, last);
        if (mem_goto_text && *mem_goto_text &&
            pmd85sym_parse_range(state->dbg, PMD85DEBUG_SPACE_CPU, mem_goto_text, &first, &last))
            show_in_memory(state, PMD85DEBUG_SPACE_CPU, first, last);
        if (g_getenv("PMD85_HISTORY")) pmd85debug_trace_enable(state->dbg, 1);
    }
    g_signal_connect(state->win, "map", G_CALLBACK(on_win_map), state);
    g_signal_connect(state->win, "unmap", G_CALLBACK(on_win_unmap), state);
    gtk_window_present(state->win);
    if (current_tab(state) == TAB_CONSOLE)
        gtk_widget_grab_focus(GTK_WIDGET(state->console_in));
    tab = tab_from_env("PMD85_POPOUT");
    if (tab >= 0) pop_out_tab(state, tab);
}

/* F12 and the menu item: show it, or hide it if it is showing. */
void pmd85_debugger_toggle(GtkWindow *parent, pmd85session *session)
{
    if (g_win && gtk_widget_get_visible(GTK_WIDGET(g_win->win)))
        hide_window(g_win);
    else
        pmd85_debugger_show(parent, session);
}

