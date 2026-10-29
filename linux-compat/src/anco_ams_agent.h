/* Original guest AMS endpoint with Linux-side execution integration. No new
 * host protocol or task selection policy. Serialized GLib default context.
 */
#ifndef ANCO_AMS_AGENT_H
#define ANCO_AMS_AGENT_H
#include "anco_linux_catalog.h"

typedef struct AncoAmsAgent AncoAmsAgent;
typedef struct AncoAmsWantParam AncoAmsWantParam;
struct AncoAmsWantParam {
    gint32 type;
    union {
        gint32 integer;
        char *string;
        /* Original VALUE_TYPE_WANTPARAMS (101). Owned recursive parameter
         * table with the same key/value representation as the outer Want. */
        GHashTable *want_params;
    } value;
};
typedef struct {
    gint32 session_id, flags;
    char *action, *uri, *bundle, *element_bundle, *ability, *device;
    GPtrArray *entities;
    GHashTable *params;
    /* Linux-side semantic projection of stock startActivityFromBroker options.
     * Derived before start callback; raw Want/params stay unchanged. A hint can
     * name a retained/dead task or be -1. It is NOT reuse/activation success.
     * launchTaskId takes precedence; oh_persistentId is a per-user mission.
     */
    gint32 launch_guest_user, launch_task_hint;
    /* Original hasExtra/getIntExtra(default1), distinct from an absent option.
     * Requested mode is NOT the actual host mode or host-window readiness. */
    gboolean launch_window_mode_present;
    gint32 launch_window_mode;
} AncoAmsWant;
/* Borrowed Want and typed payloads valid only for this synchronous call.
 * The shared runtime selects a real new/reused task, preserving original
 * launch/user/lifecycle semantics. No host Ability or first-frame prerequisite.
 * Nonzero result or a foreign/dead/nonpositive task is returned as failure.
 * Full original Want type coverage is not supplied by this callback API.
 * Ordinary session0 also requires explicit original launcher-runtime
 * configuration below. Other callbacks remain positive-session only.
 */
typedef int (*AncoAmsStartFunc)(AncoLinuxCatalog *catalog,
                               const AncoAmsWant *want, gint32 *selected_task,
                               void *user_data);
/* Production callback for the original ordinary MAIN/LAUNCHER root filter.
 * Actual accepted registration and actual catalog processes/tasks, no selection
 * fixture, shell launch, private host Ability or frame wait. Other Intent,
 * action/document/other-window-mode/push-converter/multi-Activity branches remain unfinished and
 * explicitly fail; this callback alone is not a complete GUI runtime.
 * Pass only through the Agent's prepared Want path. user_data is unused, so a
 * common runtime may retain its own session data for the unlock callback.
 */
int anco_ams_start_launcher(AncoLinuxCatalog *catalog, const AncoAmsWant *want,
                            gint32 *selected_task, void *user_data);
/* Begin the actual guest-user session lifecycle once per bound catalog. This
 * notification is NOT a session-ready Boolean. Runtime owns asynchronous
 * initialization/failure/cleanup and must check actual readiness before launch.
 */
typedef void (*AncoAmsUnlockFunc)(AncoLinuxCatalog *catalog, gint32 guest_user,
                                 void *user_data);

/* Production only: original SA66866, original UID5557 caller. Catalog and user
 * data must outlive endpoint. Does not register automatically or open Binder.
 * Caller supplies actual IPC and accepted catalog shared with Wayland.
 */
AncoAmsAgent *anco_ams_agent_new(GBinderIpc *ipc, AncoLinuxCatalog *catalog,
                                AncoAmsStartFunc start, AncoAmsUnlockFunc unlock,
                                void *user_data);
/* Corresponds to original ordinary guest-user0 startup account query. Uses
 * original reverse AMS code10(host100); the stock BMS creates that mapping.
 * Host method has stock property/subscription side effects: not a read-only
 * probe. Missing/unverified broker leaves event-driven retry pending. This
 * initial-boot API deliberately refuses other/private sessions until wired.
 */
