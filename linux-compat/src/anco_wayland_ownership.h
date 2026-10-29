/* Internal Linux adapter only: actual Wayland object lifetime to the accepted
 * catalog. No protocol extension, host API, rendering or window policy here.
 * Catalog must outlive this adapter. One serialized GLib/Wayland context.
 */
#ifndef ANCO_WAYLAND_OWNERSHIP_H
#define ANCO_WAYLAND_OWNERSHIP_H

#include "anco_linux_catalog.h"
#include <wayland-server-core.h>

typedef struct AncoWaylandOwnership AncoWaylandOwnership;

/* Local common-runtime notification, not a host protocol or a process-death
 * assertion. Called once per tracked Client AFTER all its Window retirement
 * hooks and surface bindings have been removed. disconnected=TRUE means the
 * actual wl_client destroy signal; FALSE means explicit adapter teardown while
 * the connection may remain live. Never synthesize original mClientDead from
 * FALSE. No notification on last-window close while Client remains tracked.
 * The borrowed client is still allocated but may be inside destruction: no
 * requests, destroy, nested dispatch or adapter mutation/free in this hook.
 * Retained Session association belongs to the common runtime; this notification
 * neither selects a Session nor proves outstanding native buffers completed.
 */
typedef void (*AncoWaylandClientRetireFunc)(AncoWaylandOwnership *adapter,
    struct wl_client *client, gboolean disconnected, void *user_data);
/* Install before tracking any client. One observer, no replay; clearing needs
 * matching user_data and zero tracked clients. Same serialized owner context. */
int anco_wayland_ownership_on_client_retire(AncoWaylandOwnership *adapter,
    AncoWaylandClientRetireFunc callback, void *user_data);

/* Trusted common-runtime synchronous retirement hook. These are the ACTUAL
 * admitted creation identities, retained until this Window is retired even if
 * the child has exited. They are not proof of a still-live process/admission.
 * Called once BEFORE catalog removal/listener detachment on role, surface,
 * client destruction or explicit adapter free. Resources are still allocated,
 * but may be inside destroy: no requests, resource destroy, adapter free or
 * adapter mutation/reentrant dispatch from this hook. Borrowed values only.
 * Original WMS/IME/Graphic cleanup belongs to the common runtime, not here.
 */
typedef void (*AncoWaylandWindowRetireFunc)(AncoWaylandOwnership *adapter,
    struct wl_resource *role, struct wl_resource *surface,
    const AncoLinuxWindowOwner *creation, void *user_data);

/* Attach before admitting any windows. Single owner; clearing requires its
 * matching user_data and no remaining windows. No retroactive guessed event.
 */
int anco_wayland_ownership_on_window_retire(AncoWaylandOwnership *adapter,
    AncoWaylandWindowRetireFunc callback, void *user_data);

AncoWaylandOwnership *anco_wayland_ownership_new(AncoLinuxCatalog *catalog);
/* Detaches listeners and bindings, never destroys client/resource/process. */
void anco_wayland_ownership_free(AncoWaylandOwnership *adapter);
/* Actual server wl_surface only. Credentials are read by libwayland from its
 * accepted socket, not supplied by clients. Never adopts an app_id/title/PID
 * guess or a parent-created socketpair. Idempotent for a still-live binding.
 */
int anco_wayland_ownership_track(AncoWaylandOwnership *adapter,
                                struct wl_resource *surface,
                                AncoLinuxSurfaceOwner *out);
/* Trusted runtime selects an already admitted actual Activity. No host API or
 * Wayland extension. Cannot change an existing surface's Activity. Default
 * track and owner subsequently preserve/revalidate this explicit selection.
 */
int anco_wayland_ownership_track_activity(AncoWaylandOwnership *adapter,
    struct wl_resource *surface, gint32 activity, AncoLinuxSurfaceOwner *out);
/* Trusted compositor adapter for a protocol multiplexer such as Xwayland.
 * The Wayland peer is the multiplexer, while process is the real X11 client
 * identity already validated by XWM. Internal bridge only; no new protocol. */
int anco_wayland_ownership_track_activity_process(
    AncoWaylandOwnership *adapter, struct wl_resource *surface,
    gint32 activity, const AncoLinuxProcess *process,
    AncoLinuxSurfaceOwner *out);
/* Revalidates real catalog ownership; process exit cannot leave cached success.
 * During retirement returns -EBUSY without binding or modifying output.
 * The supplied surface must be LIVE; no API may dereference a destroyed object.
 */
int anco_wayland_ownership_owner(AncoWaylandOwnership *adapter,
                                struct wl_resource *surface,
                                AncoLinuxSurfaceOwner *out);
guint anco_wayland_ownership_surfaces(AncoWaylandOwnership *adapter);

