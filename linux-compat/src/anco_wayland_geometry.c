#define WLR_USE_UNSTABLE
#include "anco_wayland_geometry.h"
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <errno.h>
#include <stdint.h>

static gboolean rect(const struct wlr_box *box, AncoWmsRect *out)
{
    const int64_t right=(int64_t)box->x+box->width;
    const int64_t bottom=(int64_t)box->y+box->height;
    if (box->width<=0 || box->height<=0 || right>INT32_MAX || right<INT32_MIN ||
        bottom>INT32_MAX || bottom<INT32_MIN) return FALSE;
    *out=(AncoWmsRect){box->x,box->y,(gint32)right,(gint32)bottom};
    return TRUE;
}
int anco_wayland_geometry_read(struct wlr_xdg_surface *surface, AncoWaylandGeometry *out)
{
    if (!surface || !out || !surface->surface) return -EINVAL;
    if (!surface->initialized || !surface->surface->mapped ||
        !wlr_surface_has_buffer(surface->surface)) return -ENODATA;
    struct wlr_box extent, geometry;
    wlr_surface_get_extends(surface->surface,&extent);
    wlr_xdg_surface_get_geometry(surface,&geometry);
    AncoWaylandGeometry next;
    if (!rect(&extent,&next.extent) || !rect(&geometry,&next.geometry)) return -ERANGE;
    const int64_t insets[]={
        (int64_t)next.geometry.left-next.extent.left,
        (int64_t)next.geometry.top-next.extent.top,
        (int64_t)next.extent.right-next.geometry.right,
        (int64_t)next.extent.bottom-next.geometry.bottom
    };
    for (unsigned i=0;i<4;i++) if(insets[i]<0 || insets[i]>INT32_MAX) return -ERANGE;
    next.surface_insets=(AncoWmsRect){(gint32)insets[0],(gint32)insets[1],(gint32)insets[2],(gint32)insets[3]};
    *out=next;
    return 0;
}
