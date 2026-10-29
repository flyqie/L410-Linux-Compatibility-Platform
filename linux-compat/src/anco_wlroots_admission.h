/* Linux-side bridge; no new Wayland or host protocol. */
#ifndef ANCO_WLROOTS_ADMISSION_H
#define ANCO_WLROOTS_ADMISSION_H
#include "anco_wayland_ownership.h"
struct wlr_xdg_surface;
struct wlr_xwayland_surface;
/* Call on the serialized runtime context from actual new_toplevel/new_popup.
 * Activity must already be selected and admitted by the trusted catalog.
 * No title/parent/type inference, configure, host export or buffer import.
 * Real role lifetime, not unmap, retires the identity. The role and surface
 * must remain live throughout this call. Output remains unchanged on error.
 * If window admission fails after surface admission, the real surface binding
 * remains attached until its actual destruction; no fabricated rollback event.
 */
int anco_wlroots_admit(AncoWaylandOwnership *owner,
    struct wlr_xdg_surface *xdg, gint32 activity, AncoLinuxWindowOwner *out);
/* Final client-configuration handoff AFTER the common runtime has selected
 * original Task/window policy and computed this toplevel's logical size.
 * The LIVE xdg object must be owned by the same upstream shell as admission.
 * Revalidates complete admitted Window identity and exact role/surface pair.
 * No Task-to-Window inference, parent resize propagation, DPI or pixel scaling.
 * Uses upstream scheduled state/serial queue; never emits a parallel configure.
 * Success means scheduled only, NOT ack, commit, new buffer or presentation.
 * Serial0 is valid. Failure leaves serial unchanged. A scheduling allocation
 * failure retains upstream state/error behavior and returns -ENOMEM.
 * Same serialized context; wake its Wayland source after calling from Binder.
 */
int anco_wlroots_toplevel_size(AncoWaylandOwnership *owner,
    struct wlr_xdg_surface *xdg, const AncoLinuxWindowOwner *identity,
    gint32 width, gint32 height, guint32 *serial);
/* Call only inside this actual surface's commit notification, on the owner
 * context. Exact live Window/role/surface association is required. Success
 * acquires one upstream wlr_buffer lock and returns the actual uint32 commit
 * seq (which can wrap; NOT an AnCo frame ID or completion). No buffer means
 * -ENODATA with outputs unchanged. No texture, copy, renderer or host call.
 * Consumer must retain the lock until its actual use/fence completes and then
 * wlr_buffer_unlock exactly once. Window destruction alone is not completion.
 */
struct wlr_buffer;
int anco_wlroots_lock_commit_buffer(AncoWaylandOwnership *owner,
    struct wlr_xdg_surface *xdg, const AncoLinuxWindowOwner *identity,
    struct wlr_buffer **buffer, guint32 *commit_seq);
/* Standard rootless Xwayland/XWM admission. _NET_WM_PID is revalidated against
 * the exact launched process. The actual Xwayland wl_surface buffer is used;
 * no XComposite capture or image transport is introduced. */
int anco_wlroots_admit_xwayland(AncoWaylandOwnership *owner,
    struct wlr_xwayland_surface *xsurface, gint32 activity,
    AncoLinuxWindowOwner *out);
int anco_wlroots_lock_xwayland_buffer(AncoWaylandOwnership *owner,
    struct wlr_xwayland_surface *xsurface,
    const AncoLinuxWindowOwner *identity,
    struct wlr_buffer **buffer, guint32 *commit_seq);
#endif
