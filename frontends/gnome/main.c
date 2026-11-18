/*
 * FujiNet Go PMD 85 -- GNOME frontend entry point.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <adwaita.h>
#ifndef G_OS_WIN32
#include <glib-unix.h>
#include <signal.h>
#endif

#include "pmd85session.h"
#include "window.h"

static pmd85session *g_session;

/* The installed icons are named after the desktop-entry id. Running straight
 * out of the build tree there is nothing installed to look up, so point the
 * icon theme at the in-tree artwork (named for the project, not the id) and
 * report that name instead. */
const char *pmd85_icon_name(void)
{
    static const char *name;
    GtkIconTheme *theme;

    if (name)
        return name;

    name = PMD85_APP_ID;
    theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    if (theme && !gtk_icon_theme_has_icon(theme, name)) {
        gtk_icon_theme_add_search_path(theme, PMD85_SOURCE_ICON_DIR);
        if (gtk_icon_theme_has_icon(theme, "fujinet-go-pmd85"))
            name = "fujinet-go-pmd85";
    }
    return name;
}

/* The log, tape and debugger windows belong to the application too (so the
 * shell groups them with it), which means the application would outlive its
 * main window and run on behind a stray log window. Closing the main window
 * is quitting, as in the Atari 800 app. */
static gboolean on_main_close(GtkWindow *win, gpointer user_data)
{
    (void)win;
    g_application_quit(G_APPLICATION(user_data));
    return FALSE;
}

/* SIGTERM (a logout, a service manager) and SIGINT (Ctrl+C in the terminal)
 * quit the way closing the window does, so the session stops the machine
 * and FujiNet and saves the settings instead of dying mid-write. */
static gboolean on_quit_signal(gpointer user_data)
{
    g_application_quit(G_APPLICATION(user_data));
    return G_SOURCE_CONTINUE;
}

static void on_activate(AdwApplication *app, gpointer user_data)
{
    GtkWindow *win;
    (void)user_data;

    win = gtk_application_get_active_window(GTK_APPLICATION(app));
    if (win) {
        gtk_window_present(win);
        return;
    }

    if (!g_session) {
        pmd85session_start_opts opts;
        g_session = pmd85session_new(NULL);
        if (!g_session) {
            g_printerr("fatal: could not create the session\n");
            g_application_quit(G_APPLICATION(app));
            return;
        }
        pmd85session_default_opts(g_session, &opts);
        if (pmd85session_start(g_session, &opts) != 0)
            g_printerr("session start: %s\n",
                       pmd85session_last_error(g_session));
    }

    win = GTK_WINDOW(pmd85_window_new(app, g_session));
    gtk_window_set_icon_name(win, pmd85_icon_name());
    g_signal_connect(win, "close-request", G_CALLBACK(on_main_close), app);
    gtk_window_present(win);
}

static void on_shutdown(GApplication *app, gpointer user_data)
{
    (void)app;
    (void)user_data;
    if (g_session) {
        pmd85session_free(g_session);
        g_session = NULL;
    }
}

int main(int argc, char *argv[])
{
    g_autoptr(AdwApplication) app = adw_application_new(
        PMD85_APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);
#ifndef G_OS_WIN32
    g_unix_signal_add(SIGTERM, on_quit_signal, app);
    g_unix_signal_add(SIGINT, on_quit_signal, app);
#endif
    return g_application_run(G_APPLICATION(app), argc, argv);
}
