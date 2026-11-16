/*
 * Pmd85Display: paints the emulator's latest XRGB8888 frame with a
 * GdkMemoryTexture (rebuilt from the previous one so the GPU texture is
 * reused, frames copied once into a pooled buffer), letterboxed to the chosen aspect inside whatever size
 * the window manager gives us (tiling or floating). A frame-clock tick
 * callback, running only while the window is mapped and not suspended,
 * doubles as the vsync source for the session's phase-lock:
 * GTK4's GPU renderer presents on the compositor's vsync, so pacing the
 * emulator on these ticks is pacing it on display vsync.
 *
 * The core's geometry is not fixed -- the Consul 2717's extended mode is 384
 * pixels wide where every other model is 288 -- so the frame's dimensions
 * come back with every copy and the texture is rebuilt to match.
 *
 * The PMD runs at 50 Hz, so the vsync ticks this forwards never phase-lock
 * the emulator (see session.c); they are kept so the widget stays the same
 * as the family's.
 *
 * Scaling. The 4:3 picture needs a non-integer stretch (288 pixels into, say,
 * 1024), which nearest-neighbour can only do with pixels of uneven width, and
 * a linear filter blurs every pixel edge across several screen pixels. The
 * default "sharp" filter does both in turn: the frame is blown up on the CPU
 * by the largest whole factor per axis that still fits (capped, so a 4K
 * screen does not upload tens of megabytes a frame), and GSK's linear filter
 * covers only the remaining fraction -- every emulated pixel the same size,
 * with at most a screen pixel or two of blend at its edge. The picture's
 * rectangle is snapped to whole device pixels (the surface's real, possibly
 * fractional scale), so its edges and the integer mode's pixels land on the
 * screen's grid.
 *
 * Copyright (C) 2026 Thomas Cherryhomes, FujiNet Go contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "display.h"

#include <math.h>
#include <string.h>

/* ---- frame buffer pool ----------------------------------------------------
 * The session copies a frame straight into a pooled buffer and the texture
 * borrows that buffer: one copy per frame, no allocation in steady state
 * (a ~1 MB malloc is an mmap/munmap pair every frame). The buffer comes
 * back to the pool when GSK drops the texture's bytes, possibly after the
 * widget is gone, so the pool is reference counted and each outstanding
 * buffer holds a reference. */
typedef struct {
    gatomicrefcount refs;
    GMutex lock;
    GPtrArray *spare;      /* idle buffers of `size` bytes each */
    gsize size;
} FramePool;

typedef struct {
    FramePool *pool;
    guint32 *px;
} FrameLoan;

#define POOL_KEEP 4

static FramePool *pool_new(gsize size)
{
    FramePool *p = g_new0(FramePool, 1);
    p->size = size;
    g_atomic_ref_count_init(&p->refs);
    g_mutex_init(&p->lock);
    p->spare = g_ptr_array_new_with_free_func(g_free);
    return p;
}

static void pool_unref(FramePool *p)
{
    if (!g_atomic_ref_count_dec(&p->refs))
        return;
    g_ptr_array_free(p->spare, TRUE);
    g_mutex_clear(&p->lock);
    g_free(p);
}

static guint32 *pool_take(FramePool *p)
{
    guint32 *px = NULL;

    g_mutex_lock(&p->lock);
    if (p->spare->len > 0)
        px = g_ptr_array_steal_index(p->spare, p->spare->len - 1);
    g_mutex_unlock(&p->lock);
    return px ? px : g_malloc(p->size);
}

static void pool_give(FramePool *p, guint32 *px)
{
    g_mutex_lock(&p->lock);
    if (p->spare->len < POOL_KEEP) {
        g_ptr_array_add(p->spare, px);
        px = NULL;
    }
    g_mutex_unlock(&p->lock);
    g_free(px);
}

static void loan_release(gpointer data)
{
    FrameLoan *loan = data;

    pool_give(loan->pool, loan->px);
    pool_unref(loan->pool);
    g_free(loan);
}

struct _Pmd85Display {
    GtkWidget parent_instance;

    pmd85session *session;
    GdkTexture *texture;
    const guint32 *px;     /* the texture's pixels, alive as long as it is */
    FramePool *pool;
    int fb_w;
    int fb_h;
    guint frame_gen;       /* bumped with every new frame */

