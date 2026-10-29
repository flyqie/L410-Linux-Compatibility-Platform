#include "anco_wms_broker.h"
#include "gbinder_ipc.h"
#include "gbinder_local_request_p.h"
#include <errno.h>
#include <string.h>

struct AncoWmsBroker {
    GBinderIpc *ipc;
    GBinderRemoteObject *remote;
    gulong death;
    GThread *thread;
    guint busy;
};

static void clear_remote(AncoWmsBroker *b)
{
    GBinderRemoteObject *old = b->remote;
    const gulong death = b->death;
    b->remote = NULL;
    b->death = 0;
    if (old) {
        if (death) gbinder_remote_object_remove_handler(old, death);
        gbinder_remote_object_unref(old);
    }
}

static void broker_died(GBinderRemoteObject *object, void *data)
{
    AncoWmsBroker *b = data;
    /* Single retained endpoint, original unlink+clear, no service restart. */
    if (b->remote == object) clear_remote(b);
}

static GBinderRemoteObject *get_remote(AncoWmsBroker *b)
{
    if (b->remote) return b->remote;
    /* Original getSysAbility's native slot sends transaction2. Linux uses the
     * original HOST SAMgr MessageParcel header, not the guest-only descriptor.
     * Context manager is the caller's actual IPC handle0, not a guessed SA.
     */
    GBinderLocalRequest *request = gbinder_local_request_new(
        gbinder_ipc_io(b->ipc), gbinder_ipc_protocol(b->ipc), NULL);
    if (!request) return NULL;
    gbinder_local_request_append_int32(request, 0x100);
    gbinder_local_request_append_int32(request, 0);
    gbinder_local_request_append_string16(request, "ohos.samgr.accessToken");
    gbinder_local_request_append_int32(request, ANCO_WMS_SA);
    int transport = -1;
    GBinderRemoteReply *reply = gbinder_ipc_transact_sync_reply_main(
        b->ipc, 0, 2, request, &transport);
    GBinderRemoteObject *next = reply && !transport ?
        gbinder_remote_reply_read_object(reply) : NULL;
    if (reply) gbinder_remote_reply_unref(reply);
    gbinder_local_request_unref(request);
    if (!next) return NULL;
    if (gbinder_remote_object_is_dead(next)) {
        gbinder_remote_object_unref(next);
        return NULL;
    }
    const gulong death = gbinder_remote_object_add_death_handler(next, broker_died, b);
    if (!death) {
        gbinder_remote_object_unref(next);
        return NULL;
    }
    b->remote = next; /* read_object returns an owned reference */
    b->death = death;
    return next;
}

AncoWmsBroker *anco_wms_broker_new(GBinderIpc *ipc)
{
    if (!ipc) return NULL;
    AncoWmsBroker *b = g_new0(AncoWmsBroker, 1);
    b->ipc = gbinder_ipc_ref(ipc);
    b->thread = g_thread_self();
    get_remote(b); /* original constructor eagerly attempts resolution */
    return b;
}

int anco_wms_broker_free(AncoWmsBroker *b)
{
    if (!b) return 0;
    if (g_thread_self() != b->thread) return -EPERM;
    if (b->busy) return -EBUSY;
    clear_remote(b);
    gbinder_ipc_unref(b->ipc);
    g_free(b);
    return 0;
}

static int supported(const AncoWmsMessage *m)
{
    if (!m) return -EINVAL;
    switch (m->code) {
    case ANCO_WMS_CREATE_WINDOW: case ANCO_WMS_SHOW_WINDOW:
    case ANCO_WMS_HIDE_WINDOW: case ANCO_WMS_REMOVE_WINDOW:
    case ANCO_WMS_UPDATE_LAYOUT: case ANCO_WMS_CREATE_ACTIVITY:
    case ANCO_WMS_REMOVE_ACTIVITY: case ANCO_WMS_MOVE_ACTIVITY_TO_TOP:
    case ANCO_WMS_FRAME_LAYOUT_FINISH: return 0;
    default: return -ENOTSUP;
    }
}

static gboolean original_proxy_return(const AncoWmsResult *result, gint32 *value)
{
    if (result->local_error) return FALSE;
    /* Exact retained BinderProxy.transactNative: status -74 returns false
     * without exception. IWmsStub.Proxy then returns-1, original outer sends
     * once more. Do NOT label this as a decoded service reply.
     */
    if (result->transport == -EBADMSG) {
        *value = -1;
        return TRUE;
    }
    if (!result->transport && result->decoded) {
        *value = result->service_result;
        return TRUE;
    }
    return FALSE;
}