int anco_ams_agent_boot_default_user(AncoAmsAgent *agent);
int anco_ams_agent_register(AncoAmsAgent *agent);
/* Trusted common-runtime observation AFTER successful selection and live
 * catalog validation, BEFORE the original start reply. Called for both new
 * and reused selections; do not infer a new Task from this event. Borrowed
 * Want/identity expire on return: copy values needed by later window events.
 * No nested dispatch, catalog mutation, agent free or observer replacement.
 * Queue work for the shared context; this does not prove host-window readiness,
 * mission association, reverse notification success or input/graphics setup.
 */
typedef void (*AncoAmsSelectedFunc)(AncoLinuxCatalog *catalog,
    const AncoAmsWant *want, const AncoLinuxLaunch *selected, void *user_data);
/* Single observer, no replay. Clear using matching user_data before its owner
 * is destroyed. Does not replace the original launcher selection callback. */
int anco_ams_agent_on_task_selected(AncoAmsAgent *agent,
    AncoAmsSelectedFunc observer, void *user_data);

/* Trusted shared-runtime observer of the ACTUAL catalog child exit, after AMS
 * finish/pending handling. Does not replace the catalog's sole AMS listener.
 * Borrowed retained launch identity; copy needed values when queuing work.
 * Runs with Agent busy and catalog exit_depth active: no nested dispatch,
 * free, listener replacement, catalog mutation or immediate Task removal.
 * The runtime must perform original window/input/IME/Graphic retirement
 * separately. This notification is neither removal nor cleanup completion.
 * One observer; no replay. Clear with matching data before freeing its owner.
 */
int anco_ams_agent_on_child_exit(AncoAmsAgent *agent,
    AncoLinuxExitFunc observer, void *user_data);
/* Linux consumers of the original validated code13 configuration. Immutable
 * borrowed map (display prefix removed), lifetime until callback return.
 * Single observer, no replay. Same serialized context, Agent busy: no nested
 * dispatch, Agent free or observer replacement. Does not fabricate properties
 * or change the original host transaction. Clear with matching user_data. */
typedef void (*AncoAmsConfigurationFunc)(gint32 display, gint32 user,
    GHashTable *configuration, void *user_data);
int anco_ams_agent_on_configuration(AncoAmsAgent *agent,
    AncoAmsConfigurationFunc observer, void *user_data);
/* Trusted Linux-side observation of an accepted original mission callback,
 * AFTER its pending finish/removal replay. associated reports whether this
 * exact mission still maps to this task at that instant; it can already be
 * false. Revalidate on later dispatch. Neither true nor receipt means a live
 * application, visible host window or working input/graphics. No replay.
 * Synchronous busy callback: no nested dispatch, catalog/agent mutation, free
 * or observer replacement. Single observer; clear with matching user_data.
 */
typedef void (*AncoAmsMissionFunc)(AncoLinuxCatalog *catalog, gint32 guest_user,
    gint32 mission, gint32 task, gboolean associated, void *user_data);
int anco_ams_agent_on_mission(AncoAmsAgent *agent,
    AncoAmsMissionFunc observer, void *user_data);

/* Linux semantic equivalent of the original guest ActivityTaskManager task
 * removal callback. Inbound original AMS agent code5/code6 resolves host
 * mission IDs through the retained original mission map before this callback.
 * The runtime must request standard application teardown for this exact Task;
 * it must not kill a shared Linux process or invent a host/window protocol.
 * reason is borrowed for the synchronous callback only. Duplicate/already
 * retired host notifications may have no mapped Task and are intentionally not
 * replayed. One observer; clear with matching user_data before owner teardown.
 */
typedef void (*AncoAmsRemoveTaskFunc)(AncoLinuxCatalog *catalog, gint32 task,
    const char *reason, void *user_data);
int anco_ams_agent_on_task_remove(AncoAmsAgent *agent,
    AncoAmsRemoveTaskFunc observer, void *user_data);

