/* L410 AnCo AMS agent: first working part of the original guest-side service.
 * The private SA mode exercises Binder without replacing production SA 66866.
 * Unknown business calls explicitly fail until Linux task execution is wired.
 */
#include <gbinder.h>
#include "anco_ams_agent.h"
#include "gbinder_ipc.h"
#include "gbinder_local_request_p.h"
#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define PRIVATE_SA 66990
#define PRODUCTION_SA 66866
#define AMS_BROKER_SA 66818
#define BROKER_UID 5557
#define AGENT_IFACE "rgm.ancoproxy.agent.IAncoAmsAgent"
#define PRIVATE_BROKER_CONFIG "/data/vendor/l410-linux/typed-window/broker-graphics-wms-ams-v123.json"

struct AncoAmsAgent {
    GBinderIpc *ipc;
    GBinderRemoteObject *ams;
    gulong ams_death;
    GMainLoop *loop;
    int sa_id;
    GHashTable *configuration;
    char *locale;
    int display_id;
    int user_id;
    AncoLinuxCatalog *catalog;
    AncoAmsStartFunc start;
    AncoAmsUnlockFunc unlock;
    void *start_data;
    GBinderLocalObject *object;
    gboolean published;
    gboolean unlock_requested;
    gboolean unlock_retry;
    GHashTable *missions;
    GHashTable *pending_starts;
    GHashTable *pending_finishes;
    AncoAmsLauncherRuntime launcher_runtime;
    gboolean launcher_configured;
    gboolean exit_attached;
    AncoLinuxExitFunc exit_observer;
    void *exit_observer_data;
    AncoAmsSelectedFunc selected_observer;
    void *selected_observer_data;
    AncoAmsMissionFunc mission_observer;
    void *mission_observer_data;
    AncoAmsRemoveTaskFunc remove_task_observer;
    void *remove_task_observer_data;
    AncoAmsConfigurationFunc configuration_observer;
    void *configuration_observer_data;
    guint busy;
};
typedef AncoAmsAgent Agent;

static void append_task_finished(GBinderLocalRequest *request, gint32 mission,
                                  gint32 task, gboolean original_finish_flag)
{
    /* IAmsBrokerProxy.notifyTaskFinished: code3, int/int/writeBoolean.
     * Boolean is four bytes on the original parcel, never a byte flag.
     */
    gbinder_local_request_append_int32(request, mission);
    gbinder_local_request_append_int32(request, task);
    gbinder_local_request_append_int32(request, original_finish_flag ? 1 : 0);
}

static void notify_task_finished(Agent *agent, gint32 mission, gint32 task,
                                  gboolean original_finish_flag)
{
    int transport = -ENOTCONN;
    gint32 result = -1;
    gboolean decoded = FALSE;
    if (agent->ams) {
        GBinderClient *client = gbinder_client_new(agent->ams, "OHOS.Anco.Service.Ams");
        GBinderLocalRequest *request = client ? gbinder_client_new_request(client) : NULL;
        if (request) {
            append_task_finished(request, mission, task, original_finish_flag);
            GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(client, 3,
                                                                          request, &transport);
            if (!transport && reply) {
                GBinderReader reader;
                gbinder_remote_reply_init_reader(reply, &reader);
                decoded = gbinder_reader_read_int32(&reader, &result) &&
                    gbinder_reader_at_end(&reader);
            }
            if (reply) gbinder_remote_reply_unref(reply);
            gbinder_local_request_unref(request);
        } else transport = -ENOMEM;
        if (client) gbinder_client_unref(client);
    }
    /* Stock high-level wrapper does not retry or await window destruction.
     * Report transport, decoding and business result separately; zero is NOT
     * evidence that SceneBoard has finished its deferred termination.
     */
    printf("AMS_TASK_FINISH_NOTIFY mission=%d task=%d original_finish_flag=%d transport=%d decoded=%d result=%d\n",
           mission, task, original_finish_flag, transport, decoded, result);
    fflush(stdout);
}

static void append_activity_start_want(GBinderLocalRequest *request,
    const AncoLinuxLaunch *activity, const AncoAmsCaller *caller, gboolean behind)
{
    /* Stock ActivityInfo path: new Want(), package/name, then caller params.
     * Operation's action="", empty nonnull entities, flags0, empty URI;
     * unset device/module are null. ElementName writes version1 and three
     * strings; moduleName is NOT a fourth serialized ElementName string. */
    gbinder_local_request_append_int32(request, 1); /* Want present */
    gbinder_local_request_append_string16(request, "");
    gbinder_local_request_append_int32(request, -1); /* no URI */
    gbinder_local_request_append_int32(request, 1); /* entities present */
    gbinder_local_request_append_int32(request, 0); /* empty array */
    gbinder_local_request_append_int32(request, 0); /* flags */
    gbinder_local_request_append_int32(request, 1); /* element present */
    gbinder_local_request_append_int32(request, 1); /* element version */
    gbinder_local_request_append_string16(request, activity->app.bundle);
    gbinder_local_request_append_string16(request, activity->app.ability);
    gbinder_local_request_append_string16(request, NULL); /* unset device */
    gbinder_local_request_append_int32(request, 1); /* params present */
    gbinder_local_request_append_int32(request, 1); /* params version */
    gbinder_local_request_append_int32(request, caller->uid != -1 ? 4 : 2);
    gbinder_local_request_append_string16(request, "launchTaskBehind");
    gbinder_local_request_append_int32(request, 1); /* Boolean type */
    gbinder_local_request_append_int32(request, behind ? 1 : 0);
    /* SP68 AmsBrokerService::OnStartAbility copies Want.GetBundle() into this
     * existing parameter before targeting ShellAssistant MainAbility. Host 6.0
     * lacks that bounded producer step, so preserve the same final Want for a
     * Linux semantic Task at the original reverse-notification boundary. */
    gbinder_local_request_append_string16(request, "bundleName");
    gbinder_local_request_append_int32(request, 9); /* String type */
    gbinder_local_request_append_string16(request, activity->app.bundle);
    if (caller->uid != -1) {
        gbinder_local_request_append_string16(request, "ohos.anco.param.callerUid");
        gbinder_local_request_append_int32(request, 5); /* Integer type */
        gbinder_local_request_append_int32(request, caller->uid);
        gbinder_local_request_append_string16(request, "ohos.anco.param.callerBundleName");
        gbinder_local_request_append_int32(request, 9); /* String type */
        gbinder_local_request_append_string16(request, caller->package);
    }
    gbinder_local_request_append_string16(request, activity->app.bundle);
}

/* Original outer notifyActivityRealStart gates precede the inner visibility
 * query as well as the reverse transaction. Shared by both local entrances. */
static gboolean activity_start_suppressed(AncoAmsAgent *agent, gint32 task,
    gint32 session)
{
    if (session > 0 || (agent->pending_starts &&
        g_hash_table_contains(agent->pending_starts, GINT_TO_POINTER(task))))
        return TRUE;
    if (agent->missions) {
        GHashTableIter iter; gpointer value;
        g_hash_table_iter_init(&iter, agent->missions);
        while (g_hash_table_iter_next(&iter, NULL, &value))
            if (GPOINTER_TO_INT(value) == task) return TRUE;
    }
    return FALSE;
}