    /* the sharp filter's integer blow-up of the current frame */
    GdkTexture *big;
    FramePool *big_pool;
    guint big_gen;
    int big_kx, big_ky;

    uint64_t serial;
    guint tick_id;
    gulong suspended_id;   /* notify::suspended on the toplevel, while mapped */
    GtkWindow *toplevel;
    Pmd85AspectMode aspect_mode;
    Pmd85Scaling scaling;
};

/* Largest blow-up per axis for the sharp filter. Past this the leftover
 * linear stretch is under 1/6 of an emulated pixel, and the upload stays
 * around 10 MB a frame even fullscreen on a 4K screen. */
#define SHARP_MAX_FACTOR 6

G_DEFINE_FINAL_TYPE(Pmd85Display, pmd85_display, GTK_TYPE_WIDGET)

/* The core's XRGB8888 is a uint32 0x00RRGGBB, so in memory on a
 * little-endian host the bytes run B,G,R,X -- which is exactly
 * GDK_MEMORY_B8G8R8X8, and no per-pixel conversion is needed. On a
 * big-endian host the same uint32 lays out X,R,G,B. */
#if G_BYTE_ORDER == G_BIG_ENDIAN
#define PMD85_GDK_FORMAT GDK_MEMORY_X8R8G8B8
#else
#define PMD85_GDK_FORMAT GDK_MEMORY_B8G8R8X8
#endif

/* Wrap px (w x h XRGB8888, a buffer from pool) in a texture that replaces
 * *slot. The previous texture, when it was the same size, goes in as the
 * update source so GSK can reuse its GPU texture instead of making a new
 * one. Takes ownership of px. */
static void build_texture(GdkTexture **slot, FramePool *pool, guint32 *px,
                          int w, int h)
{
    GdkMemoryTextureBuilder *b = gdk_memory_texture_builder_new();
    FrameLoan *loan = g_new(FrameLoan, 1);
    GBytes *bytes;

    g_atomic_ref_count_inc(&pool->refs);
    loan->pool = pool;
    loan->px = px;
    bytes = g_bytes_new_with_free_func(px, (gsize)w * h * sizeof(guint32),
                                       loan_release, loan);
    gdk_memory_texture_builder_set_bytes(b, bytes);
    gdk_memory_texture_builder_set_stride(b, (gsize)w * 4);
    gdk_memory_texture_builder_set_width(b, w);
    gdk_memory_texture_builder_set_height(b, h);
    gdk_memory_texture_builder_set_format(b, PMD85_GDK_FORMAT);
    if (*slot && gdk_texture_get_width(*slot) == w &&
        gdk_texture_get_height(*slot) == h) {
        cairo_rectangle_int_t all = { 0, 0, w, h };
        cairo_region_t *region = cairo_region_create_rectangle(&all);

        gdk_memory_texture_builder_set_update_texture(b, *slot);
        gdk_memory_texture_builder_set_update_region(b, region);
        cairo_region_destroy(region);
    }
    g_clear_object(slot);
    *slot = gdk_memory_texture_builder_build(b);
    g_bytes_unref(bytes);
    g_object_unref(b);
}

/* The current frame blown up kx by ky, nearest-neighbour, rebuilt only when
 * the frame or the factors change. */
static GdkTexture *sharp_texture(Pmd85Display *self, int kx, int ky)
{
    const int sw = self->fb_w, sh = self->fb_h;
    const int bw = sw * kx, bh = sh * ky;
    guint32 *dst;
    int x, y, i;

    if (self->big && self->big_gen == self->frame_gen &&
        self->big_kx == kx && self->big_ky == ky)
        return self->big;

    if (!self->big_pool || self->big_pool->size != (gsize)bw * bh * sizeof(guint32)) {
        if (self->big_pool) pool_unref(self->big_pool);
        self->big_pool = pool_new((gsize)bw * bh * sizeof(guint32));
    }
    dst = pool_take(self->big_pool);
    for (y = 0; y < sh; y++) {
        const guint32 *src = self->px + (gsize)y * sw;
        guint32 *row = dst + (gsize)y * ky * bw;
        guint32 *o = row;
        for (x = 0; x < sw; x++)
            for (i = 0; i < kx; i++)
                *o++ = src[x];
        for (i = 1; i < ky; i++)
            memcpy(row + (gsize)i * bw, row, (gsize)bw * sizeof(guint32));
    }
    build_texture(&self->big, self->big_pool, dst, bw, bh);
    self->big_gen = self->frame_gen;
    self->big_kx = kx;
    self->big_ky = ky;
    return self->big;
}

