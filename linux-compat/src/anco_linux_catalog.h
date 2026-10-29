/* Linux-side registration/launcher ownership; original host ABI is unchanged.
 * One serialized GLib main context owns this state and its child watches.
 * The caller is the trusted app-catalog manager, not an application client.
 */
#ifndef ANCO_LINUX_CATALOG_H
#define ANCO_LINUX_CATALOG_H

#include "anco_bms_registration.h"
#include "anco_linux_ownership.h"

typedef struct AncoLinuxCatalog AncoLinuxCatalog;

typedef struct {
    AncoLinuxApp app;
    AncoLinuxProcess process;
    gint32 task_id;
    gint32 activity_id;
} AncoLinuxLaunch;

/* Actual direct-child wait event, once per still-active admitted Task in that
 * process, after all its Tasks/Activities and the live instance are retired.
 * Borrowed retained launch identity remains valid during this callback. This
 * is NOT a per-surface close, nor an unconditional host Task/window operation.
 * Do not mutate/free the catalog or replace its listener from this callback.
 */
typedef void (*AncoLinuxExitFunc)(AncoLinuxCatalog *catalog,
                                  const AncoLinuxLaunch *launch,
                                  gint wait_status, void *user_data);
/* Single common-runtime listener. A second owner is refused; clearing requires
 * matching user_data. Catalog free refuses an attached listener or active exit.
 */
int anco_linux_catalog_on_exit(AncoLinuxCatalog *catalog,
                               AncoLinuxExitFunc callback, void *user_data);

/* Only during the synchronous exit callback (including its AMS observer).
 * Borrowed, unordered snapshot of ALL Activities still admitted to this Task
 * immediately before its real child wait retired live identities. Empty is
 * valid; explicitly removed Activities are excluded. No host removal, export
 * success or rendering completion is implied. Task must match this callback.
 * Copy identities before scheduling asynchronous cleanup. Storage expires at
 * callback return; app strings remain catalog-owned. Errors preserve outputs.
 */
int anco_linux_catalog_exit_activities(AncoLinuxCatalog *catalog, gint32 task,
    const AncoLinuxLaunch **activities, guint *count);

/* Trusted common-runtime event AFTER a real Activity and its Task/process
 * records are committed, before the admission call returns. Covers initial
 * Activities in new/existing processes and additional Activities, never reuse.
 * Borrowed identity; query/copy it synchronously. No nested main-loop dispatch,
 * catalog mutation, destruction or listener replacement. In particular this
 * hook is not permission to recursively launch another application. It does
 * not wait for mission/frames, create a host window or acknowledge host state.
 * One observer, no replay; attach before admission, clear with matching data.
 */
typedef void (*AncoLinuxActivityCreatedFunc)(AncoLinuxCatalog *catalog,
    const AncoLinuxLaunch *activity, void *user_data);
int anco_linux_catalog_on_activity_created(AncoLinuxCatalog *catalog,
    AncoLinuxActivityCreatedFunc observer, void *user_data);

AncoLinuxCatalog *anco_linux_catalog_new(const char *owned_bundle_prefix,
                                        gint32 guest_user, gboolean privacy);
/* Returns -EBUSY without freeing while an owned child/watch is outstanding. */
int anco_linux_catalog_free(AncoLinuxCatalog *catalog);

/* Retains original metadata bytes and derives identity FROM that metadata.
 * No host call, automatic appId assignment, metadata builder or UID change.
 * Caller must obtain the stable appId from its persistent package records and
 * check fresh host namespace conflicts before first production publication.
 */
int anco_linux_catalog_stage(AncoLinuxCatalog *catalog, const void *json,
                             gsize length);
/* Bind an original declared component to the actual Linux command. Absolute
 * executable and cwd, argv vector (no shell), matching launcher-owned rootfs.
 * Commands cannot be changed after registration/while an instance is active.
 */
int anco_linux_catalog_command(AncoLinuxCatalog *catalog, const char *bundle,
                               const char *ability, const char *const *argv,
                               const char *cwd);
gboolean anco_linux_catalog_publish(AncoLinuxCatalog *catalog,
                                    GBinderRemoteObject *bms, const char *bundle,
                                    AncoBmsRegistrationResult *result);
/* Recovery of this manager's already registered ordinary-user0
 * package. Performs a fresh original BMS flags7 query, runs the original native
 * parser helper with that reply via pipes, and reconciles identity/version and
 * declared components before accepting. No cached JSON or re-registration.
 * Commands must already be bound. Does not start applications or publish AMS.
 * A new package may be recovered while unrelated package instances run; the
 * target package itself must not already be accepted and callback reentry is
 * still rejected.
 * The parser currently handles inline <=32KiB replies; larger replies fail.
 */
gboolean anco_linux_catalog_recover(AncoLinuxCatalog *catalog,
    GBinderRemoteObject *bms, const char *bundle, gint32 host_user,
    const char *native_decoder, AncoBmsRegistrationResult *result);