int anco_ams_agent_notify_activity_start(AncoAmsAgent *agent,
    const AncoAmsActivityStart *start, AncoAmsActivityStartResult *out)
{
    if (!agent || !agent->catalog || !start || !out) return -EINVAL;
    AncoAmsActivityStartResult result = {.transport = -ENOTCONN, .result = 0};
    AncoLinuxLaunch activity, source;
    int error = anco_linux_catalog_activity(agent->catalog, start->activity, &activity);
    if (error) return error;
    AncoLinuxLaunch root;
    error = anco_linux_catalog_task(agent->catalog, activity.task_id, &root);
    if (error) return error;
    if (root.activity_id != activity.activity_id) return -ENOTSUP;
    if (activity_start_suppressed(agent, activity.task_id, start->broker_session)) {
        result.suppressed = TRUE; *out = result; return 0;
    }
    if (!start->caller || start->caller->uid < -1) return -EINVAL;
    if (start->caller->uid != -1 && (!start->caller->package ||
        !*start->caller->package)) return -ENODATA; /* actual UID/name resolver required */
    if (start->caller->uid != -1 &&
        !g_utf8_validate(start->caller->package, -1, NULL)) return -EINVAL;
    if (!start->broker_version || !*start->broker_version) return -ENODATA;
    for (const char *p = start->broker_version; *p; ++p)
        if ((unsigned char)*p > 127) return -ENOTSUP; /* ASCII paired version */
    const gboolean v2 = start->multiwindow_enabled &&
        strcmp(start->broker_version, "1.0.0") >= 0;
    if (v2 && (start->background_task_count > G_MAXINT32 ||
        (start->background_task_count && !start->background_tasks))) return -EINVAL;
    if (start->source_activity != -1) {
        error = anco_linux_catalog_activity(agent->catalog, start->source_activity, &source);
        if (error) return error;
    }
    if (v2) {
        for (gsize i = 0; i < start->background_task_count; ++i) {
            AncoLinuxLaunch background; gboolean finished;
            error = anco_linux_catalog_task_record(agent->catalog,
                start->background_tasks[i], &background, &finished);
            if (error) return error;
        }
    }
    if (!agent->pending_starts) agent->pending_starts = g_hash_table_new(g_direct_hash, g_direct_equal);
    if (!agent->pending_finishes) agent->pending_finishes = g_hash_table_new(g_direct_hash, g_direct_equal);
    /* Stock inserts BEFORE the reverse call and never removes on error. */
    g_hash_table_add(agent->pending_starts, GINT_TO_POINTER(activity.task_id));
    ++agent->busy;
    result.code = v2 ? 25 : 1;
    if (agent->ams) {
        GBinderClient *client = gbinder_client_new(agent->ams, "OHOS.Anco.Service.Ams");
        GBinderLocalRequest *request = client ? gbinder_client_new_request(client) : NULL;
        if (request) {
            gbinder_local_request_append_int32(request, activity.task_id);
            append_activity_start_want(request, &activity, start->caller,
                start->launch_task_behind);
            gbinder_local_request_append_int32(request,
                start->source_activity == -1 ? -1 : source.task_id);
            if (v2) {
                gbinder_local_request_append_int32(request, (gint32)start->background_task_count);
                for (gsize i = 0; i < start->background_task_count; ++i)
                    gbinder_local_request_append_int32(request, start->background_tasks[i]);
            }
            result.attempted = TRUE;
            GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(client, result.code,
                request, &result.transport);
            if (!result.transport && reply) {
                GBinderReader reader; gbinder_remote_reply_init_reader(reply, &reader);
                result.decoded = gbinder_reader_read_int32(&reader, &result.result);
                /* Proxy reads first int, no readException or EOF assertion. */
            }
            /* Original native UNKNOWN_TRANSACTION returns transact=false;
             * Proxy returns-1. Other transport exceptions reach the outer
             * AmsBrokerProxy's fallback0. Neither is a decoded service result.
             * Retain raw transport and never add a retry. */
            if (result.transport == -EBADMSG) result.result = -1;
            if (reply) gbinder_remote_reply_unref(reply);
            gbinder_local_request_unref(request);
        } else result.transport = -ENOMEM;
        if (client) gbinder_client_unref(client);
    }
    --agent->busy;
    *out = result;
    return 0;
}

int anco_ams_agent_configure_launcher(AncoAmsAgent *agent,
                                      const AncoAmsLauncherRuntime *runtime)
{
    if (!agent || !runtime) return -EINVAL;
    if (agent->busy) return -EBUSY;
    if (!agent->catalog || agent->start != anco_ams_start_launcher) return -ENOTSUP;
    if (agent->launcher_configured) return -EALREADY;
    if (!runtime->broker_version || !*runtime->broker_version ||
        runtime->context_caller.uid < 0 || !runtime->context_caller.package ||
        !*runtime->context_caller.package) return -ENODATA;
    for (const char *p = runtime->broker_version; *p; ++p)
        if ((unsigned char)*p > 127) return -ENOTSUP;
    if (!g_utf8_validate(runtime->context_caller.package, -1, NULL)) return -EINVAL;
    if (runtime->multiwindow_enabled && strcmp(runtime->broker_version, "1.0.0") >= 0 &&
        !runtime->background_tasks) return -ENODATA;
    agent->launcher_runtime = *runtime;
    agent->launcher_runtime.broker_version = g_strdup(runtime->broker_version);
    agent->launcher_runtime.context_caller.package = g_strdup(runtime->context_caller.package);
    agent->launcher_configured = TRUE;
    return 0;
}

int anco_ams_agent_task_for_mission(AncoAmsAgent *agent, gint32 user,
                                    gint32 mission, gint32 *task)
{
    if (!agent || !agent->catalog || !task) return -EINVAL;
    if (user != anco_linux_catalog_user(agent->catalog) || !agent->missions) return -ENOENT;
    gpointer value = g_hash_table_lookup(agent->missions, GINT_TO_POINTER(mission));
    if (!value) return -ENOENT;
    AncoLinuxLaunch record; gboolean finished;
    const gint32 selected = GPOINTER_TO_INT(value);
    const int result = anco_linux_catalog_task_record(agent->catalog, selected,
                                                       &record, &finished);
    if (result) return result;
    *task = record.task_id;
    return 0;
}

int anco_ams_agent_notify_runtime_front(AncoAmsAgent *agent, gint32 task,
    const gint32 *background_tasks, gsize background_task_count,
    AncoAmsMoveToFrontResult *out)
{
    if (!agent || !out || task <= 0) return -EINVAL;
    if (agent->busy) return -EBUSY;
    if (!agent->catalog || !agent->missions || !agent->launcher_configured)
        return -ENODATA;
    const AncoAmsLauncherRuntime *runtime = &agent->launcher_runtime;
    if (!runtime->broker_version || !*runtime->broker_version) return -ENODATA;
    const gboolean v2 = runtime->multiwindow_enabled &&
        strcmp(runtime->broker_version, "1.0.0") >= 0;
    if (v2 && (background_task_count > G_MAXINT32 ||
        (background_task_count && !background_tasks))) return -EINVAL;
    AncoLinuxLaunch record; gboolean finished;
    int local = anco_linux_catalog_task_record(agent->catalog, task, &record,
                                                &finished);
    if (local) return local;
    if (finished) return -ESTALE;
    gint32 mission = -1;
    GHashTableIter iter; gpointer key, value;
    g_hash_table_iter_init(&iter, agent->missions);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (GPOINTER_TO_INT(value) == task) {
            mission = GPOINTER_TO_INT(key);
            break;
        }
    }
    if (mission < 0) return -ENOENT;
    if (v2) {
        for (gsize i = 0; i < background_task_count; ++i) {
            AncoLinuxLaunch background; gboolean background_finished;
            local = anco_linux_catalog_task_record(agent->catalog,
                background_tasks[i], &background, &background_finished);
            if (local) return local;
        }
    }
    AncoAmsMoveToFrontResult result = {.code = v2 ? 26 : 7,
        .transport = -ENOTCONN, .result = -1};
    ++agent->busy;
    if (agent->ams) {
        GBinderClient *client = gbinder_client_new(agent->ams,
                                                   "OHOS.Anco.Service.Ams");
        GBinderLocalRequest *request = client ?
            gbinder_client_new_request(client) : NULL;
        if (request) {
            gbinder_local_request_append_int32(request, mission);
            gbinder_local_request_append_int32(request, task);
            if (v2) {
                gbinder_local_request_append_int32(request,
                    (gint32)background_task_count);
                for (gsize i = 0; i < background_task_count; ++i)
                    gbinder_local_request_append_int32(request,
                        background_tasks[i]);
            }
            result.attempted = TRUE;
            GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(
                client, result.code, request, &result.transport);
            if (!result.transport && reply) {
                GBinderReader reader;
                gbinder_remote_reply_init_reader(reply, &reader);
                result.decoded = gbinder_reader_read_int32(&reader,
                                                            &result.result);
            }
            if (reply) gbinder_remote_reply_unref(reply);
            gbinder_local_request_unref(request);
        } else result.transport = -ENOMEM;
        if (client) gbinder_client_unref(client);
    }
    --agent->busy;
    *out = result;
    return 0;
}

static void finish_task_identity(Agent *agent, gint32 task, gint status)
{
    gint32 mission = -1;
    GHashTableIter iter; gpointer key, value;
    g_hash_table_iter_init(&iter, agent->missions);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (GPOINTER_TO_INT(value) == task) {
            mission = GPOINTER_TO_INT(key); break;
        }
    }
    if (mission != -1) notify_task_finished(agent, mission, task, TRUE);
    else if (agent->pending_starts && g_hash_table_contains(agent->pending_starts,
                                                            GINT_TO_POINTER(task))) {
        if (!agent->pending_finishes)
            agent->pending_finishes = g_hash_table_new(g_direct_hash, g_direct_equal);
        g_hash_table_insert(agent->pending_finishes, GINT_TO_POINTER(task), NULL);
        printf("AMS_GUEST_ORIGIN_FINISH_PENDING task=%d\n", task);
    } else printf("AMS_TASK_FINISH_NO_MISSION task=%d status=%d\n", task, status);
}

int anco_ams_agent_notify_task_finished(AncoAmsAgent *agent, gint32 task)
{
    if (!agent || task <= 0) return -EINVAL;
    if (agent->busy) return -EBUSY;
    if (!agent->catalog || !agent->missions) return -ENODATA;
    AncoLinuxLaunch record; gboolean finished;
    const int result = anco_linux_catalog_task_record(agent->catalog, task, &record, &finished);
    if (result) return result;
    if (!finished) return -EBUSY;
    ++agent->busy;
    finish_task_identity(agent, task, 0);
    --agent->busy;
    return 0;
}

