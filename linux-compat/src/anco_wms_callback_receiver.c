#include "anco_wms_callback_receiver.h"
#include "gbinder_reader_p.h"
#include "gbinder_buffer_p.h"
#include "gbinder_local_reply_p.h"
#include "gbinder_local_object_p.h"
#include "gbinder_local_request_p.h"
#include "gbinder_output_data.h"
#include "gbinder_ipc.h"
#include "gbinder_rpc_protocol.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct AncoWmsCallbackReceiver {
    GBinderLocalObject *object;
    AncoWmsCallbackReceiverOps ops;
    void *data;
    guint busy;
    GThread *owner;
    gboolean publication_retained;
};
typedef struct {
    const guint8 *start, *end, *ptr;
    const GBinderReaderData *data;
} CheckedReaderPrefix;
G_STATIC_ASSERT(sizeof(GBinderReader) >= sizeof(CheckedReaderPrefix));
G_STATIC_ASSERT(offsetof(CheckedReaderPrefix, data) == 3 * sizeof(void *));

static GBinderLocalReply *security_exception(GBinderLocalObject *object,
                                            AncoWmsCallbackReceiver *self)
{
    GBinderLocalReply *reply = gbinder_local_object_new_reply(object);
    if (!reply) return NULL;
    gbinder_local_reply_append_int32(reply, (guint32)-1); /* EX_SECURITY */
    gbinder_local_reply_append_string16(reply,
        "Binder invocation to an incorrect interface");
    /* This rejection happens before any window consumer/AppOps recording.
     * Original flags&2 exception path clears its collection before serialization.
     * Do not prepend invented AppOps records or a writeNoException marker. */
    const char *trace = self->ops.exception_trace ?
        self->ops.exception_trace(self->data) : NULL;
    GBinderOutputData *out = gbinder_local_reply_data(reply);
    const guint start = out->bytes->len;
    gbinder_local_reply_append_int32(reply, 0);
    if (trace) {
        gbinder_local_reply_append_string16(reply, trace);
        const guint32 length = out->bytes->len - start;
        memcpy(out->bytes->data + start, &length, sizeof(length));
    }
    return reply;
}

static GBinderLocalReply *receive(GBinderLocalObject *object,
    GBinderRemoteRequest *request, guint code, guint flags, int *status, void *data)
{
    AncoWmsCallbackReceiver *self = data;
    /* Generic descriptor/ping use libgbinder's original Binder handlers.
     * Ordinary unrecognized business codes have no invented success reply. */
    if (code < 1 || code > 7) { *status = -EBADMSG; return NULL; }
    ++self->busy;
    *status = 0;
    GBinderReader reader;
    gbinder_remote_request_init_reader(request, &reader);
    CheckedReaderPrefix p;
    memcpy(&p, &reader, sizeof(p));
    if (p.data && p.data->buffer)
        gbinder_reader_init(&reader, (GBinderReaderData *)p.data,
                            0, p.data->buffer->size);
    AncoWmsCallbackHeader header;
    GBinderLocalReply *reply = NULL;
    if (!anco_wms_callback_read_header(&reader, flags, &header)) {
        if (!(flags & 1)) {
            reply = security_exception(object, self);
            if (!reply) *status = -ENOMEM;
        }
    } else {
        AncoWmsCallbackMessage message;
        /* Short/overlapping scalar data is the original default projection.
         * Local EBADMSG must not replace the original callback result. */
        const int projection = anco_wms_callback_read(&reader, code, &message);
        const guint32 uid = gbinder_remote_request_sender_euid(request);
        /* Actual reverse traffic, not a completion receipt or a focus command.
         * Keep decoded/defaulted parameters visible while real consumers are
         * being connected; unavailable consumers below still return -1. */
        printf("WMS_REAL_CALLBACK code=%u uid=%u projection=%d",code,uid,projection);
        switch(code) {
        case 1: printf(" focus=%d display=%d",message.data.container_focus.focused,
            message.data.container_focus.display); break;
        case 2: printf(" task=%d mode=%d",message.data.mode.task,message.data.mode.mode); break;
        case 3: printf(" task=%d bounds=%d,%d,%d,%d scale=%.9g",message.data.bounds.task,
            message.data.bounds.left,message.data.bounds.top,message.data.bounds.right,
            message.data.bounds.bottom,(double)message.data.bounds.scale); break;
        case 4: printf(" focus=%d task=%d display=%d",message.data.task_focus.focused,
            message.data.task_focus.task,message.data.task_focus.display); break;
        case 5: printf(" task=%d mid_scene=%d",message.data.mid_scene.task,
            message.data.mid_scene.mid_scene); break;
        case 6: printf(" task=%d visibility=%d",message.data.visibility.task,
            message.data.visibility.state); break;
        case 7: printf(" smart_edge_present=%d",message.data.smart_edge.present); break;
        }
        putchar('\n');
        /* Java getAppId uses signed int remainder, including sentinel values. */
        const gint64 signed_uid = uid <= INT32_MAX ? (gint64)uid :
                                                   (gint64)uid - 0x100000000LL;
        gboolean done = FALSE;
        if (signed_uid % 100000 == 5557) {
            if (code == 7) {
                if (message.data.smart_edge.present && self->ops.smart_edge)
                    done = self->ops.smart_edge(&message, self->data);
            } else if (self->ops.window) {
                done = self->ops.window(&message, &header, self->data);
            }
        }
        printf("WMS_CALLBACK_CONSUMED code=%u result=%d\n",code,done?0:-1);
        /* Original Stub writes bare int even for methods normally sent oneway;
         * actual one-way Binder transactions have no application reply. */
        if (!(flags & 1)) {
            reply = gbinder_local_object_new_reply(object);
            if (reply) gbinder_local_reply_append_int32(reply, done ? 0 : (guint32)-1);
            else *status = -ENOMEM;
        }
    }
    --self->busy;
    return reply;
}

