/* Actual original host display -> standard Wayland metadata. No rendering. */
#ifndef ANCO_WAYLAND_DISPLAY_H
#define ANCO_WAYLAND_DISPLAY_H
#include "anco_graphic_display.h"
#include <stdbool.h>
#include <wayland-server-core.h>
struct wlr_output;
struct wlr_output_layout;
typedef struct {
    AncoGraphicDisplay display;
    double scale, scaled_density;
    int density_dpi;
    int rotation;
    bool rotation_known; // Absent in an older helper: unknown, never assumed0.
} AncoWaylandDisplayInfo;
int anco_wayland_display_query(const char *native_query,
    const AncoGraphicDisplay *displays, size_t count, AncoWaylandDisplayInfo *info);
struct wlr_output *anco_wayland_display_create(struct wl_display *display,
    struct wlr_output_layout *layout, const AncoWaylandDisplayInfo *info);
#endif