static void notify_task_removed(Agent *agent, gint32 task)
{
    if (agent->pending_starts && g_hash_table_contains(agent->pending_starts,
                                                      GINT_TO_POINTER(task))) {
        if (!agent->pending_finishes)
            agent->pending_finishes = g_hash_table_new(g_direct_hash, g_direct_equal);
        g_hash_table_insert(agent->pending_finishes, GINT_TO_POINTER(task), GINT_TO_POINTER(TRUE));
        printf("AMS_TASK_REMOVE_PENDING task=%d\n", task);
        return;
    }
    gint32 mission = -1;
    GHashTableIter iter; gpointer key, value;
    g_hash_table_iter_init(&iter, agent->missions);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (GPOINTER_TO_INT(value) == task) {
            mission = GPOINTER_TO_INT(key);
            g_hash_table_iter_remove(&iter); /* original findMissionId(task,true) */
            break;
        }
    }
    if (mission == -1) return;
    int transport = -ENOTCONN;
    if (agent->ams) {
        GBinderClient *client = gbinder_client_new(agent->ams, "OHOS.Anco.Service.Ams");
        GBinderLocalRequest *request = client ? gbinder_client_new_request(client) : NULL;
        if (request) {
            gbinder_local_request_append_int32(request, mission);
            gbinder_local_request_append_int32(request, task);
            transport = gbinder_client_transact_sync_oneway(client, 5, request);
            gbinder_local_request_unref(request);
        } else transport = -ENOMEM;
        if (client) gbinder_client_unref(client);
    }
    /* Original void wrapper ignores the result/catches exceptions. Code5 is
     * ONE-WAY; its Java proxy's readInt is not a cleanup acknowledgement. */
    printf("AMS_TASK_REMOVE_NOTIFY mission=%d task=%d oneway=1 transport=%d\n",
           mission, task, transport);
}

int anco_ams_agent_notify_task_removed(AncoAmsAgent *agent, gint32 task)
{
    if (!agent || task <= 0) return -EINVAL;
    if (agent->busy) return -EBUSY;
    if (!agent->catalog || !agent->missions) return -ENODATA;
    AncoLinuxLaunch record; gboolean finished;
    const int result = anco_linux_catalog_task_record(agent->catalog, task, &record, &finished);
    if (result) return result;
    if (!finished) return -EBUSY;
    ++agent->busy;
    notify_task_removed(agent, task);
    --agent->busy;
    return 0;
}

int anco_ams_agent_on_mission(AncoAmsAgent *agent,
    AncoAmsMissionFunc observer, void *data)
{
    if (!agent || !agent->catalog || !agent->missions) return -EINVAL;
    if (agent->busy) return -EBUSY;
    if (observer && agent->mission_observer) return -EALREADY;
    if (!observer && agent->mission_observer && agent->mission_observer_data != data)
        return -EPERM;
    agent->mission_observer = observer;
    agent->mission_observer_data = observer ? data : NULL;
    return 0;
}

int anco_ams_agent_on_task_remove(AncoAmsAgent *agent,
    AncoAmsRemoveTaskFunc observer, void *data)
{
    if (!agent || !agent->catalog || !agent->missions) return -EINVAL;
    if (agent->busy) return -EBUSY;
    if (observer && agent->remove_task_observer) return -EALREADY;
    if (!observer && agent->remove_task_observer &&
        agent->remove_task_observer_data != data) return -EPERM;
    agent->remove_task_observer = observer;
    agent->remove_task_observer_data = observer ? data : NULL;
    return 0;
}

static void remove_task_from_runtime(Agent *agent, gint32 task,
                                     const char *reason)
{
    if (agent->remove_task_observer)
        agent->remove_task_observer(agent->catalog, task, reason,
                                    agent->remove_task_observer_data);
}

static int on_mission_removed(Agent *agent, GBinderRemoteRequest *request)
{
    GBinderReader reader;
    gint32 mission = -1;
    gbinder_remote_request_init_reader(request, &reader);
    const gboolean read_mission = gbinder_reader_read_int32(&reader, &mission);
    /* The original host callback uses Parcel::writeString8: an int32 byte
     * length followed by UTF-8, NUL and four-byte padding.  Do not confuse it
     * with readCString (gbinder_reader_read_string8), which has no length. */
    const char *reason = NULL;
    if (!read_mission ||
        !gbinder_reader_read_nullable_string8(&reader, &reason, NULL) ||
        !reason) return -1;
    gpointer value = NULL;
    const gboolean mapped = g_hash_table_lookup_extended(agent->missions,
        GINT_TO_POINTER(mission), NULL, &value);
    const gint32 task = mapped ? GPOINTER_TO_INT(value) : -1;
    if (mapped) {
        remove_task_from_runtime(agent, task, reason);
        if (!strcmp(reason, "missionRemovedFromRecent"))
            g_hash_table_remove(agent->missions, GINT_TO_POINTER(mission));
    }
    printf("AMS_MISSION_REMOVE mission=%d task=%d mapped=%d reason=%s trailing_bytes=%zu\n",
           mission, task, mapped, reason, gbinder_reader_bytes_remaining(&reader));
    return 0; /* Original AncoAmsAgent returns zero after clearIdentityCall. */
}

static int on_shell_process_removed(Agent *agent, GBinderRemoteRequest *request)
{
    GBinderReader reader;
    gint32 process = -1, count = -1;
    gbinder_remote_request_init_reader(request, &reader);
    const gboolean read_process = gbinder_reader_read_int32(&reader, &process);
    const char *reason = NULL;
    if (!read_process ||
        !gbinder_reader_read_nullable_string8(&reader, &reason, NULL) ||
        !reason || !gbinder_reader_read_int32(&reader, &count) || count < 0 ||
        (gsize)count > gbinder_reader_bytes_remaining(&reader) / sizeof(gint32))
        return -1;
    GArray *missions = g_array_sized_new(FALSE, FALSE, sizeof(gint32), (guint)count);
    for (gint32 i = 0; i < count; ++i) {
        gint32 mission;
        if (!gbinder_reader_read_int32(&reader, &mission)) {
            g_array_free(missions, TRUE);
            return -1;
        }
        g_array_append_val(missions, mission);
    }
    guint removed = 0;
    if (!strcmp(reason, "foundation_died")) {
        GArray *tasks = g_array_new(FALSE, FALSE, sizeof(gint32));
        GHashTableIter iter;
        gpointer value;
        g_hash_table_iter_init(&iter, agent->missions);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            const gint32 task = GPOINTER_TO_INT(value);
            g_array_append_val(tasks, task);
        }
        if (agent->pending_starts) {
            g_hash_table_iter_init(&iter, agent->pending_starts);
            gpointer key;
            while (g_hash_table_iter_next(&iter, &key, NULL)) {
                const gint32 task = GPOINTER_TO_INT(key);
                g_array_append_val(tasks, task);
            }
            g_hash_table_remove_all(agent->pending_starts);
        }
        if (agent->pending_finishes) g_hash_table_remove_all(agent->pending_finishes);
        g_hash_table_remove_all(agent->missions);
        for (guint i = 0; i < tasks->len; ++i) {
            remove_task_from_runtime(agent, g_array_index(tasks, gint32, i),
                                     "missionRemovedFromRecent");
            ++removed;
        }
        g_array_free(tasks, TRUE);
    } else if (strcmp(reason, "crash_die")) {
        if (agent->pending_starts) g_hash_table_remove_all(agent->pending_starts);
        for (guint i = 0; i < missions->len; ++i) {
            const gint32 mission = g_array_index(missions, gint32, i);
            gpointer value = NULL;
            if (g_hash_table_lookup_extended(agent->missions,
                    GINT_TO_POINTER(mission), NULL, &value)) {
                remove_task_from_runtime(agent, GPOINTER_TO_INT(value), "ShellProcDie");
                ++removed;
            }
        }
    }
    printf("AMS_SHELL_PROCESS_REMOVE process=%d reason=%s missions=%d tasks=%u trailing_bytes=%zu\n",
           process, reason, count, removed, gbinder_reader_bytes_remaining(&reader));
    g_array_free(missions, TRUE);
    return 0; /* Original ignores process for selection and returns zero. */
}