/* Call ONLY from the trusted shell's actual get_toplevel/get_popup constructor
 * using its real role-to-wl_surface association. This adapter cannot discover
 * that association from arbitrary wl_resource user data or client metadata.
 * Both resources must be live and share a client; surface already tracked.
 * The catalog's original-resource Window allocator must be configured first.
 * Does not set resource implementation, classify an Android type/token, map
 * the role or create/export a host window. No raw-surface auto-window hook.
 */
int anco_wayland_ownership_track_window(AncoWaylandOwnership *adapter,
                                       struct wl_resource *role,
                                       struct wl_resource *surface,
                                       AncoLinuxWindowOwner *out);
/* Actual shell-role lifetime can end before its protocol resource becomes
 * destroyed (e.g. upstream popup_done leaves an inert xdg_popup resource).
 * Call from the same trusted constructor with that REAL role's destroy signal.
 * The signal must fire before its storage is freed, while resources are live.
 * Either role-object, protocol-resource, surface or client destruction retires
 * once through the existing hook. No unmap inference or host removal occurs.
 * Existing bindings cannot acquire/change a lifetime signal retroactively.
 * Signal may not be NULL. Adapter must be freed before borrowed shell state.
 */
int anco_wayland_ownership_track_window_lifetime(AncoWaylandOwnership *adapter,
    struct wl_resource *role, struct wl_resource *surface,
    struct wl_signal *role_destroy, AncoLinuxWindowOwner *out);
/* Same allocator for a compositor-owned role object which is not a Wayland
 * protocol resource (for example wlr_xwayland_surface). */
int anco_wayland_ownership_track_window_key_lifetime(
    AncoWaylandOwnership *adapter, const void *role,
    struct wl_resource *surface, struct wl_signal *role_destroy,
    AncoLinuxWindowOwner *out);
/* During retirement returns -EBUSY; cleanup uses the hook creation identity.
 * No output modification or new ownership lookup during that callback. */
int anco_wayland_ownership_window_owner(AncoWaylandOwnership *adapter,
                                        struct wl_resource *role,
                                        AncoLinuxWindowOwner *out);
/* Resolve the exact role registered for this LIVE wl_surface, not any Window
 * sharing its Activity. Revalidates catalog/process ownership. No registration
 * or host request. During retirement returns EBUSY; use the hook's creation
 * identity for cleanup instead. Failure leaves out untouched.
 */
int anco_wayland_ownership_surface_window(AncoWaylandOwnership *adapter,
    struct wl_resource *surface, AncoLinuxWindowOwner *out);
/* Count of attached role resources; not a host-visible/live-process count. */
guint anco_wayland_ownership_windows(AncoWaylandOwnership *adapter);

/* Resolve a previously admitted guest Window identity to its actual objects.
 * The identity must originate from this adapter/catalog, NOT a host desktop
 * window ID, Wayland protocol ID or an input callback's unclassified integer.
 * All owner fields (including process start time) must match; live catalog
 * ownership is revalidated. Does not select focus, test coordinates, register
 * input tokens or establish that a surface is mapped/host-exported.
 * Borrowed resources are valid only until the next serialized lifecycle event.
 * No callback/dispatch occurs. During retirement returns EBUSY. Every failure
 * leaves both outputs untouched; distinct non-NULL outputs are required.
 */
int anco_wayland_ownership_resolve_window(AncoWaylandOwnership *adapter,
    const AncoLinuxWindowOwner *identity, struct wl_resource **role,
    struct wl_resource **surface);

/* Snapshot ALL currently admitted Window identities in this exact live Task.
 * No implicit descendant-Task/recent restoration or focus/stacking selection.
 * Revalidates the real Task and every matching catalog window; includes its
 * different Activities and unmapped-but-live roles. Not visibility/readiness.
 * Same serialized context, no dispatch. Retirement callback returns -EBUSY.
 * On success, caller owns *windows (g_free); zero count gives NULL. Entries
 * contain values only, no borrowed wl_resource pointers. Resolve each identity
 * again before later use; this snapshot cannot extend any resource lifetime.
 * Every failure leaves both outputs unchanged, never returns partial results.
 */
int anco_wayland_ownership_task_windows(AncoWaylandOwnership *adapter,
    gint32 task, AncoLinuxWindowOwner **windows, guint *count);

/* Role destroy drops its Window registration but leaves Surface/Task alive.
 * Surface destroy detaches the still-live role listener; disconnect detaches
 * both before libwayland resource teardown. Adapter free retires registrations
 * while leaving resources intact. The hook supplies a teardown call point, not
 * the original exporter: the common runtime must actually implement original
 * export cleanup before allowing retirement. Free itself sends no WMS messages.
 * Reattaching after explicit registration retirement creates a new identity.
 */

#endif
