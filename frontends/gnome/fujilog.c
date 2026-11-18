/*
 * The FujiNet web UI (127.0.0.1:64003 for this app), opened in the system
 * browser, and a console log window streaming the runtime's captured output.
 *
 * The web UI is not embedded (as in the Atari 800 app): its Google Drive and
 * OneDrive Authorize buttons open the provider's consent page in a new tab,
 * which an embedded view drops, and both providers refuse OAuth from an
 * embedded web view anyway.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fujilog.h"

#include <string.h>

/* ---- the web UI ----------------------------------------------------------- */

void pmd85_fujinet_config_show(GtkWindow *parent, pmd85session *session)
{
    GtkUriLauncher *launcher =
        gtk_uri_launcher_new(pmd85session_fujinet_webui_url(session));
    gtk_uri_launcher_launch(launcher, parent, NULL, NULL, NULL);
    g_object_unref(launcher);
}

/* ---- console log ----------------------------------------------------------
 * Appended to as FujiNet writes (the Atari 800 app's scheme): the view is
 * never rebuilt, so the reader's scroll position and selection stay where
 * they are, and only a reader already at the bottom follows new text. A
 * restart of FujiNet (a new log) clears the view. */

typedef struct {
    pmd85session *session;
    GtkTextView *view;
    GtkScrolledWindow *scroll;
    pmd85_log_cursor cursor;  /* where in FujiNet's log the view has got to */
    gboolean placeholder;     /* the "no output yet" line is on show, not log text */
    gboolean follow;          /* pin the end as the view grows */
    guint timer;
} LogState;

/* a long session keeps the last LOG_KEEP characters, whole lines */
#define LOG_KEEP (256 * 1024)

/* GtkTextView lays new text out later and moves the adjustment itself, so the
 * end is pinned whenever the height changes; whether to follow is decided as
 * text arrives. */
static void log_resized(GtkAdjustment *adj, gpointer user_data)
{
    LogState *st = user_data;
    if (st->follow)
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) -
                                          gtk_adjustment_get_page_size(adj));
}

static gboolean log_refresh(gpointer user_data)
{
    LogState *st = user_data;
    static char buf[64 * 1024];
    GtkTextBuffer *b = gtk_text_view_get_buffer(st->view);
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(st->scroll);
    GtkTextIter it;
    int n, reset, rounds = 0;

    do {
        char *clean;
        n = pmd85session_fujinet_read_log(st->session, &st->cursor, buf, sizeof buf, &reset);
        if (n <= 0 && !reset)
            break;

        if (reset || st->placeholder) {
            gtk_text_buffer_set_text(b, "", -1);
            st->placeholder = FALSE;
            st->follow = TRUE;
        } else {
            /* follow the tail only while the reader is at the bottom (or the
             * view has not been laid out yet) */
            st->follow = !adj || gtk_adjustment_get_page_size(adj) <= 0.0 ||
                         gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >=
                             gtk_adjustment_get_upper(adj) - 2.0;
        }

        clean = g_utf8_make_valid(buf, -1);
        gtk_text_buffer_get_end_iter(b, &it);
        gtk_text_buffer_insert(b, &it, clean, -1);
        g_free(clean);

        {
            int count = gtk_text_buffer_get_char_count(b);
            if (count > LOG_KEEP) {
                GtkTextIter s0, e0;
                gtk_text_buffer_get_start_iter(b, &s0);
                gtk_text_buffer_get_iter_at_offset(b, &e0, count - LOG_KEEP * 3 / 4);
                gtk_text_iter_forward_line(&e0);
                gtk_text_buffer_delete(b, &s0, &e0);
            }
        }
        if (st->follow && adj)
            log_resized(adj, st);
    } while (n >= (int)sizeof buf - 1 && ++rounds < 16);
    return G_SOURCE_CONTINUE;
}

static void log_destroyed(GtkWidget *win, gpointer user_data)
{
    LogState *st = user_data;
    (void)win;
    g_source_remove(st->timer);
    g_free(st);
}

void pmd85_fujinet_log_show(GtkWindow *parent, pmd85session *session)
{
    static GtkWindow *win;
    LogState *st;
    GtkWidget *scroll, *view, *tbview;

    if (win) {
        gtk_window_present(win);
        return;
    }
    win = GTK_WINDOW(adw_window_new());
    g_object_add_weak_pointer(G_OBJECT(win), (gpointer *)&win);
    gtk_window_set_title(win, "FujiNet Console Log");
    gtk_window_set_default_size(win, 820, 560);
    gtk_window_set_transient_for(win, parent);
    /* one of the app's windows: grouped with the main one, with its icon */
    gtk_window_set_application(win, gtk_window_get_application(parent));

    view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    tbview = adw_toolbar_view_new();
    {
        GtkWidget *header = adw_header_bar_new();
        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tbview), header);
    }
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tbview), scroll);
    adw_window_set_content(ADW_WINDOW(win), tbview);

    st = g_new0(LogState, 1);
    st->session = session;
    st->view = GTK_TEXT_VIEW(view);
    st->scroll = GTK_SCROLLED_WINDOW(scroll);
    st->follow = TRUE;
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(st->view), "(no FujiNet output yet)", -1);
    st->placeholder = TRUE;
    g_signal_connect(gtk_scrolled_window_get_vadjustment(st->scroll), "changed",
                     G_CALLBACK(log_resized), st);
    st->timer = g_timeout_add(1000, log_refresh, st);
    g_signal_connect(win, "destroy", G_CALLBACK(log_destroyed), st);
    log_refresh(st);
    gtk_window_present(win);
}