static int on_mission_created(Agent *agent, GBinderRemoteRequest *request,
                              gboolean with_user)
{
    if (!agent->catalog || !agent->missions) return -1;
    GBinderReader reader; gint32 mission = -1, task = -1, user = 0;
    gbinder_remote_request_init_reader(request, &reader);
    /* Legacy2: mission/task; code32: mission/user/task, NOT mission/task/user. */
    const gboolean read_mission = gbinder_reader_read_int32(&reader, &mission);
    const gboolean read_user = read_mission &&
        (!with_user || gbinder_reader_read_int32(&reader, &user));
    const gboolean read_task = read_user && gbinder_reader_read_int32(&reader, &task);
    if (!read_task) {
        printf("AMS_MISSION_DECODE_FAILED with_user=%d mission=%d user=%d task=%d "
               "read_mission=%d read_user=%d read_task=%d remaining=%zu\n",
               with_user, mission, user, task, read_mission, read_user, read_task,
               gbinder_reader_bytes_remaining(&reader));
        return -1;
    }
    /* Original IAncoAmsAgentStub.onTransact reads the defined two/three ints
     * and immediately invokes onMissionCreated[InUser]. It does not enforce
     * dataAvail()==0. Real host6.0 code2 carries 20 additional bytes after the
     * two business ints; do not invent their meaning or reject a callback
     * that the original Stub accepts. Identity checks below still apply. */
    printf("AMS_MISSION_PARSED mission=%d user=%d task=%d trailing_bytes=%zu\n",
           mission, user, task, gbinder_reader_bytes_remaining(&reader));
    AncoLinuxLaunch record; gboolean finished;
    const gint32 catalog_user = anco_linux_catalog_user(agent->catalog);
    const int record_result = anco_linux_catalog_task_record(agent->catalog, task,
                                                            &record, &finished);
    if (user != catalog_user || record_result) {
        printf("AMS_MISSION_IDENTITY_FAILED mission=%d user=%d task=%d "
               "catalog_user=%d record_result=%d\n",
               mission, user, task, catalog_user, record_result);
        return -1;
    }
    /* Only actually created Linux identities can enter this user's map. Record
     * may be finished, as original Task history outlives the app process.
     * Same mission key is replaced as in the stock map; task IDs are separate.
     */
    g_hash_table_replace(agent->missions, GINT_TO_POINTER(mission), GINT_TO_POINTER(task));
    if (agent->pending_starts) g_hash_table_remove(agent->pending_starts, GINT_TO_POINTER(task));
    gpointer pending_removed = NULL;
    if (agent->pending_finishes && g_hash_table_lookup_extended(agent->pending_finishes,
        GINT_TO_POINTER(task), NULL, &pending_removed)) {
        g_hash_table_remove(agent->pending_finishes, GINT_TO_POINTER(task));
        ++agent->busy;
        notify_task_finished(agent, mission, task, FALSE); /* exact early-finish replay */
        if (GPOINTER_TO_INT(pending_removed)) notify_task_removed(agent, task);
        --agent->busy;
    }
    if (agent->mission_observer) {
        const gboolean associated = g_hash_table_lookup(agent->missions,
            GINT_TO_POINTER(mission)) == GINT_TO_POINTER(task);
        ++agent->busy;
        agent->mission_observer(agent->catalog, user, mission, task, associated,
            agent->mission_observer_data);
        --agent->busy;
    }
    printf("AMS_MISSION_ASSOCIATED mission=%d user=%d task=%d finished=%d\n",
           mission, user, task, finished);
    return 0;
}

static void catalog_child_exited(AncoLinuxCatalog *catalog,
                                  const AncoLinuxLaunch *launch, gint status, void *data)
{
    Agent *agent = data;
    g_assert(agent->catalog == catalog);
    ++agent->busy;
    finish_task_identity(agent, launch->task_id, status);
    /* Positive-session broker-origin starts suppress reverse notification.
     * Ordinary session0 requires explicit prepared launcher-runtime context.
     * Stock broker-origin starts suppress reverse activity-start and therefore
     * DO NOT create the guest-origin pending set. Do not invent one for every
     * start or retroactively replay an unassociated broker-origin exit.
     * Explicit guest-origin notification above now retains pending finish for
     * original mission association. Explicit runtime Task-removal notification
     * supplies REMOVED=true/reverse5; the actual Task/window removal runtime
     * remains unfinished. There is no automatic task removal on process exit.
     * Finish keeps association/recent identity; it is NOT notifyTaskRemoved.
     */
    if (agent->exit_observer)
        agent->exit_observer(catalog, launch, status, agent->exit_observer_data);
    --agent->busy;
}

int anco_ams_agent_on_child_exit(AncoAmsAgent *agent,
    AncoLinuxExitFunc observer, void *data)
{
    if (!agent || !agent->catalog || !agent->exit_attached) return -EINVAL;
    if (agent->busy) return -EBUSY;
    if (observer && agent->exit_observer) return -EALREADY;
    if (!observer && agent->exit_observer && agent->exit_observer_data != data)
        return -EPERM;
    agent->exit_observer = observer;
    agent->exit_observer_data = observer ? data : NULL;
    return 0;
}

int anco_ams_agent_on_configuration(AncoAmsAgent *agent,
    AncoAmsConfigurationFunc observer, void *data)
{
    if (!agent) return -EINVAL;
    if (agent->busy) return -EBUSY;
    if (observer && agent->configuration_observer) return -EALREADY;
    if (!observer && agent->configuration_observer && agent->configuration_observer_data != data)
        return -EPERM;
    agent->configuration_observer = observer;
    agent->configuration_observer_data = observer ? data : NULL;
    return 0;
}

int anco_ams_agent_on_task_selected(AncoAmsAgent *agent,
    AncoAmsSelectedFunc observer, void *data)
{
    if (!agent || !agent->catalog) return -EINVAL;
    if (agent->busy) return -EBUSY;
    if (observer && agent->selected_observer) return -EALREADY;
    if (!observer && agent->selected_observer && agent->selected_observer_data != data)
        return -EPERM;
    agent->selected_observer = observer;
    agent->selected_observer_data = observer ? data : NULL;
    return 0;
}

static int attach_catalog_events(Agent *agent)
{
    agent->missions = g_hash_table_new(g_direct_hash, g_direct_equal);
    const int result = anco_linux_catalog_on_exit(agent->catalog, catalog_child_exited, agent);
    if (result) { g_clear_pointer(&agent->missions, g_hash_table_unref); return result; }
    agent->exit_attached = TRUE;
    return 0;
}

/* Original WantParams type tag and payload; do not coerce launch values to
 * text. Integer-backed types retain their wire value until interpreted. */
typedef AncoAmsWantParam WantParam;

static void free_want_param(gpointer data)
{
    WantParam *param = data;
    if (param->type == 9) g_free(param->value.string);
    else if (param->type == 101 && param->value.want_params)
        g_hash_table_unref(param->value.want_params);
    g_free(param);
}

typedef AncoAmsWant StartWant;

static void clear_start_want(StartWant *want)
{
    g_free(want->action);
    g_free(want->uri);
    g_free(want->bundle);
    g_free(want->element_bundle);
    g_free(want->ability);
    g_free(want->device);
    if (want->entities) g_ptr_array_unref(want->entities);
    if (want->params) g_hash_table_unref(want->params);
}

static gboolean bounded_string(GBinderReader *reader, char **value, gsize limit)
{
    return gbinder_reader_read_nullable_string16(reader, value) &&
        (!*value || strlen(*value) <= limit);
}

enum { WANT_PARAMS_MAX_RECURSION_DEPTH = 100 };

/* Exact recursive subset of original WantParams::ReadFromParcel. A nested
 * VALUE_TYPE_WANTPARAMS payload starts directly with its entry count; only the
 * outer WriteParcelable contributes the preceding version/presence marker. */
static gboolean read_want_param_entries(GBinderReader *reader, guint depth,
                                        GHashTable **out)
{
    gint32 count = 0;
    if (!out || !gbinder_reader_read_int32(reader, &count) ||
        count < 0 || count > 64)
        return FALSE;
    GHashTable *params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                               free_want_param);
    for (int i = 0; i < count; ++i) {
        char *key = NULL;
        gint32 type = 0;
        if (depth >= WANT_PARAMS_MAX_RECURSION_DEPTH ||
            !bounded_string(reader, &key, 256) || !key || !*key ||
            !gbinder_reader_read_int32(reader, &type)) {
            g_free(key);
            g_hash_table_unref(params);
            return FALSE;
        }
        WantParam *param = g_new0(WantParam, 1);
        param->type = type;
        if (type == 9) {
            if (!bounded_string(reader, &param->value.string, 4096)) {
                g_free(key);
                free_want_param(param);
                g_hash_table_unref(params);
                return FALSE;
            }
        } else if (type == 1 || type == 3 || type == 4 || type == 5) {
            if (!gbinder_reader_read_int32(reader, &param->value.integer)) {
                g_free(key);
                free_want_param(param);
                g_hash_table_unref(params);
                return FALSE;
            }
        } else if (type == 101) {
            if (!read_want_param_entries(reader, depth + 1,
                                         &param->value.want_params)) {
                g_free(key);
                free_want_param(param);
                g_hash_table_unref(params);
                return FALSE;
            }
        } else {
            printf("AMS_WANT_UNSUPPORTED_PARAM type=%d key=%s\n", type, key);
            g_free(key);
            free_want_param(param);
            g_hash_table_unref(params);
            return FALSE;
        }
        g_hash_table_replace(params, key, param);
    }
    *out = params;
    return TRUE;
}

static gboolean read_want_params(GBinderReader *reader, StartWant *want)
{
    gint32 version = 0;
    return gbinder_reader_read_int32(reader, &version) && version == 1 &&
        read_want_param_entries(reader, 1, &want->params);
}

