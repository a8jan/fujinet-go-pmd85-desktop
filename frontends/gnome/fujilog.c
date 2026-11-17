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

/* ---- console log --------------------------------------------------------- */

typedef struct {
    pmd85session *session;
    GtkTextView *view;
    GtkTextMark *end_mark;  /* at the buffer's end, right gravity */
    gboolean filled;        /* the first fill has happened */
    guint timer;
} LogState;

static gboolean log_refresh(gpointer user_data)
{
    LogState *st = user_data;
    static char buf[128 * 1024];
    static char shown[128 * 1024];
    GtkTextBuffer *tb = gtk_text_view_get_buffer(st->view);
    GtkAdjustment *adj =
        gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(st->view));
    int n = pmd85session_fujinet_copy_log(st->session, buf, sizeof(buf));
    const char *text = n > 0 ? buf : "(no FujiNet output yet)";
    double value = 0;
    gboolean at_end = TRUE;

    /* Unchanged: leave the view alone (keeps the scroll position and any
     * selection). */
    if (st->filled && strcmp(text, shown) == 0)
        return G_SOURCE_CONTINUE;
    g_strlcpy(shown, text, sizeof(shown));

    /* Follow the tail only while the reader is already at the bottom (the
     * first fill starts there); otherwise keep them where they were. */
    if (adj && st->filled) {
        value = gtk_adjustment_get_value(adj);
        at_end = value >= gtk_adjustment_get_upper(adj) -
                              gtk_adjustment_get_page_size(adj) - 1.0;
    }
    gtk_text_buffer_set_text(tb, text, -1);
    if (at_end)
        gtk_text_view_scroll_to_mark(st->view, st->end_mark, 0, FALSE, 0, 1.0);
    else if (adj)
        gtk_adjustment_set_value(adj, value);
    st->filled = TRUE;
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
    {
        GtkTextBuffer *tb = gtk_text_view_get_buffer(st->view);
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(tb, &end);
        st->end_mark = gtk_text_buffer_create_mark(tb, NULL, &end, FALSE);
    }
    st->timer = g_timeout_add_seconds(1, log_refresh, st);
    g_signal_connect(win, "destroy", G_CALLBACK(log_destroyed), st);
    log_refresh(st);
    gtk_window_present(win);
}