gboolean anco_wms_broker_send(AncoWmsBroker *b, const AncoWmsMessage *message,
                             AncoWmsSendResult *out)
{
    if (!out) return FALSE;
    *out = (AncoWmsSendResult){0, 0, {0, -1, FALSE, -1}};
    out->last.local_error = !b ? -EINVAL : supported(message);
    if (!out->last.local_error && g_thread_self() != b->thread)
        out->last.local_error = -EPERM;
    if (out->last.local_error) return FALSE;
    const AncoWmsMessage frozen = *message; /* original WmsMessage retains body */
    ++b->busy;
    GBinderRemoteObject *remote = get_remote(b);
    if (!remote) {
        out->last.local_error = -ENOTCONN;
        --b->busy;
        return FALSE; /* original return0 but NO transaction */
    }
    gboolean success = anco_wms_process_request(remote, &frozen, &out->last);
    ++out->attempts;
    if (original_proxy_return(&out->last, &out->original_return)) {
        if (out->original_return && (remote = get_remote(b)) != NULL) {
            success = anco_wms_process_request(remote, &frozen, &out->last);
            ++out->attempts;
            /* On exception, original sendRequest retains the previous return.
             * A successful but short Parcel readInt returns0 in native code;
             * provenance still keeps decodedFALSE and successFALSE here.
             */
            if (!original_proxy_return(&out->last, &out->original_return) &&
                !out->last.local_error && !out->last.transport)
                out->original_return = 0;
        }
    }
    --b->busy;
    return success;
}

static gboolean valid_window(const AncoWmsCreateState *s)
{
    return s && !(s->has_task && s->task_is_home) &&
        (s->type < 2000 || s->type == 2038 || s->type == 2005 || s->type == 2108);
}

gboolean anco_wms_broker_create_window(AncoWmsBroker *b,
    AncoWmsCreateState *s, AncoWmsCreateResult *out)
{
    if (!out) return FALSE;
    *out = (AncoWmsCreateResult){
        .disposition = ANCO_WMS_CREATE_NOT_ENTERED,
        .send = {0, 0, {0, -1, FALSE, -1}}
    };
    if (!b || g_thread_self() != b->thread) {
        out->send.last.local_error = b ? -EPERM : -EINVAL;
        return FALSE;
    }
    if (!valid_window(s)) {
        out->disposition = ANCO_WMS_CREATE_INVALID_WINDOW;
        return FALSE;
    }
    if (s->last_title && strstr(s->last_title, "com.tencent.mm.ui.LauncherUI") &&
        s->has_task && s->task_has_wechat_splash_activity) {
        out->disposition = ANCO_WMS_CREATE_SKIPPED;
        return FALSE;
    }
    s->activity_exists_process_died = FALSE;
    const AncoWmsMessage message = {
        .code = ANCO_WMS_CREATE_WINDOW,
        .data.create = {
            s->window,
            s->anco_enabled && s->has_activity ? s->activity : -1,
            s->type,
            s->anco_enabled && s->type >= 1000 && s->type <= 1999 &&
                s->has_parent ? s->parent : -1,
            s->has_task ? s->task : -1
        }
    };
    out->disposition = ANCO_WMS_CREATE_SEND_ENTERED;
    return anco_wms_broker_send(b, &message, &out->send);
}

gboolean anco_wms_broker_set_visibility(AncoWmsBroker *b,
    const AncoWmsCreateState *s, gboolean visible, gint32 reason,
    AncoWmsSendResult *out)
{
    if (!out) return FALSE;
    *out = (AncoWmsSendResult){0, 0, {0, -1, FALSE, -1}};
    if (!b || g_thread_self() != b->thread) {
        out->last.local_error = b ? -EPERM : -EINVAL;
        return FALSE;
    }
    if (!valid_window(s)) return FALSE;
    const AncoWmsMessage message = {
        .code = visible ? ANCO_WMS_SHOW_WINDOW : ANCO_WMS_HIDE_WINDOW,
        .data.visibility = {
            s->window,
            s->anco_enabled && s->has_activity ? s->activity : -1,
            s->has_task ? s->task : -1,
            reason
        }
    };
    return anco_wms_broker_send(b, &message, out);
}

gboolean anco_wms_broker_update_layout(AncoWmsBroker *b,
    const AncoWmsCreateState *s, const AncoWmsLayout *attrs,
    const AncoWmsLayoutState *geometry, AncoWmsSendResult *out)
{
    if (!out) return FALSE;
    *out = (AncoWmsSendResult){0, 0, {0, -1, FALSE, -1}};
    if (!b || g_thread_self() != b->thread) {
        out->last.local_error = b ? -EPERM : -EINVAL;
        return FALSE;
    }
    if (!attrs || !valid_window(s)) return FALSE;
    if (!geometry || !!geometry->anco_enabled != !!s->anco_enabled ||
        geometry->window_type != s->type || !!geometry->has_task != !!s->has_task ||
        !!geometry->has_parent != !!s->has_parent ||
        (s->has_parent && geometry->parent_window_id != s->parent)) {
        out->last.local_error = -EINVAL;
        return FALSE;
    }
    AncoWmsMessage message = {.code = ANCO_WMS_UPDATE_LAYOUT};
    out->last.local_error = anco_wms_layout_project(attrs, geometry,
                                                   &message.data.layout.params);
    if (out->last.local_error) return FALSE;
    message.data.layout.window = s->window;
    message.data.layout.activity = s->anco_enabled && s->has_activity ? s->activity : -1;
    message.data.layout.task = s->has_task ? s->task : -1;
    return anco_wms_broker_send(b, &message, out);
}
