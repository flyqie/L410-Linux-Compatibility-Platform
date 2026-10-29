/* Linux-side identities for the original AnCo Task/Activity/window contract.
 * No Binder messages or host window policy are defined here. Single event-loop
 * ownership is required; callers must serialize mutations.
 */
#ifndef ANCO_LINUX_OWNERSHIP_H
#define ANCO_LINUX_OWNERSHIP_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct AncoLinuxOwnership AncoLinuxOwnership;

/* Supplied by the original-registration/launcher integration AFTER successful
 * installation admission, from the registered entry, never a window title or
 * xdg app_id. This module cannot authenticate a BMS acknowledgement itself.
 * guest_uid and Linux credentials belong to separate namespaces.
 */
typedef struct {
    int32_t user_id;
    int32_t guest_uid;
    const char *bundle;
    const char *ability;
} AncoLinuxApp;

typedef struct {
    pid_t pid;
    uid_t uid;
    gid_t gid;
    uint64_t start_ticks;
} AncoLinuxProcess;

typedef struct {
    int32_t user_id;
    int32_t task_id;
    int32_t activity_id;
    int32_t guest_uid;
    AncoLinuxProcess process;
} AncoLinuxSurfaceOwner;

typedef struct {
    int32_t window_id;
    AncoLinuxSurfaceOwner owner;
} AncoLinuxWindowOwner;

AncoLinuxOwnership *anco_linux_ownership_new(void);
void anco_linux_ownership_free(AncoLinuxOwnership *state);

/* Capture a live same-PID-namespace process. Refuses ambiguous namespace/PID
 * mappings; no container-name lookup, PID sentinel, or guessed conversion.
 */
int anco_linux_process_read(pid_t pid, AncoLinuxProcess *out);
/* Exact live /proc ancestry using full ancestor identity. */
int anco_linux_process_is_descendant(const AncoLinuxProcess *ancestor,
                                     const AncoLinuxProcess *candidate);

/* These functions consume a task ALREADY SELECTED by the launch integration;
 * they are not a package-name singleton or a replacement for original Intent
 * reuse policy. An actual launcher-owned process is required before creation.
 * Errors are negative errno values and leave output IDs untouched.
 */
int anco_linux_task_create(AncoLinuxOwnership *state,
                          const AncoLinuxApp *registered_app,
                          const AncoLinuxProcess *process, int32_t *task_id);
int anco_linux_task_restore_cursor(AncoLinuxOwnership *state,
                                   int32_t user_id, int32_t task_id);
int anco_linux_activity_create(AncoLinuxOwnership *state, int32_t task_id,
                              const AncoLinuxApp *registered_app,
                              const AncoLinuxProcess *process,
                              int32_t *activity_id);

/* The adapter must pass actual wl_client / wl_resource addresses and the
 * credentials read from that client, not client-provided IDs or metadata.
 * Object identity is scoped by client as in Wayland. A process may own several
 * Activities; a transient dialog may share its owner's Activity. No heuristic
 * derives Activity identity from set_parent, title, app_id, or surface number.
 */
int anco_linux_surface_bind(AncoLinuxOwnership *state, int32_t activity_id,
                            const void *client, const void *surface,
                            pid_t peer_pid, uid_t peer_uid, gid_t peer_gid);
/* A trusted protocol multiplexer can expose a real application process below
 * the catalog-owned launcher. The full process identity and actual /proc
 * ancestry are revalidated; client-supplied parent or title data is unused. */
int anco_linux_surface_bind_descendant(AncoLinuxOwnership *state,
    int32_t activity_id,const void *client,const void *surface,
    const AncoLinuxProcess *process);
int anco_linux_surface_owner(AncoLinuxOwnership *state, const void *client,
                             const void *surface, AncoLinuxSurfaceOwner *out);
int anco_linux_surface_remove(AncoLinuxOwnership *state, const void *client,
                              const void *surface);
void anco_linux_client_disconnected(AncoLinuxOwnership *state,
                                     const void *client);

/* Original WMS base = 100000 * Resources.getInteger(0x010e0091). The caller
 * must supply the verified resource value for the paired guest configuration;
 * retained-image defaults are NOT a current-host configuration discovery.
 * There is no implicit base. Repeated identical setup is harmless; changing
 * it requires a new runtime, never silently rewinds live/retired window IDs.
 */
int anco_linux_windows_configure(AncoLinuxOwnership *state,
                                 int32_t resource_value);
/* Reserve one canonical original Window ID for a compositor-owned transient
 * which belongs to an already admitted Activity but has no client surface.
 * This is used for the stock AnCo starting-window type only.  It changes no
 * host protocol and deliberately creates no logical Linux Window record. */
int anco_linux_window_reserve_transient(AncoLinuxOwnership *state,
                                        int32_t activity_id, int32_t *out);
/* Trusted Linux window manager's actual logical window object, backed by an
 * already admitted surface. NOT a wl_surface-created hook: cursors, unroled
 * surfaces and subsurfaces are not automatically windows. Role/type/token
 * admission and original WMS export remain the caller's separate obligations.
 * One live logical window per surface. Object identity is client scoped.
 * These IDs are independent of Task, Activity, Wayland IDs and View IDs.
 */
int anco_linux_window_add(AncoLinuxOwnership *state, const void *client,
                          const void *window, const void *surface,
                          AncoLinuxWindowOwner *out);
int anco_linux_window_owner(AncoLinuxOwnership *state, const void *client,
                            const void *window, AncoLinuxWindowOwner *out);
/* Window retirement leaves the live surface, Activity and Task intact.
 * Surface/Activity/client/process retirement also drops associated windows.
 * No host remove/cleanup message is sent by this identity-only module.
 */
int anco_linux_window_remove(AncoLinuxOwnership *state, const void *client,
                             const void *window);
int anco_linux_activity_remove(AncoLinuxOwnership *state, int32_t activity_id);

/* Finish retires the task's Activities/surfaces, retaining its ID in the recent
 * task collision set. Explicit recent removal releases it. Neither operation
 * sends signals: stopping an application process is the launcher's lifecycle.
 */
int anco_linux_task_finish(AncoLinuxOwnership *state, int32_t task_id);
int anco_linux_task_remove_recent(AncoLinuxOwnership *state, int32_t task_id);
int anco_linux_task_alive(AncoLinuxOwnership *state, int32_t task_id);
/* Removes bindings for dead processes, retaining recent task identities. */
unsigned anco_linux_ownership_reap(AncoLinuxOwnership *state);

#endif