static gboolean read_start_want(GBinderRemoteRequest *request, StartWant *want)
{
    GBinderReader reader;
    gint32 present = 0, has_uri = 0, has_entities = 0;
    gint32 has_element = 0, has_params = 0, version = 0;
    const char *stage = "session/presence";
    gbinder_remote_request_init_reader(request, &reader);
    if (!gbinder_reader_read_int32(&reader, &want->session_id) ||
        want->session_id < 0 ||
        !gbinder_reader_read_int32(&reader, &present) || present != 1)
        goto invalid;
    stage = "action/uri";
    if (!bounded_string(&reader, &want->action, 1024) ||
        !gbinder_reader_read_int32(&reader, &has_uri) ||
        (has_uri != 1 && has_uri != -1) ||
        (has_uri == 1 && !bounded_string(&reader, &want->uri, 4096)))
        goto invalid;
    stage = "entities";
    if (!gbinder_reader_read_int32(&reader, &has_entities) ||
        (has_entities != 1 && has_entities != -1))
        goto invalid;
    if (has_entities == 1) {
        gint32 count = 0;
        if (!gbinder_reader_read_int32(&reader, &count) || count < 0 || count > 64)
            goto invalid;
        want->entities = g_ptr_array_new_with_free_func(g_free);
        for (int i = 0; i < count; ++i) {
            char *entity = NULL;
            const gboolean valid = bounded_string(&reader, &entity, 1024);
            if (!valid) {
                g_free(entity);
                goto invalid;
            }
            g_ptr_array_add(want->entities, entity);
        }
    }
    stage = "flags/element";
    if (!gbinder_reader_read_int32(&reader, &want->flags) ||
        !gbinder_reader_read_int32(&reader, &has_element) ||
        (has_element != 1 && has_element != -1))
        goto invalid;
    if (has_element == 1 &&
        (!gbinder_reader_read_int32(&reader, &version) || version != 1 ||
         !bounded_string(&reader, &want->element_bundle, 256) ||
         !bounded_string(&reader, &want->ability, 256) ||
         !bounded_string(&reader, &want->device, 256)))
        goto invalid;
    stage = "params/bundle";
    if (!gbinder_reader_read_int32(&reader, &has_params) ||
        (has_params != 1 && has_params != -1) ||
        (has_params == 1 && !read_want_params(&reader, want)) ||
        !bounded_string(&reader, &want->bundle, 256) ||
        !gbinder_reader_at_end(&reader))
        goto invalid;
    if (!want->bundle && !want->element_bundle) goto invalid;
    if (want->bundle && want->element_bundle &&
        strcmp(want->bundle, want->element_bundle)) goto invalid;
    return TRUE;
invalid:
    printf("AMS_WANT_INVALID stage=%s consumed=%zu remaining=%zu\n", stage,
           gbinder_reader_bytes_read(&reader),
           gbinder_reader_bytes_remaining(&reader));
    return FALSE;
}

static gint32 want_int_extra(const StartWant *want, const char *key, gint32 fallback)
{
    const WantParam *param = want->params ? g_hash_table_lookup(want->params, key) : NULL;
    /* WantParams type5 becomes java.lang.Integer. Boolean/Byte/Short are not
     * Integer: Android Intent.getIntExtra returns its default for those types.
     * These three ASCII option keys are not in the stock EXCLUDE_KEY list.
     */
    return param && param->type == 5 ? param->value.integer : fallback;
}

static int prepare_task_hint(Agent *agent, StartWant *want)
{
    const gint32 current = anco_linux_catalog_user(agent->catalog);
    const gint32 incoming = want_int_extra(want, "ancoLaunchUserId", 0);
    const gint32 user = !anco_linux_catalog_privacy(agent->catalog) ? 0 :
        (incoming == 0 || incoming == 124 ? incoming : current);
    want->launch_guest_user = user;
    want->launch_task_hint = want_int_extra(want, "launchTaskId", -1);
    want->launch_window_mode_present = want->params &&
        g_hash_table_contains(want->params, "ancoLaunchWindowMode");
    if (want->launch_window_mode_present) {
        want->launch_window_mode = want_int_extra(want, "ancoLaunchWindowMode", 1);
        /* Original option is consumed by the real selected Task, not passed
         * to an executable. Keep raw Want unchanged for the borrowed callback.
         * This finite ordinary-root bridge supports only fullscreen/freeform;
         * original split/middle/PC/pinned policies are not synthesized. */
        if (!agent->selected_observer ||
            (want->launch_window_mode != 1 && want->launch_window_mode != 102))
            return -ENOTSUP;
    }
    /* One catalog owns one real guest-user session. Never use its mission
     * map/processes for a request which normalizes to a different session.
     */
    if (user != current) return -EXDEV;
    if (want->launch_task_hint == -1) {
        const gint32 mission = want_int_extra(want, "oh_persistentId", -1);
        gint32 mapped;
        if (!anco_ams_agent_task_for_mission(agent, user, mission, &mapped))
            want->launch_task_hint = mapped;
    }
    /* Original explicit values other than -1 remain explicit, even if invalid.
     * Do not replace unknown tasks with a package-name singleton or claim this
     * hint is live. Activity/Intent selection and actual client reuse remain
     * the common runtime's responsibility after receiving these options.
     */
    return 0;
}

static int select_launcher(AncoLinuxCatalog *catalog, const AncoAmsWant *want,
                            gint32 *task, gboolean *created)
{
    if (!catalog || !want || !task || !created || want->session_id < 0) return -EINVAL;
    if (want->launch_guest_user != anco_linux_catalog_user(catalog)) return -EXDEV;
    if (want->launch_window_mode_present && want->launch_window_mode != 1 &&
        want->launch_window_mode != 102) return -ENOTSUP;
    const char *bundle = want->bundle ? want->bundle : want->element_bundle;
    if (!bundle || !want->ability || !*want->ability) return -ENOTSUP;
    /* Stock IntentWantUtils also accepts an explicit ComponentName with an
     * empty action and a null/empty category list: it simply skips setAction
     * and addCategory before Context.startActivityAsUser. This is the normal
     * host icon launch observed on this device. Preserve the older explicit
     * MAIN/LAUNCHER form as the other finite ordinary-launcher shape.
     */
    const gboolean empty_explicit = (!want->action || !*want->action) &&
        (!want->entities || !want->entities->len);
    if (!empty_explicit) {
        if (!want->action || (strcmp(want->action, "ohos.want.action.home") &&
            strcmp(want->action, "action.system.home") &&
            strcmp(want->action, "android.intent.action.MAIN"))) return -ENOTSUP;
        if (!want->entities || !want->entities->len) return -ENOTSUP;
        for (guint i = 0; i < want->entities->len; ++i) {
            const char *entity = g_ptr_array_index(want->entities, i);
            if (!entity || (strcmp(entity, "entity.system.home") &&
                           strcmp(entity, "android.intent.category.LAUNCHER"))) return -ENOTSUP;
        }
    }
    if ((want->uri && *want->uri) || (want->device && *want->device)) return -ENOTSUP;
    const WantParam *mime = want->params ? g_hash_table_lookup(want->params, "mime-type") : NULL;
    if (mime && mime->type == 9 && mime->value.string && *mime->value.string) return -ENOTSUP;
    /* Original AMS adds PushAncoWantIntentConverter after the six constant
     * converters. Its isFromAdroi gate requires BOTH of these exact String
     * parameters before parsing notification data, which may replace the
     * whole Intent. That resolver is not this ordinary launcher branch.
     * Missing/wrong-type/nonmatching caller fields do not select it in stock.
     */
    const WantParam *caller = want->params ? g_hash_table_lookup(want->params,
        "ohos.aafwk.param.callerBundleName") : NULL;
    const WantParam *identifier = want->params ? g_hash_table_lookup(want->params,
        "ohos.aafwk.param.callerAppIdentifier") : NULL;
    if (caller && caller->type == 9 && caller->value.string &&
        !strcmp(caller->value.string, "com.droi.tong") &&
        identifier && identifier->type == 9 && identifier->value.string &&
        !strcmp(identifier->value.string, "5765880207855132255")) return -ENOTSUP;
    const char *unwired[] = {"ancoPendingRequest", "activityTokenCode",
                             "ability.params.stream", "ability.verify.uri"};
    for (guint i = 0; want->params && i < G_N_ELEMENTS(unwired); ++i)
        if (g_hash_table_contains(want->params, unwired[i])) return -ENOTSUP;
    /* Full original OH->Android flag table has seven entries. WantFlags are
     * NOT Android flags: OH bit4 means FORWARD_RESULT0x02000000, not bit4.
     * Unknown bits contribute nothing; ordinary broker always adds NEW_TASK.
     * Other-start allowlist is only RESET_TASK_IF_NEEDED0x00200000.
     */
    const guint32 flags = (guint32)want->flags;
    const guint32 projected = (flags & 0x100080c3u) |
        ((flags & 4u) ? 0x02000000u : 0u) | 0x10000000u |
        ((guint32)want_int_extra(want, "KEY_START_ABILITY_FLAG", 0) & 0x00200000u);
    if (projected != 0x10000000u) return -ENOTSUP;
    AncoLinuxLaunch actual;
    const int result = anco_linux_catalog_select_launcher(catalog, bundle, want->ability,
                                                         want->launch_task_hint, &actual, created);
    if (!result) *task = actual.task_id;
    return result;
}

