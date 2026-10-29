#include "anco_wayland_context.h"
#include <errno.h>

struct AncoWaylandContext {
    GSource source;
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wl_listener destroyed;
    GPollFD poll;
    int error;
};

static void display_destroyed(struct wl_listener *listener, void *data)
{
    (void)data;
    AncoWaylandContext *c = wl_container_of(listener, c, destroyed);
    wl_list_remove(&c->destroyed.link);
    wl_list_init(&c->destroyed.link);
    c->display = NULL; c->loop = NULL;
    /* Source removal retires its poll registration before display/epoll close.
     * Owner's handle/reference remains valid until explicit free.
     */
    g_source_destroy(&c->source);
}

static gboolean check(GSource *source)
{
    AncoWaylandContext *c = (AncoWaylandContext *)source;
    return c->poll.revents != 0;
}
static gboolean dispatch(GSource *source, GSourceFunc callback, gpointer data)
{
    (void)callback; (void)data;
    AncoWaylandContext *c = (AncoWaylandContext *)source;
    g_source_set_ready_time(source, -1);
    if (!c->display || !c->loop) return G_SOURCE_REMOVE;
    if (c->poll.revents & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        c->error = -EIO;
        return G_SOURCE_REMOVE;
    }
    const int result = wl_event_loop_dispatch(c->loop, 0);
    if (result < 0) {
        if (errno == EINTR) {
            g_source_set_ready_time(source, 0);
            return G_SOURCE_CONTINUE;
        }
        c->error = -errno;
        if (!c->error) c->error = -EIO;
        return G_SOURCE_REMOVE;
    }
    /* Wayland dispatches checked sources AFTER its final idle drain. A checked
     * source can queue idle work which does not make the aggregate fd ready.
     * Drain that work before returning to GLib's poll, just as the next native
     * Wayland loop iteration drains idle before polling. No polling timer or
     * extra checked-source dispatch is needed.
     */
    if (!g_source_is_destroyed(source) && c->loop)
        wl_event_loop_dispatch_idle(c->loop);
    /* A callback may cancel this source while its GLib dispatch reference
     * keeps the allocation live. It does not destroy the borrowed display.
     */
    if (!g_source_is_destroyed(source) && c->display)
        wl_display_flush_clients(c->display);
    return g_source_is_destroyed(source) ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}
static void finalize(GSource *source)
{
    AncoWaylandContext *c = (AncoWaylandContext *)source;
    if (c->display) wl_list_remove(&c->destroyed.link);
}
static GSourceFuncs funcs = {.check = check, .dispatch = dispatch, .finalize = finalize};

AncoWaylandContext *anco_wayland_context_new(struct wl_display *display)
{
    if (!display) { errno = EINVAL; return NULL; }
    if (wl_display_get_destroy_listener(display, display_destroyed)) {
        errno = EALREADY; return NULL;
    }
    struct wl_event_loop *loop = wl_display_get_event_loop(display);
    const int fd = wl_event_loop_get_fd(loop);
    if (fd < 0) { errno = EBADF; return NULL; }
    AncoWaylandContext *c = (AncoWaylandContext *)g_source_new(&funcs, sizeof(*c));
    c->display = display; c->loop = loop;
    c->poll = (GPollFD){fd, G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL, 0};
    c->destroyed.notify = display_destroyed;
    wl_display_add_destroy_listener(display, &c->destroyed);
    g_source_add_poll(&c->source, &c->poll);
    g_source_set_name(&c->source, "AnCo Linux Wayland/default context");
    g_source_set_can_recurse(&c->source, FALSE);
    /* First pass also drains idle work queued before attachment. Later idle
     * scheduling by other sources uses explicit wake, not a polling timer.
     */
    g_source_set_ready_time(&c->source, 0);
    if (!g_source_attach(&c->source, NULL)) {
        g_source_unref(&c->source); errno = EIO; return NULL;
    }
    return c;
}
int anco_wayland_context_wake(AncoWaylandContext *c)
{
    if (!c) return -EINVAL;
    if (g_source_is_destroyed(&c->source)) return -ESHUTDOWN;
    g_source_set_ready_time(&c->source, 0);
    return 0;
}
gboolean anco_wayland_context_active(AncoWaylandContext *c)
{
    return c && !g_source_is_destroyed(&c->source);
}
int anco_wayland_context_error(AncoWaylandContext *c)
{
    return c ? c->error : -EINVAL;
}
void anco_wayland_context_free(AncoWaylandContext *c)
{
    if (c) { g_source_destroy(&c->source); g_source_unref(&c->source); }
}