static gboolean tick_cb(GtkWidget *widget, GdkFrameClock *clock,
                        gpointer user_data)
{
    Pmd85Display *self = PMD85_DISPLAY(widget);
    guint32 *px;
    int w = 0, h = 0;
    (void)user_data;

    if (!self->session)
        return G_SOURCE_CONTINUE;

    /* Frame-clock time is CLOCK_MONOTONIC microseconds. */
    pmd85session_notify_vsync(self->session,
                               gdk_frame_clock_get_frame_time(clock) * 1000);

    px = pool_take(self->pool);
    if (pmd85session_copy_frame(self->session, px, &self->serial, &w, &h)
        && w > 0 && h > 0) {
        build_texture(&self->texture, self->pool, px, w, h);
        self->px = px;
        self->fb_w = w;
        self->fb_h = h;
        self->frame_gen++;
        gtk_widget_queue_draw(widget);
    } else {
        pool_give(self->pool, px);
    }
    return G_SOURCE_CONTINUE;
}

/* The tick runs only while somebody can see the result. A hidden or
 * suspended window gets no frame callbacks from the compositor anyway, but a
 * minimised one on some stacks keeps ticking, and every tick is a wakeup.
 * The emulator does not care: with no ticks the vsync stream goes stale
 * after a quarter second and the session paces on the wall clock. The first
 * tick after the window comes back copies whatever frame is newest (the
 * serial is kept, so an unchanged picture stays on screen). */
static void update_tick(Pmd85Display *self)
{
    gboolean want = gtk_widget_get_mapped(GTK_WIDGET(self)) &&
                    !(self->toplevel && gtk_window_is_suspended(self->toplevel));

    if (want && !self->tick_id)
        self->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(self), tick_cb,
                                                     NULL, NULL);
    else if (!want && self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
}

static void on_suspended(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    (void)obj; (void)pspec;
    update_tick(PMD85_DISPLAY(user_data));
}

static void pmd85_display_map(GtkWidget *widget)
{
    Pmd85Display *self = PMD85_DISPLAY(widget);
    GtkRoot *root;

    GTK_WIDGET_CLASS(pmd85_display_parent_class)->map(widget);
    root = gtk_widget_get_root(widget);
    if (GTK_IS_WINDOW(root)) {
        self->toplevel = GTK_WINDOW(root);
        self->suspended_id = g_signal_connect(self->toplevel, "notify::suspended",
                                              G_CALLBACK(on_suspended), self);
    }
    update_tick(self);
}

static void pmd85_display_unmap(GtkWidget *widget)
{
    Pmd85Display *self = PMD85_DISPLAY(widget);

    if (self->toplevel && self->suspended_id)
        g_signal_handler_disconnect(self->toplevel, self->suspended_id);
    self->suspended_id = 0;
    self->toplevel = NULL;
    GTK_WIDGET_CLASS(pmd85_display_parent_class)->unmap(widget);
    update_tick(self);
}

/* The widget's origin in its surface's coordinates, and the surface's
 * device-pixel scale: what rounding to the screen's pixel grid needs. */
static double device_geometry(GtkWidget *widget, double *ox, double *oy)
{
    GtkNative *native = gtk_widget_get_native(widget);
    graphene_point_t p;
    double tx = 0, ty = 0, scale = 1;

    *ox = *oy = 0;
    if (!native)
        return 1;
    if (gtk_native_get_surface(native))
        scale = gdk_surface_get_scale(gtk_native_get_surface(native));
    if (scale <= 0)
        scale = 1;
    gtk_native_get_surface_transform(native, &tx, &ty);
    if (gtk_widget_compute_point(widget, GTK_WIDGET(native),
                                 &GRAPHENE_POINT_INIT(0, 0), &p)) {
        *ox = p.x + tx;
        *oy = p.y + ty;
    }
    return scale;
}