int anco_ams_start_launcher(AncoLinuxCatalog *catalog, const AncoAmsWant *want,
                            gint32 *task, void *data)
{
    (void)data;
    if (!want || want->session_id <= 0) return -EINVAL;
    gboolean created;
    return select_launcher(catalog, want, task, &created);
}

int anco_ams_agent_notify_runtime_start(AncoAmsAgent *agent,
    gint32 activity, gint32 source_activity, gint32 session,
    const AncoAmsCaller *caller, gboolean behind, AncoAmsActivityStartResult *out)
{
    if (!agent || !agent->catalog || !caller || !out) return -EINVAL;
    if (!agent->launcher_configured) return -ENOTSUP;
    AncoLinuxLaunch actual, root;
    int local = anco_linux_catalog_activity(agent->catalog, activity, &actual);
    if (local) return local;
    local = anco_linux_catalog_task(agent->catalog, actual.task_id, &root);
    if (local) return local;
    if (actual.activity_id != root.activity_id) return -ENOTSUP;
    if (activity_start_suppressed(agent, actual.task_id, session)) {
        *out = (AncoAmsActivityStartResult){.suppressed = TRUE,
            .transport = -ENOTCONN, .result = 0};
        return 0;
    }
    ++agent->busy;
    GArray *background = g_array_new(FALSE, FALSE, sizeof(gint32));
    const AncoAmsLauncherRuntime *runtime = &agent->launcher_runtime;
    if (runtime->multiwindow_enabled && strcmp(runtime->broker_version, "1.0.0") >= 0)
        local = runtime->background_tasks(agent->catalog, &actual, background, runtime->user_data);
    if (!local) {
        const AncoAmsActivityStart start = {.activity = activity,
            .source_activity = source_activity, .broker_session = session,
            .caller = caller, .launch_task_behind = behind,
            .broker_version = runtime->broker_version,
            .multiwindow_enabled = runtime->multiwindow_enabled,
            .background_tasks = (const gint32 *)background->data,
            .background_task_count = background->len};
        local = anco_ams_agent_notify_activity_start(agent, &start, out);
    }
    g_array_unref(background);
    --agent->busy;
    return local;
}

static int start_zero_launcher(Agent *agent, const StartWant *want, gint32 *selected)
{
    if (!agent->launcher_configured || agent->start != anco_ams_start_launcher)
        return -ENOTSUP;
    gboolean created;
    const int started = select_launcher(agent->catalog, want, selected, &created);
    if (started || !created) return started;
    AncoLinuxLaunch actual;
    const int owned = anco_linux_catalog_task(agent->catalog, *selected, &actual);
    if (owned) return owned;
    /* A Linux desktop entry starts the process only.  Its real XDG/X11
     * windows later create their own AnCo carriers through WMS.  Publishing
     * an Android-style reverse Activity here creates a window with no Linux
     * surface and lets launcher stack policy replace unrelated windows. */
    printf("AMS_ZERO_SELECTED task=%d new=1 reverse_activity_start=0 window_created_by_real_surface=1\n",
        *selected);
    return 0; /* actual selected Task, NOT successful host window/notification */
}

static int on_ability_start(Agent *agent, GBinderRemoteRequest *request, gint32 *task_id)
{
    StartWant want = {0};
    int result = -1;
    *task_id = -1;
    const gboolean valid = read_start_want(request, &want);
    if (valid) {
        printf("AMS_WANT_PARSED session=%d bundle=%s ability=%s action=%s params=%u\n",
               want.session_id,
               want.bundle ? want.bundle : want.element_bundle,
               want.ability ? want.ability : "",
               want.action ? want.action : "",
               want.params ? g_hash_table_size(want.params) : 0);
        /* Original AncoAmsAgent returns the selected guest task ID first;
         * host AMS then selects ShellAssistant from that successful reply.
         * The Linux runtime must supply a real new or reused task, without
         * waiting for a host Ability or first frame. Until wired, fail below. */
        if (agent->catalog && agent->start && want.session_id == 0 &&
            (!agent->launcher_configured || agent->start != anco_ams_start_launcher)) {
            /* Stock suppression is >0, not >=0. Do not treat a zero-session
             * request as supported without guest-origin activity-start and
             * its real pending-association/replay path. No spawn or success.
             */
            puts("AMS_START_ZERO_SESSION_REQUIRES_REVERSE_ACTIVATION");
        } else if (agent->catalog && agent->start &&
                   prepare_task_hint(agent, &want)) {
            printf("AMS_START_OPTIONS_REJECTED user=%d current=%d mode_present=%d mode=%d\n",
                   want.launch_guest_user, anco_linux_catalog_user(agent->catalog),
                   want.launch_window_mode_present, want.launch_window_mode);
        } else if (agent->catalog && agent->start) {
            gint32 selected = -1;
            const int started = want.session_id == 0 ? start_zero_launcher(agent, &want, &selected) :
                agent->start(agent->catalog, &want, &selected, agent->start_data);
            AncoLinuxLaunch actual;
            const int owned = !started && selected > 0 ?
                anco_linux_catalog_task(agent->catalog, selected, &actual) : -EINVAL;
            if (!started && !owned) {
                *task_id = actual.task_id;
                result = 0;
                if (agent->selected_observer) {
                    ++agent->busy;
                    agent->selected_observer(agent->catalog, &want, &actual,
                        agent->selected_observer_data);
                    --agent->busy;
                }
                printf("AMS_START_SELECTED_TASK session=%d task=%d activity=%d pid=%ld\n",
                       want.session_id, actual.task_id, actual.activity_id,
                       (long)actual.process.pid);
            } else {
                printf("AMS_START_REJECTED started=%d ownership=%d selected=%d\n",
                       started, owned, selected);
            }
        } else puts("AMS_START_NO_TASK_RUNTIME");
    }
    clear_start_want(&want);
    return result;
}

/* The original ConfigurationParams parcel has an outer presence word, then
 * display ID, entry count, two String8 arrays (each with its own count),
 * user ID and locale. Keep the received state for the Linux task launcher;
 * malformed or unsupported parcels must not partially update it. */
static int update_configuration(Agent *agent, GBinderRemoteRequest *request)
{
    GBinderReader reader;
    gbinder_remote_request_init_reader(request, &reader);
    gint32 present = 0, display = 0, count = 0, keys_count = 0;
    gint32 values_count = 0, user = 0;
    const char *stage = "presence";
    GHashTable *next = NULL;
    const char **keys = NULL;
    if (!gbinder_reader_read_int32(&reader, &present) || present != 1)
        goto invalid;
    stage = "display/count";
    if (!gbinder_reader_read_int32(&reader, &display) ||
        !gbinder_reader_read_int32(&reader, &count) ||
        display < 0 || count < 0 || count >= 1000)
        goto invalid;
    stage = "keys";
    if (!gbinder_reader_read_int32(&reader, &keys_count) ||
        keys_count != count)
        goto invalid;
    keys = g_new0(const char *, count ? count : 1);
    for (int i = 0; i < count; ++i) {
        if (!gbinder_reader_read_nullable_string8(&reader, &keys[i], NULL) ||
            !keys[i] || strlen(keys[i]) > 256)
            goto invalid;
    }
    stage = "values";
    if (!gbinder_reader_read_int32(&reader, &values_count) ||
        values_count != count)
        goto invalid;
    next = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%d#", display);
    for (int i = 0; i < count; ++i) {
        const char *value = NULL;
        if (!gbinder_reader_read_nullable_string8(&reader, &value, NULL) ||
            (value && strlen(value) > 2048))
            goto invalid;
        if (value && g_str_has_prefix(keys[i], prefix) && *value)
            g_hash_table_replace(next, g_strdup(keys[i] + strlen(prefix)),
                                 g_strdup(value));
    }
    stage = "user/locale";
    const char *locale = NULL;
    if (!gbinder_reader_read_int32(&reader, &user) ||
        (user < 0 && user != -10000))
        goto invalid;
    stage = "locale";
    if (!gbinder_reader_read_nullable_string8(&reader, &locale, NULL) ||
        (locale && strlen(locale) > 128) || !gbinder_reader_at_end(&reader))
        goto invalid;
    if (locale && *locale)
        g_hash_table_replace(next, g_strdup("ohos.global.locale"),
                             g_strdup(locale));
    if (agent->configuration) g_hash_table_unref(agent->configuration);
    agent->configuration = next;
    g_free(agent->locale);
    agent->locale = g_strdup(locale);
    agent->display_id = display;
    agent->user_id = user;
    printf("AMS_CONFIGURATION_UPDATED display=%d user=%d entries=%u locale=%s\n",
           display, user, g_hash_table_size(next), locale ? locale : "");
    if (agent->configuration_observer) {
        ++agent->busy;
        agent->configuration_observer(display, user, next, agent->configuration_observer_data);
        --agent->busy;
    }
    g_free(keys);
    return 0;
invalid:
    printf("AMS_CONFIGURATION_INVALID stage=%s consumed=%zu remaining=%zu\n",
           stage, gbinder_reader_bytes_read(&reader),
           gbinder_reader_bytes_remaining(&reader));
    if (next) g_hash_table_unref(next);
    g_free(keys);
    return -1;
}