gboolean anco_linux_catalog_unpublish(AncoLinuxCatalog *catalog,
                                      GBinderRemoteObject *bms, const char *bundle,
                                      AncoBmsRegistrationResult *result);
const AncoLinuxApp *anco_linux_catalog_registered(AncoLinuxCatalog *catalog,
                                                  const char *bundle,
                                                  const char *ability);

/* Launch the registered Linux command and retain only its real process/app
 * ownership.  No Task or Activity is allocated here: the trusted compositor
 * creates those from an actual independent top-level window.  This is an
 * internal Linux semantic split; it sends no new host/AnCo/Wayland message.
 * A process may exit without ever creating a window. */
int anco_linux_catalog_start_process(AncoLinuxCatalog *catalog,
    const char *bundle, const char *ability, AncoLinuxProcess *out);

/* ONLY the new-task execution branch, AFTER original launch selection. This
 * API is not a replacement for Intent reuse/document/affinity policy. No first
 * frame, host Ability or watchdog is required to allocate the real task.
 * No diagnostic arguments or toolkit singleton override are added to argv.
 */
int anco_linux_catalog_start_new(AncoLinuxCatalog *catalog, const char *bundle,
                                 const char *ability, AncoLinuxLaunch *out);
/* Trusted common runtime, AFTER it explicitly selects a NEW Task for the
 * same registered component in this catalog's actual already-launched process.
 * source_task identifies that live process ownership; it is NOT an implicit
 * source Activity or a window-role/Intent decision. Allocates a distinct Task
 * and initial Activity without exec, another child watch, host notification,
 * or inference from set_parent/title/app_id. Output unchanged on failure.
 * Full original role/Task policy and host lifecycle are wired separately.
 */
int anco_linux_catalog_task_create_in_process(AncoLinuxCatalog *catalog,
    gint32 source_task, AncoLinuxLaunch *out);
/* Linux process remains alive after its last Task/root window has retired and
 * creates another independent xdg_toplevel.  Resolve the exact retained child
 * identity (PID, credentials and start ticks) and allocate a fresh original
 * Task/Activity in that same process.  This is the no-source-Activity form of
 * _task_create_in_process: no exec, process signal, package singleton guess or
 * host protocol is introduced. */
int anco_linux_catalog_task_create_for_process(AncoLinuxCatalog *catalog,
    const AncoLinuxProcess *process, AncoLinuxLaunch *out);
/* A GUI process may enter the already-prepared Linux session through its PTY,
 * or a launcher-owned application may reparent a helper which creates another
 * real top-level. Admit the former only by a unique registered executable inode
 * or the exact executable inode of an already-admitted live window process;
 * admit the latter only by the exact bundle/ability environment stamped by an
 * active catalog launch. This creates no host protocol or package identity: it
 * reuses that exact registered app and the ordinary Task/Activity allocator.
 * The adopted instance exists only while it has live window Tasks; retiring
 * its last Task forgets the instance without signalling or reaping the process.
 * No title, app_id, process-name or parent-PID inference.
 */
int anco_linux_catalog_task_adopt_registered_process(AncoLinuxCatalog *catalog,
    const AncoLinuxProcess *process, AncoLinuxLaunch *out);
/* Stock ordinary MAIN/LAUNCHER branch for an explicitly registered STANDARD
 * component. Caller must first project/validate the original Want's filter,
 * flags and guest user (see anco_ams_start_launcher). A known real launcher
 * task may be selected without spawning. Unknown explicit hint does NOT fall
 * through to implicit matching. No first-frame or host Ability prerequisite.
 * Full document/actions/affinity/multi-Activity/recent restoration and display
 * traversal are unfinished; ambiguous or untyped live tasks fail explicitly.
 * This is not the complete application compatibility goal.
 */
int anco_linux_catalog_start_launcher(AncoLinuxCatalog *catalog,
                                      const char *bundle, const char *ability,
                                      gint32 task_hint, AncoLinuxLaunch *out);
/* Same original ordinary selector, exposing its actual new/reused branch.
 * Outputs change only on success. Never infer NEW Task from ID/PID changes. */
int anco_linux_catalog_select_launcher(AncoLinuxCatalog *catalog,
    const char *bundle, const char *ability, gint32 task_hint,
    AncoLinuxLaunch *out, gboolean *created);
/* Revalidate a selected task against this catalog's real live child ownership.
 * Not a task-selection algorithm; never adopts IDs returned by external callers.
 */
int anco_linux_catalog_task(AncoLinuxCatalog *catalog, gint32 task,
                            AncoLinuxLaunch *out);
/* Exact supportWindowModes retained from the registered component metadata.
 * Recovery has already compared these values with the same-round original BMS
 * AbilityInfo reply before a recovered package is accepted.  The caller gets
 * the original enum values (0..3), never a host window-mode bitmask. */
int anco_linux_catalog_task_window_modes(AncoLinuxCatalog *catalog, gint32 task,
    gint32 modes[4], guint *count);