AncoWmsCallbackReceiver *anco_wms_callback_receiver_new(GBinderIpc *ipc,
    const AncoWmsCallbackReceiverOps *ops, void *data)
{
    if (!ipc || !ops) return NULL;
    const GBinderRpcProtocol *protocol = gbinder_ipc_protocol(ipc);
    if (!protocol || strcmp(protocol->name, "aidl2")) return NULL;
    AncoWmsCallbackReceiver *self = g_new0(AncoWmsCallbackReceiver, 1);
    self->ops = *ops; self->data = data;
    self->owner = g_thread_self();
    const char *ifaces[] = {ANCO_WMS_CALLBACK_IFACE, NULL};
    self->object = gbinder_local_object_new(ipc, ifaces, receive, self);
    if (!self->object) { g_free(self); return NULL; }
    return self;
}
GBinderLocalObject *anco_wms_callback_receiver_ref_object(AncoWmsCallbackReceiver *self)
{ return self ? gbinder_local_object_ref(self->object) : NULL; }
static int publication(AncoWmsCallbackReceiver *self, gboolean add,
                       AncoWmsPublicationResult *result)
{
    if (!result) return -EINVAL;
    *result = (AncoWmsPublicationResult){.service_status=-1};
    if (!self) return -EINVAL;
    if (self->owner != g_thread_self()) return -EPERM;
    if (self->busy) return -EBUSY;
    if (add && self->publication_retained) return -EALREADY;
    if (!add && !self->publication_retained) return -ENOENT;
    GBinderIpc *ipc = self->object->ipc;
    const GBinderRpcProtocol *protocol = gbinder_ipc_protocol(ipc);
    if (!protocol || strcmp(protocol->name, "aidl2")) return -EPROTONOSUPPORT;
    GBinderLocalRequest *request = gbinder_local_request_new(
        gbinder_ipc_io(ipc), protocol, NULL);
    if (!request) return -ENOMEM;
    gbinder_local_request_append_int32(request, 0x100);
    gbinder_local_request_append_int32(request, 0);
    gbinder_local_request_append_string16(request, "ohos.samgr.accessToken");
    gbinder_local_request_append_int32(request, ANCO_WMS_CALLBACK_SA);
    if (add) {
        gbinder_local_request_append_local_object(request, self->object);
        gbinder_local_request_append_int32(request, 0);
        gbinder_local_request_append_int32(request, 8);
        gbinder_local_request_append_string16(request, "");
        gbinder_local_request_append_string16(request, "");
    }
    ++self->busy;
    result->attempted = TRUE;
    if (add) self->publication_retained = TRUE;
    int status = -1;
    GBinderRemoteReply *reply = gbinder_ipc_transact_sync_reply_main(
        ipc, 0, add ? 3 : 4, request, &status);
    result->transport_status = status;
    if (!status && reply) {
        GBinderReader reader;
        gbinder_remote_reply_init_reader(reply, &reader);
        result->reply_decoded = gbinder_reader_read_int32(&reader, &result->service_status);
    }
    if (!add && !status && result->reply_decoded && !result->service_status)
        self->publication_retained = FALSE;
    gbinder_remote_reply_unref(reply);
    gbinder_local_request_unref(request);
    --self->busy;
    return status ? status : result->reply_decoded ? result->service_status : -1;
}
int anco_wms_callback_receiver_publish(AncoWmsCallbackReceiver *self, AncoWmsPublicationResult *result)
{ return publication(self, TRUE, result); }
int anco_wms_callback_receiver_withdraw(AncoWmsCallbackReceiver *self, AncoWmsPublicationResult *result)
{ return publication(self, FALSE, result); }
int anco_wms_callback_receiver_free(AncoWmsCallbackReceiver *self)
{
    if (!self) return 0;
    if (self->owner != g_thread_self()) return -EPERM;
    if (self->busy || self->publication_retained) return -EBUSY;
    gbinder_local_object_drop(self->object);
    g_free(self);
    return 0;
}