static GBinderLocalRequest *samgr_request(GBinderIpc *ipc)
{
    GBinderLocalRequest *req = gbinder_local_request_new(
        gbinder_ipc_io(ipc), gbinder_ipc_protocol(ipc), NULL);
    if (req) {
        gbinder_local_request_append_int32(req, 0x100);
        gbinder_local_request_append_int32(req, 0);
        gbinder_local_request_append_string16(req, "ohos.samgr.accessToken");
    }
    return req;
}

static GBinderRemoteObject *lookup_checked(GBinderIpc *ipc, int id, int *error)
{
    if (error) *error = -ENOMEM;
    GBinderLocalRequest *req = samgr_request(ipc);
    if (!req) return NULL;
    gbinder_local_request_append_int32(req, id);
    int transport = -1;
    GBinderRemoteReply *reply = gbinder_ipc_transact_sync_reply_main(
        ipc, 0, 2, req, &transport);
    GBinderRemoteObject *obj = reply && !transport ?
        gbinder_remote_reply_read_object(reply) : NULL;
    /* Current original CheckSystemAbilityInner returns7 for an absent valid
     * SA (also for invalid IDs),1 for permission failure. Only a valid-ID7
     * query establishes absence; transport0 without an object is malformed.
     * This preflight does not make the subsequent Add atomic or exclusive. */
    if (error) *error = transport == 7 && id >= 0 && id < 0x1000000 ? 0 :
        transport ? transport : obj ? 0 : -EBADMSG;
    printf("SA_LOOKUP id=%d transport=%d present=%d\n", id, transport,
           obj != NULL);
    if (reply) gbinder_remote_reply_unref(reply);
    gbinder_local_request_unref(req);
    return obj;
}

static GBinderRemoteObject *lookup(GBinderIpc *ipc, int id)
{
    return lookup_checked(ipc, id, NULL);
}

static void append_sa_object_properties(GBinderLocalRequest *req, GBinderLocalObject *object)
{
    /* Original ServiceManager.addSysAbility(int,IBinder) -> libbinder
     * BpSystemAbilityManager::AddSystemAbility: false, int32 VALUE8, "", "".
     * The 8 is not an eight-bit field and not an inferred project priority. */
    gbinder_local_request_append_local_object(req, object);
    gbinder_local_request_append_int32(req, 0);
    gbinder_local_request_append_int32(req, 8);
    gbinder_local_request_append_string16(req, "");
    gbinder_local_request_append_string16(req, "");
}

static int change_sa(Agent *agent, guint code, GBinderLocalObject *object)
{
    GBinderLocalRequest *req = samgr_request(agent->ipc);
    if (!req) return -1;
    gbinder_local_request_append_int32(req, agent->sa_id);
    if (code == 3) {
        append_sa_object_properties(req, object);
    }
    int transport = -1;
    GBinderRemoteReply *reply = gbinder_ipc_transact_sync_reply_main(
        agent->ipc, 0, code, req, &transport);
    gint32 result = -1;
    gboolean decoded = reply && gbinder_remote_reply_read_int32(reply, &result);
    printf("SA_CHANGE code=%u id=%d transport=%d decoded=%d result=%d\n",
           code, agent->sa_id, transport, decoded, result);
    if (reply) gbinder_remote_reply_unref(reply);
    gbinder_local_request_unref(req);
    return !transport && decoded ? result : -1;
}

static void ams_died(GBinderRemoteObject *object, void *user_data)
{
    Agent *agent = user_data;
    if (object == agent->ams) {
        puts("AMS_BROKER_DIED");
        fflush(stdout);
        agent->ams_death = 0;
        gbinder_remote_object_unref(agent->ams);
        agent->ams = NULL;
    }
}

static void clear_ams(Agent *agent)
{
    if (agent->ams) {
        if (agent->ams_death)
            gbinder_remote_object_remove_handler(agent->ams, agent->ams_death);
        gbinder_remote_object_unref(agent->ams);
    }
    agent->ams = NULL;
    agent->ams_death = 0;
}

static int request_user_unlock(Agent *agent, gint32 incoming)
{
    if (!agent->catalog || !agent->unlock) return -ENOTSUP;
    const gint32 current = anco_linux_catalog_user(agent->catalog);
    /* Original UserUtil: normal mode chooses0; private mode preserves0/124,
     * otherwise the current guest user. This endpoint owns one current session
     * only: it cannot fabricate an unowned second user's lifecycle. */
    const gint32 user = !anco_linux_catalog_privacy(agent->catalog) ? 0 :
        incoming == 0 || incoming == 124 ? incoming : current;
    if (user != current) return -ENOENT;
    if (!agent->unlock_requested) {
        /* Original manager records before initiating user lifecycle, and clears
         * unlock retry. This means requested, never that initialization is done. */
        agent->unlock_requested = TRUE;
        agent->unlock_retry = FALSE;
        agent->unlock(agent->catalog, user, agent->start_data);
        printf("AMS_USER_UNLOCK_REQUESTED guest=%d\n", user);
    }
    return 0;
}
static int on_unlock_user(Agent *agent, GBinderRemoteRequest *request)
{
    GBinderReader reader; gint32 user;
    gbinder_remote_request_init_reader(request, &reader);
    if (!gbinder_reader_read_int32(&reader, &user) || !gbinder_reader_at_end(&reader)) return -1;
    return request_user_unlock(agent, user) ? -1 : 0;
}
static int decode_account_verified(GBinderRemoteReply *reply, int transport)
{
    if (transport || !reply) return -1;
    GBinderReader reader; gint32 verified;
    gbinder_remote_reply_init_reader(reply, &reader);
    if (!gbinder_reader_read_int32(&reader, &verified) || !gbinder_reader_at_end(&reader) ||
        (verified != 0 && verified != 1)) return -1;
    return verified;
}
static int query_default_account(Agent *agent)
{
    int result = -1, transport = -1;
    if (agent->ams) {
        GBinderClient *client = gbinder_client_new(agent->ams, "OHOS.Anco.Service.Ams");
        GBinderLocalRequest *request = client ? gbinder_client_new_request(client) : NULL;
        if (request) {
            /* Original ordinary guest boot/retry query, not a Linux UID. */
            gbinder_local_request_append_int32(request, 100);
            GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(client, 10,
                request, &transport);
            result = decode_account_verified(reply, transport);
            if (reply) gbinder_remote_reply_unref(reply);
            gbinder_local_request_unref(request);
        }
        if (client) gbinder_client_unref(client);
    }
    agent->unlock_retry = result != 1;
    printf("AMS_ACCOUNT_QUERY transport=%d verified=%d pending=%d\n",
           transport, result, agent->unlock_retry);
    return result;
}
int anco_ams_agent_boot_default_user(AncoAmsAgent *agent)
{
    if (!agent || !agent->catalog || !agent->unlock ||
        anco_linux_catalog_user(agent->catalog) != 0 ||
        anco_linux_catalog_privacy(agent->catalog)) return -ENOTSUP;
    const int verified = query_default_account(agent);
    if (verified == 1) return request_user_unlock(agent, 0);
    return verified == 0 ? -EAGAIN : -ENOTCONN;
}

static int connect_ams(Agent *agent)
{
    GBinderRemoteObject *next = lookup(agent->ipc, AMS_BROKER_SA);
    if (!next) {
        clear_ams(agent);
        return -1;
    }
    clear_ams(agent);
    agent->ams = next;
    agent->ams_death = gbinder_remote_object_add_death_handler(next,
        ams_died, agent);
    printf("AMS_BROKER_CONNECTED death_handler=%lu\n", agent->ams_death);
    fflush(stdout);
    if (!agent->ams_death) clear_ams(agent);
    if (agent->ams_death && agent->catalog && agent->unlock_retry) {
        /* Stock code14 retries only a PREVIOUS pending query, no timer and no
         * eager query merely because the broker connected. Current single-user
         * ordinary runtime cannot perform private/cross-user boot recovery. */
        if (anco_linux_catalog_user(agent->catalog) == 0 &&
            !anco_linux_catalog_privacy(agent->catalog)) {
            agent->unlock_retry = FALSE;
            if (query_default_account(agent) == 1) request_user_unlock(agent, 0);
        }
    }
    return agent->ams_death ? 0 : -1;
}

static gboolean is_project_broker(pid_t pid)
{
    if (pid <= 0) return FALSE;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
    gchar *command = NULL;
    gsize length = 0;
    if (!g_file_get_contents(path, &command, &length, NULL)) return FALSE;
    gboolean binary = FALSE;
    gboolean config = FALSE;
    for (gsize offset = 0; offset < length;) {
        const char *arg = command + offset;
        const gsize remaining = length - offset;
        const gsize used = strnlen(arg, remaining);
        if (used == remaining) break;
        if (strstr(arg, "anco_service_broker")) binary = TRUE;
        if (!strcmp(arg, PRIVATE_BROKER_CONFIG)) config = TRUE;
        offset += used + 1;
    }
    g_free(command);
    return binary && config;
}