static void pmd85_display_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    Pmd85Display *self = PMD85_DISPLAY(widget);
    const float w = (float)gtk_widget_get_width(widget);
    const float h = (float)gtk_widget_get_height(widget);
    double ds, ox, oy, dev_w, dev_h, dw, dh, x0, y0, aspect;
    GdkTexture *tex;
    GskScalingFilter filter;

    gtk_snapshot_append_color(snapshot, &(GdkRGBA){0, 0, 0, 1},
                              &GRAPHENE_RECT_INIT(0, 0, w, h));
    if (!self->texture || w < 1 || h < 1 || self->fb_w < 1 || self->fb_h < 1)
        return;

    /* everything below in device pixels */
    ds = device_geometry(widget, &ox, &oy);
    dev_w = w * ds;
    dev_h = h * ds;

    aspect = self->aspect_mode == PMD85_ASPECT_TV_4_3
                 ? 4.0 / 3.0
                 : (double)self->fb_w / (double)self->fb_h;

    if (self->aspect_mode == PMD85_ASPECT_INTEGER) {
        int scale = (int)MIN(dev_w / self->fb_w, dev_h / self->fb_h);
        if (scale < 1) scale = 1;
        dw = (double)scale * self->fb_w;
        dh = (double)scale * self->fb_h;
    } else if (dev_w / dev_h > aspect) {
        dh = floor(dev_h);
        dw = round(dh * aspect);
    } else {
        dw = floor(dev_w);
        dh = round(dw / aspect);
    }

    /* centred, on the screen's pixel grid */
    x0 = round(ox * ds + (dev_w - dw) / 2.0);
    y0 = round(oy * ds + (dev_h - dh) / 2.0);

    tex = self->texture;
    switch (self->aspect_mode == PMD85_ASPECT_INTEGER ? PMD85_SCALING_NEAREST
                                                      : self->scaling) {
    case PMD85_SCALING_SMOOTH:
        filter = GSK_SCALING_FILTER_LINEAR;
        break;
    case PMD85_SCALING_SHARP: {
        int kx = CLAMP((int)(dw / self->fb_w), 1, SHARP_MAX_FACTOR);
        int ky = CLAMP((int)(dh / self->fb_h), 1, SHARP_MAX_FACTOR);
        if (kx > 1 || ky > 1)
            tex = sharp_texture(self, kx, ky);
        filter = GSK_SCALING_FILTER_LINEAR;
        break;
    }
    default:
        filter = GSK_SCALING_FILTER_NEAREST;
        break;
    }

    gtk_snapshot_append_scaled_texture(snapshot, tex, filter,
        &GRAPHENE_RECT_INIT((float)(x0 / ds - ox), (float)(y0 / ds - oy),
                            (float)(dw / ds), (float)(dh / ds)));
}

static void pmd85_display_dispose(GObject *object)
{
    Pmd85Display *self = PMD85_DISPLAY(object);
    if (self->tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick_id);
        self->tick_id = 0;
    }
    g_clear_object(&self->texture);
    g_clear_object(&self->big);
    self->px = NULL;
    if (self->pool) {
        pool_unref(self->pool);
        self->pool = NULL;
    }
    if (self->big_pool) {
        pool_unref(self->big_pool);
        self->big_pool = NULL;
    }
    G_OBJECT_CLASS(pmd85_display_parent_class)->dispose(object);
}

static void pmd85_display_class_init(Pmd85DisplayClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    widget_class->snapshot = pmd85_display_snapshot;
    widget_class->map = pmd85_display_map;
    widget_class->unmap = pmd85_display_unmap;
    object_class->dispose = pmd85_display_dispose;
}

static void pmd85_display_init(Pmd85Display *self)
{
    self->pool = pool_new((gsize)PMD85SESSION_FB_MAX_WIDTH *
                          PMD85SESSION_FB_MAX_HEIGHT * sizeof(guint32));
    self->aspect_mode = PMD85_ASPECT_TV_4_3;
    self->scaling = PMD85_SCALING_SHARP;
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
}

GtkWidget *pmd85_display_new(pmd85session *session)
{
    Pmd85Display *self = g_object_new(PMD85_TYPE_DISPLAY, NULL);
    self->session = session;
    return GTK_WIDGET(self);
}

void pmd85_display_set_aspect_mode(Pmd85Display *self, Pmd85AspectMode mode)
{
    self->aspect_mode = mode;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void pmd85_display_set_scaling(Pmd85Display *self, Pmd85Scaling scaling)
{
    self->scaling = scaling;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}
