#ifndef ANCO_WAYLAND_GEOMETRY_H
#define ANCO_WAYLAND_GEOMETRY_H
#include "anco_wms_layout.h"
struct wlr_xdg_surface;
typedef struct {
    /* Committed surface-local LOGICAL coordinates, never buffer pixels.
     * Extent includes mapped subsurfaces per original XDG geometry rules.
     * This is layout only; each real buffer keeps its own queue/lifetime. */
    AncoWmsRect extent, geometry;
    AncoWmsRect surface_insets;
} AncoWaylandGeometry;
/* Call on a live actual upstream XDG object in the serialized commit context.
 * Requires committed mapped content; output stays unchanged on local errors.
 * Supplies semantic inputs for original fromLp. Does not supply global frame,
 * Task bounds/type/flags, admit a window, crop/merge buffers or route input.
 * Consumer must distinguish aggregate extent from individual buffer bounds.
 * No hardcoded toolkit margins, integer scale multiplication or host protocol.
 */
int anco_wayland_geometry_read(struct wlr_xdg_surface *, AncoWaylandGeometry *);
#endif
