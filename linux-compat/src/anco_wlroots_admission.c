#define WLR_USE_UNSTABLE
#include "anco_wlroots_admission.h"
#include <wlr/types/wlr_xdg_shell.h>
#include <errno.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/xwayland.h>

int anco_wlroots_admit(AncoWaylandOwnership *owner,
    struct wlr_xdg_surface *xdg, gint32 activity, AncoLinuxWindowOwner *out)
{
    if (!owner || !xdg || !out || activity <= 0) return -EINVAL;
    struct wl_resource *role;
    struct wl_signal *destroy;
    switch (xdg->role) {
    case WLR_XDG_SURFACE_ROLE_TOPLEVEL:
        if (!xdg->toplevel || xdg->toplevel->base != xdg) return -EINVAL;
        role = xdg->toplevel->resource;
        destroy = &xdg->toplevel->events.destroy;
        break;
    case WLR_XDG_SURFACE_ROLE_POPUP:
        if (!xdg->popup || xdg->popup->base != xdg) return -EINVAL;
        role = xdg->popup->resource;
        destroy = &xdg->popup->events.destroy;
        break;
    default:
        return -EPROTOTYPE;
    }
    if (!role || !xdg->surface || !xdg->surface->resource) return -EINVAL;
    AncoLinuxSurfaceOwner surface;
    int error = anco_wayland_ownership_track_activity(owner,
        xdg->surface->resource, activity, &surface);
    if (error) return error;
    return anco_wayland_ownership_track_window_lifetime(owner, role,
        xdg->surface->resource, destroy, out);
}

int anco_wlroots_toplevel_size(AncoWaylandOwnership *owner,
    struct wlr_xdg_surface *xdg, const AncoLinuxWindowOwner *identity,
    gint32 width, gint32 height, guint32 *serial)
{
    if (!owner || !xdg || !identity || !serial || width < 0 || height < 0)
        return -EINVAL;
    struct wl_resource *role, *surface;
    int error = anco_wayland_ownership_resolve_window(owner, identity, &role, &surface);
    if (error) return error;
    if (xdg->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) return -EPROTOTYPE;
    if (!xdg->toplevel || xdg->toplevel->base != xdg || !xdg->surface ||
        xdg->toplevel->resource != role || xdg->surface->resource != surface)
        return -ESTALE;
    if (!xdg->initialized) return -EAGAIN;
    const uint32_t pending = wlr_xdg_toplevel_set_size(xdg->toplevel, width, height);
    if (!xdg->configure_idle) return -ENOMEM;
    *serial = pending;
    return 0;
}

int anco_wlroots_lock_commit_buffer(AncoWaylandOwnership *owner,
    struct wlr_xdg_surface *xdg, const AncoLinuxWindowOwner *identity,
    struct wlr_buffer **buffer, guint32 *seq)
{
    if (!owner || !xdg || !identity || !buffer || !seq) return -EINVAL;
    struct wl_resource *role, *surface;
    int error = anco_wayland_ownership_resolve_window(owner, identity, &role, &surface);
    if (error) return error;
    if (!xdg->surface || xdg->surface->resource != surface) return -ESTALE;
    struct wl_resource *actual_role = NULL;
    if (xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && xdg->toplevel &&
        xdg->toplevel->base == xdg) actual_role = xdg->toplevel->resource;
    else if (xdg->role == WLR_XDG_SURFACE_ROLE_POPUP && xdg->popup &&
        xdg->popup->base == xdg) actual_role = xdg->popup->resource;
    else return -EPROTOTYPE;
    if (actual_role != role) return -ESTALE;
    if (!(xdg->surface->current.committed & WLR_SURFACE_STATE_BUFFER) ||
        !xdg->surface->current.buffer) return -ENODATA;
    *buffer = wlr_buffer_lock(xdg->surface->current.buffer);
    *seq = xdg->surface->current.seq;
    return 0;
}

int anco_wlroots_admit_xwayland(AncoWaylandOwnership *owner,
    struct wlr_xwayland_surface *xsurface, gint32 activity,
    AncoLinuxWindowOwner *out)
{
    if (!owner || !xsurface || !out || activity < 0 || xsurface->pid <= 0 ||
        !xsurface->surface || !xsurface->surface->resource) return -EINVAL;
    AncoLinuxProcess process;
    int error = anco_linux_process_read(xsurface->pid, &process);
    if (error) return error;
    AncoLinuxSurfaceOwner surface;
    error = anco_wayland_ownership_track_activity_process(owner,
        xsurface->surface->resource, activity, &process, &surface);
    if (error) return error;
    return anco_wayland_ownership_track_window_key_lifetime(owner,
        xsurface, xsurface->surface->resource, &xsurface->events.dissociate, out);
}

int anco_wlroots_lock_xwayland_buffer(AncoWaylandOwnership *owner,
    struct wlr_xwayland_surface *xsurface,
    const AncoLinuxWindowOwner *identity,
    struct wlr_buffer **buffer, guint32 *seq)
{
    if (!owner || !xsurface || !identity || !buffer || !seq ||
        !xsurface->surface || !xsurface->surface->resource) return -EINVAL;
    AncoLinuxWindowOwner current;
    int error = anco_wayland_ownership_surface_window(owner,
        xsurface->surface->resource, &current);
    if (error) return error;
    const AncoLinuxSurfaceOwner *a=&current.owner,*b=&identity->owner;
    if (current.window_id!=identity->window_id || a->user_id!=b->user_id ||
        a->task_id!=b->task_id || a->activity_id!=b->activity_id ||
        a->guest_uid!=b->guest_uid || a->process.pid!=b->process.pid ||
        a->process.uid!=b->process.uid || a->process.gid!=b->process.gid ||
        a->process.start_ticks!=b->process.start_ticks) return -ESTALE;
    if (!(xsurface->surface->current.committed & WLR_SURFACE_STATE_BUFFER) ||
        !xsurface->surface->current.buffer) return -ENODATA;
    *buffer = wlr_buffer_lock(xsurface->surface->current.buffer);
    *seq = xsurface->surface->current.seq;
    return 0;
}