/* Real previously-created identity, including a finished/reaped direct child.
 * Never admits a new/live process. Finished identities remain in recent-task
 * collision storage; live task selection must still use _task above. Strings
 * are borrowed until catalog destruction. No recent-task removal API yet.
 */
int anco_linux_catalog_task_record(AncoLinuxCatalog *catalog, gint32 task,
                                   AncoLinuxLaunch *out, gboolean *finished);
/* Trusted runtime ONLY after actual Task selection and original window/input/
 * IME/Graphic cleanup. Retires this Task's identity, Activities and bindings;
 * keeps the actual child/watch, sibling Tasks and retained recent identity.
 * No process signal, host message, fake child wait, or role/close inference.
 * Runtime owns original finish/removal notifications and their ordering.
 * Repeated retirement of the same retained Task is harmless. A live process
 * may have no active Task; its actual child watch must still reap it later.
 */
int anco_linux_catalog_task_retire(AncoLinuxCatalog *catalog, gint32 task);
/* Trusted common-runtime semantic Activity admission AFTER launch/window
 * policy selects this real Task and registered component. Same launched
 * process/component only; no exec, extra Task, host notification or inference
 * from set_parent/title/app_id. Output is unchanged on failure.
 */
int anco_linux_catalog_activity_create(AncoLinuxCatalog *catalog, gint32 task,
                                       AncoLinuxLaunch *out);
int anco_linux_catalog_activity(AncoLinuxCatalog *catalog, gint32 activity,
                                AncoLinuxLaunch *out);
/* Identity retirement only; the common WMS runtime must perform original
 * export/IME/input/Graphic cleanup FIRST. Removes this Activity's bindings,
 * leaving sibling Activities, process and Task intact. Root Activity removal
 * does not invent a replacement root or make ordinary launcher reuse succeed.
 */
int anco_linux_catalog_activity_remove(AncoLinuxCatalog *catalog, gint32 activity);
int anco_linux_catalog_bind_surface_for_activity(AncoLinuxCatalog *catalog,
    gint32 activity, const void *client, const void *surface,
    pid_t pid, uid_t uid, gid_t gid, AncoLinuxSurfaceOwner *out);
/* Trusted XWM/multiplexer path for the actual GUI process below a retained
 * launcher process. The ownership core validates full identity and ancestry. */
int anco_linux_catalog_bind_surface_for_process(AncoLinuxCatalog *catalog,
    gint32 activity,const void *client,const void *surface,
    const AncoLinuxProcess *process,AncoLinuxSurfaceOwner *out);
/* The Wayland adapter must supply actual client/resource and peer credentials.
 * Exact launched process only; helpers/reused toolkit servers need separately
 * admitted process ownership, never a title/app_id/parent-PID fallback.
 * New surfaces default to the real initial Activity. Existing bindings retain
 * their explicit Activity; this API never silently reparents them. Tasks query
 * launch identity, not a current top/focused Activity or window-selection rule.
 */
int anco_linux_catalog_bind_surface(AncoLinuxCatalog *catalog,
                                    const void *client, const void *surface,
                                    pid_t pid, uid_t uid, gid_t gid,
                                    AncoLinuxSurfaceOwner *out);
int anco_linux_catalog_remove_surface(AncoLinuxCatalog *catalog,
                                      const void *client, const void *surface);
void anco_linux_catalog_disconnect(AncoLinuxCatalog *catalog, const void *client);
/* Shared canonical Window IDs: all display/role adapters in this runtime use
 * the same allocator, not one cursor per Wayland adapter. See ownership.h for
 * the explicit original-resource setup and logical-window admission boundary.
 */
int anco_linux_catalog_windows_configure(AncoLinuxCatalog *catalog,
                                         gint32 resource_value);
int anco_linux_catalog_window_reserve_transient(AncoLinuxCatalog *catalog,
                                                gint32 activity, gint32 *out);
int anco_linux_catalog_window_add(AncoLinuxCatalog *catalog, const void *client,
                                  const void *window, const void *surface,
                                  AncoLinuxWindowOwner *out);
int anco_linux_catalog_window_owner(AncoLinuxCatalog *catalog, const void *client,
                                    const void *window, AncoLinuxWindowOwner *out);
int anco_linux_catalog_window_remove(AncoLinuxCatalog *catalog, const void *client,
                                     const void *window);
/* Explicit caller lifecycle action, no timed kills. SIGTERM/SIGKILL only, and
 * only this catalog's unreaped direct child. This is a PROCESS signal: all
 * Tasks sharing that process are affected; it is not closing one Task/window.
 * Child reaping must be exclusive
 * to this module/main context. No process-tree/group cleanup is claimed here.
 */
int anco_linux_catalog_signal(AncoLinuxCatalog *catalog, gint32 task, int signal);
guint anco_linux_catalog_children(AncoLinuxCatalog *catalog);
/* Semantic guest identity of this session; never a POSIX credential. */
gint32 anco_linux_catalog_user(AncoLinuxCatalog *catalog);
gboolean anco_linux_catalog_privacy(AncoLinuxCatalog *catalog);

#endif