/* Original per-guest-user mission->task association. May resolve a retained
 * finished task; caller must check actual live catalog state before selection.
 * This is not an activation or reuse algorithm. No mission/task ID conflation.
 */
int anco_ams_agent_task_for_mission(AncoAmsAgent *agent, gint32 guest_user,
                                    gint32 mission, gint32 *task);
/* Trusted runtime ONLY after actual Task window/input/IME/Graphic cleanup and
 * catalog identity retirement. Replays the stock guest-origin finish branch:
 * an associated mission receives reverse code3 with originalFinish=true;
 * a start still awaiting mission association is retained in pendingFinishes
 * and replayed by the original mission-created path with originalFinish=false.
 * No host API, parcel shape, mission association or removal policy is added.
 * Process exit uses this same branch through the catalog observer; this entry
 * exists for a semantic Task whose shared Linux process remains alive. */
int anco_ams_agent_notify_task_finished(AncoAmsAgent *agent, gint32 task);
/* Trusted runtime ONLY after actual Task removal and original window/input/
 * IME/Graphic cleanup. Retained, retired catalog identity is required; the
 * current integration refuses a still-live Task. A child wait does not call
 * this automatically: process exit is NOT Task removal. Original pending
 * removed Boolean, mission unlink and reverse code5 ONE-WAY are preserved.
 * Return0 is local handling, never a remote cleanup acknowledgement. Missing
 * association/broker and delivery failure do not trigger retries or signals.
 * Does not retire catalog history or implement inbound5/35 removal policy.
 */
int anco_ams_agent_notify_task_removed(AncoAmsAgent *agent, gint32 task);
typedef struct {
    gint32 uid;
    /* Actual Request.callingPackage, or actual registry-resolved name when
     * that field was empty. Unresolved UID/name lookup is not guessed. */
    const char *package;
} AncoAmsCaller;
typedef struct {
    gint32 activity, source_activity, broker_session;
    const AncoAmsCaller *caller;
    gboolean launch_task_behind;
    /* Actual paired guest properties, not a hardcoded current-host version:
     * sys.anco.broker.amsbroker.version / sys.anco.multiwindow.enable.
     * Preserve stock ASCII String.compareTo("1.0.0") plus feature gate. */
    const char *broker_version;
    gboolean multiwindow_enabled;
    /* Original updateTaskVisibleShortCut result: getVisibility(start)!=0,
     * !Home/Recents leaf Tasks, in original traversal order. NOT visible IDs.
     * Actual retained Task identity can outlive its application process. */
    const gint32 *background_tasks;
    gsize background_task_count;
} AncoAmsActivityStart;
typedef struct {
    gboolean suppressed, attempted, decoded;
    guint32 code;
    int transport;
    /* First service int only if decoded; otherwise original wrapper fallback
     * (0, or -1 for native unsupported). Never infer host success from this. */
    gint32 result;
} AncoAmsActivityStartResult;
/* Trusted common runtime ONLY after actual NEW Task selection. Initial
 * Activity must belong to a real live catalog launch. source_activity=-1
 * means no source Activity; otherwise its actual Task supplies sourceTaskId.
 * Original host uses the source Task's mission/listener/Ability Context for
 * launch routing. This field is not a native parent-window or modal contract;
 * it does not embed the new Task's windows or complete cross-Task focus policy.
 * Caller is the separate real Request UID/package; never infer it from the
 * source window or host/POSIX UID. uid=-1 explicitly omits caller params.
 * Subsequent
 * Activities in an existing Task are not this ActivityStarter notification.
 * Positive broker session / pending / associated Task suppress as original.
 * Return0 means local preparation, not transport/service/host-window success.
 * The original pending marker survives delivery failure; no guessed retry or
 * timed kill. Background IDs must come from actual shared Task visibility.
 * This API does not compute visibility or implement a complete GUI runtime.
 */