/* This device's Binder driver reports sender_pid=0 for host one-way calls.
 * Restrict the UID 0 development mapping to the short-lived private broker
 * session whose exact command line is under this project's /data tree. */
static gboolean private_broker_running(void)
{
    GDir *proc = g_dir_open("/proc", 0, NULL);
    if (!proc) return FALSE;
    const gchar *name;
    guint count = 0;
    while ((name = g_dir_read_name(proc)) != NULL) {
        gchar *end = NULL;
        const long pid = strtol(name, &end, 10);
        if (end != name && !*end && pid > 0 && is_project_broker((pid_t)pid))
            count++;
    }
    g_dir_close(proc);
    return count == 1;
}

static GBinderLocalReply *on_agent_transaction(GBinderLocalObject *self,
    GBinderRemoteRequest *request, guint code, guint flags, int *status,
    void *user_data)
{
    Agent *agent = user_data;
    const char *iface = gbinder_remote_request_interface(request);
    const uid_t uid = gbinder_remote_request_sender_euid(request);
    const pid_t pid = gbinder_remote_request_sender_pid(request);
    const gboolean authorized = uid == BROKER_UID ||
        (!agent->catalog && uid == 0 && (agent->sa_id == PRIVATE_SA ||
            is_project_broker(pid) ||
            (pid == 0 && private_broker_running())));
    printf("AMS_AGENT_CALL code=%u uid=%lu pid=%ld authorized=%d flags=%u iface=%s\n",
           code, (unsigned long)uid, (long)pid, authorized, flags,
           iface ? iface : "(none)");
    fflush(stdout);
    if (!authorized || !iface || strcmp(iface, AGENT_IFACE)) {
        *status = -1;
        return NULL;
    }
    /* Original code 14: connect the reverse host broker and track its death.
     * Its Proxy sets FLAG_ONEWAY, so the reply is informational in that case. */
    gint32 task_id = -1;
    ++agent->busy;
    const int result = code == 14 ? connect_ams(agent) :
        code == 13 ? update_configuration(agent, request) :
        code == 11 ? on_unlock_user(agent, request) :
        code == 6 ? on_shell_process_removed(agent, request) :
        code == 5 ? on_mission_removed(agent, request) :
        code == 2 ? on_mission_created(agent, request, FALSE) :
        code == 32 ? on_mission_created(agent, request, TRUE) :
        code == 1 ? on_ability_start(agent, request, &task_id) : -1;
    if (code != 14 && code != 13 && code != 11 && code != 6 && code != 5 &&
        code != 1 && code != 2 && code != 32)
        printf("AMS_AGENT_UNIMPLEMENTED code=%u result=%d\n", code, result);
    GBinderLocalReply *reply = gbinder_local_object_new_reply(self);
    if (reply) {
        gbinder_local_reply_append_int32(reply, (guint32)result);
        if (code == 1) gbinder_local_reply_append_int32(reply, (guint32)task_id);
    }
    *status = 0;
    --agent->busy;
    fflush(stdout);
    return reply;
}

AncoAmsAgent *anco_ams_agent_new(GBinderIpc *ipc, AncoLinuxCatalog *catalog,
                                AncoAmsStartFunc start, AncoAmsUnlockFunc unlock, void *data)
{
    if (!ipc || !catalog || !start || !unlock) return NULL;
    Agent *agent = g_new0(Agent, 1);
    agent->sa_id = PRODUCTION_SA;
    agent->ipc = gbinder_ipc_ref(ipc);
    agent->catalog = catalog; agent->start = start; agent->start_data = data;
    agent->unlock = unlock;
    const char *ifaces[] = { AGENT_IFACE, NULL };
    agent->object = gbinder_local_object_new(ipc, ifaces, on_agent_transaction, agent);
    if (!agent->object) { gbinder_ipc_unref(agent->ipc); g_free(agent); return NULL; }
    if (attach_catalog_events(agent)) {
        gbinder_local_object_drop(agent->object);
        gbinder_ipc_unref(agent->ipc); g_free(agent); return NULL;
    }
    /* Original AmsBrokerProxy.<init> creates its reverse proxy immediately.
     * Code14 is the later broker-added notification, not the first connection
     * prerequisite. Failure leaves the ordinary account retry pending when
     * boot_default_user runs; never manufacture an unlock response. */
    (void)connect_ams(agent);
    return agent;
}
int anco_ams_agent_register(AncoAmsAgent *agent)
{
    if (!agent) return -EINVAL;
    if (agent->published) return -EALREADY;
    int query_error = 0;
    GBinderRemoteObject *existing = lookup_checked(agent->ipc, agent->sa_id, &query_error);
    if (existing) { gbinder_remote_object_unref(existing); return -EADDRINUSE; }
    if (query_error) return query_error;
    const int result = change_sa(agent, 3, agent->object);
    if (!result) agent->published = TRUE;
    return result;
}
int anco_ams_agent_free(AncoAmsAgent *agent)
{
    if (!agent) return 0;
    if (agent->busy || anco_linux_catalog_children(agent->catalog)) return -EBUSY;
    if (agent->published) {
        const int result = change_sa(agent, 4, NULL);
        if (result) return result;
        agent->published = FALSE;
    }
    if (agent->exit_attached) {
        const int result = anco_linux_catalog_on_exit(agent->catalog, NULL, agent);
        if (result) return result;
    }
    /* drop clears transaction handler/user_data even if remote references
     * retain the local object, so they cannot call freed Agent memory. */
    gbinder_local_object_drop(agent->object);
    clear_ams(agent);
    if (agent->missions) g_hash_table_unref(agent->missions);
    if (agent->pending_starts) g_hash_table_unref(agent->pending_starts);
    if (agent->pending_finishes) g_hash_table_unref(agent->pending_finishes);
    if (agent->configuration) g_hash_table_unref(agent->configuration);
    /* Launcher context belongs to this Agent, not to a display configuration
     * snapshot. Configuration callbacks must leave these owned copies alive. */
    g_free((gpointer)agent->launcher_runtime.broker_version);
    g_free((gpointer)agent->launcher_runtime.context_caller.package);
    g_free(agent->locale);
    gbinder_ipc_unref(agent->ipc);
    g_free(agent);
    return 0;
}

#ifndef ANCO_AMS_AGENT_EMBEDDED
static Agent *signal_agent;

static void stop_agent(int signo)
{
    (void)signo;
    if (signal_agent && signal_agent->loop)
        g_main_loop_quit(signal_agent->loop);
}

static gboolean end_private_probe(gpointer user_data)
{
    Agent *agent = user_data;
    g_main_loop_quit(agent->loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    const gboolean production = argc == 2 &&
        !strcmp(argv[1], "--production-66866");
    if (argc != 1 && !production) {
        fprintf(stderr, "usage: %s [--production-66866]\n", argv[0]);
        return 2;
    }
    Agent agent = {0};
    agent.sa_id = production ? PRODUCTION_SA : PRIVATE_SA;
    agent.ipc = gbinder_ipc_new("/dev/binder", "aidl2");
    if (!agent.ipc) return 3;
    int query_error = 0;
    GBinderRemoteObject *existing = lookup_checked(agent.ipc, agent.sa_id, &query_error);
    if (existing) {
        gbinder_remote_object_unref(existing);
        fprintf(stderr, "REFUSE_EXISTING_SA id=%d\n", agent.sa_id);
        gbinder_ipc_unref(agent.ipc);
        return 4;
    }
    if (query_error) {
        fprintf(stderr, "REFUSE_UNRESOLVED_SA id=%d error=%d\n", agent.sa_id, query_error);
        gbinder_ipc_unref(agent.ipc);
        return 4;
    }
    const char *ifaces[] = { AGENT_IFACE, NULL };
    GBinderLocalObject *object = gbinder_local_object_new(agent.ipc, ifaces,
        on_agent_transaction, &agent);
    if (!object) { gbinder_ipc_unref(agent.ipc); return 5; }
    if (change_sa(&agent, 3, object)) {
        gbinder_local_object_unref(object);
        gbinder_ipc_unref(agent.ipc);
        return 6;
    }
    agent.loop = g_main_loop_new(NULL, FALSE);
    signal_agent = &agent;
    signal(SIGINT, stop_agent);
    signal(SIGTERM, stop_agent);
    if (!production) g_timeout_add_seconds(12, end_private_probe, &agent);
    puts("AMS_AGENT_READY");
    fflush(stdout);
    g_main_loop_run(agent.loop);
    signal_agent = NULL;
    const int removed = change_sa(&agent, 4, NULL);
    clear_ams(&agent);
    if (agent.configuration) g_hash_table_unref(agent.configuration);
    g_free(agent.locale);
    g_main_loop_unref(agent.loop);
    gbinder_local_object_unref(object);
    gbinder_ipc_unref(agent.ipc);
    return removed ? 7 : 0;
}
#endif
