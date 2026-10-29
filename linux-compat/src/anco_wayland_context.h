/* Linux-only event-loop integration. Original host interfaces unchanged. */
#ifndef ANCO_WAYLAND_CONTEXT_H
#define ANCO_WAYLAND_CONTEXT_H
#include <glib.h>
#include <wayland-server-core.h>

typedef struct AncoWaylandContext AncoWaylandContext;
/* Borrowed live display, attached to GLib DEFAULT context used by AMS and
 * catalog child watches. One serialized owner; do not also run/dispatch the
 * Wayland loop. Duplicate adapters for the display are refused (EALREADY).
 * Never destroys display, clients or apps. Owner retains the returned handle.
 * Normal display destruction cancels the source before its fd is closed.
 * As with native Wayland, do not destroy the display INSIDE a protocol/idle
 * callback; schedule its destruction after dispatch has unwound.
 */
AncoWaylandContext *anco_wayland_context_new(struct wl_display *display);
/* Non-Wayland GLib callbacks which queue Wayland idle work, checked work or
 * output MUST wake this source after queuing. The aggregate fd excludes idle
 * work. This schedules a nonblocking dispatch, without periodic polling or
 * callbacks in GSource.prepare/check. Returns -ESHUTDOWN when detached.
 */
int anco_wayland_context_wake(AncoWaylandContext *context);
gboolean anco_wayland_context_active(AncoWaylandContext *context);
/* Actual loop/fd error (negative errno), not a readiness/UI success flag. */
int anco_wayland_context_error(AncoWaylandContext *context);
/* Cancel and release handle, leaving borrowed display/resources untouched.
 * May be called from a Wayland callback; unlike display destruction, removal
 * does not free the loop being dispatched. Do not use handle after free.
 */
void anco_wayland_context_free(AncoWaylandContext *context);
#endif