int anco_ams_agent_notify_activity_start(AncoAmsAgent *agent,
    const AncoAmsActivityStart *start, AncoAmsActivityStartResult *out);
/* Semantic equivalent of updateTaskVisibleShortCut(startActivity). Append
 * only actual nonvisible, non-Home/Recents leaf Tasks, preserving traversal.
 * Must query shared Task/Window state; catalog process liveness is not it.
 * The callback may not dispatch another main loop or mutate resources.
 * Output array is owned by the Agent; callback/data lifetime is runtime-owned.
 */
typedef int (*AncoAmsBackgroundTasksFunc)(AncoLinuxCatalog *catalog,
    const AncoLinuxLaunch *start, GArray *tasks, void *user_data);
typedef struct {
    const char *broker_version;
    gboolean multiwindow_enabled;
    AncoAmsCaller context_caller;
    AncoAmsBackgroundTasksFunc background_tasks;
    void *user_data;
} AncoAmsLauncherRuntime;
/* Once, after actual session/config/Context caller preparation, on the shared
 * serialized default GLib context. Copies strings; does not set readiness,
 * collect properties, fabricate UID1000 or change native Binder identity.
 * Enables only ordinary MAIN/LAUNCHER session0 selection->original reverse
 * notification. V2 requires the real visibility callback. CLI has no runtime.
 */
int anco_ams_agent_configure_launcher(AncoAmsAgent *agent,
                                      const AncoAmsLauncherRuntime *runtime);
/* Common-runtime NEW Task notification using its configured original broker
 * version/multiwindow feature and actual background-Task collector. Shared by
 * ordinary session0 launch and trusted Linux-side new-Task admission, including
 * an already running process. No creation, selection, role inference or exec.
 * Requires a live catalog initial Activity and configured launcher runtime.
 * Caller identity is the actual Request caller, separate from source Activity;
 * -1 source means absent. No automatic Context identity substitution.
 * Same serialized context and borrowed-callback restrictions as above. The
 * collector runs under Agent busy; it must not dispatch/mutate the runtime.
 * It is a state query, not a notification/activation action. Original pending,
 * positive-session and mission suppression remain in notify_activity_start.
 * The same original gates run BEFORE this wrapper's background query, so a
 * suppressed notification cannot fail because of an irrelevant collector.
 * Errors leave out unchanged. An admitted Task survives notification/collector
 * failure as before; caller owns actual lifecycle, not rollback or timed kill.
 * No implicit retries or promise of a host window, buffer or input readiness.
 */
int anco_ams_agent_notify_runtime_start(AncoAmsAgent *agent,
    gint32 activity, gint32 source_activity, gint32 broker_session,
    const AncoAmsCaller *caller, gboolean launch_task_behind,
    AncoAmsActivityStartResult *out);

typedef struct {
    gboolean attempted, decoded;
    guint32 code;
    int transport;
    gint32 result;
} AncoAmsMoveToFrontResult;
/* Linux semantic equivalent of the stock AncoActivityManager
 * notifyMoveTaskToFront path. The Task must already have its real host mission;
 * this does not create/recover a host Window or fabricate foreground state.
 * Feature 1.0.0 uses original synchronous transaction26 and the exact
 * updateTaskVisibleShortCut result supplied by the shared runtime; legacy uses
 * transaction7. The proxy reads only the first reply int. Return0 is local
 * preparation, while transport/decoded/result remain separate evidence.
 * Call only from the serialized context outside Agent observer callbacks. */
int anco_ams_agent_notify_runtime_front(AncoAmsAgent *agent, gint32 task,
    const gint32 *background_tasks, gsize background_task_count,
    AncoAmsMoveToFrontResult *out);

/* Returns error without freeing if original SA removal fails. Does not kill
 * app children or unregister BMS packages; common runtime owns that lifecycle.
 * Refuses active children or callbacks; stop/reap children before freeing.
 * Detaches its catalog exit listener only after successful SA removal.
 */
int anco_ams_agent_free(AncoAmsAgent *agent);
#endif
