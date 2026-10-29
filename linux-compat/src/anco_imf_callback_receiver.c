#include "anco_imf_callback_receiver.h"
#include "gbinder_reader_p.h"
#include "gbinder_buffer_p.h"
#include "gbinder_local_reply_p.h"
#include "gbinder_local_object_p.h"
#include "gbinder_output_data.h"
#include "gbinder_ipc.h"
#include "gbinder_rpc_protocol.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct AncoImfCallbackReceiver {
    GBinderLocalObject *object;
    AncoImfCallbackReceiverOps ops;
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
                                            AncoImfCallbackReceiver *self)
{
    GBinderLocalReply *reply = gbinder_local_object_new_reply(object);
    if (!reply) return NULL;
    gbinder_local_reply_append_int32(reply, (guint32)-1); /* EX_SECURITY */
    gbinder_local_reply_append_string16(reply,
        "Binder invocation to an incorrect interface");
    /* This rejection happens before any IMF consumer/AppOps recording.
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
    AncoImfCallbackReceiver *self = data;
    /* Generic descriptor/ping use libgbinder's original Binder handlers.
     * Ordinary unrecognized business codes have no invented success reply. */
    const gboolean window_notification = code == 1004 || code == 1005;
    if ((code < 1 || code > 14) && !window_notification) {
        *status = -EBADMSG; return NULL;
    }
    ++self->busy;
    *status = 0;
    GBinderReader reader;
    gbinder_remote_request_init_reader(request, &reader);
    CheckedReaderPrefix p;
    memcpy(&p, &reader, sizeof(p));
    if (p.data && p.data->buffer)
        gbinder_reader_init(&reader, (GBinderReaderData *)p.data,
                            0, p.data->buffer->size);
    AncoCallbackHeader header;
    GBinderLocalReply *reply = NULL;
    if (!anco_callback_read_header(&reader, flags, ANCO_IMF_CALLBACK_IFACE, &header)) {
        if (!(flags & 1)) {
            reply = security_exception(object, self);
            if (!reply) *status = -ENOMEM;
        }
    } else if (window_notification) {
        /* AncoImeProxy overrides these before its ordinary AIDL stub.
         * No body reads, no status integer or writeNoException in the reply.
         * Existing consumer dispatch carries the ORIGINAL transaction number.
         * Actual service owner must implement onWindowShown/hideImeWindow;
         * queueing this notification alone is not proof of either effect. */
        const AncoImfCallbackMessage message = {.code=code};
        AncoImfCallbackResult ignored = {.status=-1};
        if (self->ops.consume)
            self->ops.consume(&message, &header, &ignored, self->data);
        if (!(flags & 1)) {
            reply = gbinder_local_object_new_reply(object);
            if (!reply) *status = -ENOMEM;
        }
    } else {
        AncoImfCallbackMessage message;
        const int decoded=anco_imf_callback_read(&reader, code, &message);
        if(decoded){ *status=decoded; --self->busy; return NULL; }
        AncoImfCallbackResult result = {.status=-1};
        if (self->ops.consume)
            self->ops.consume(&message, &header, &result, self->data);
        if (!(flags & 1)) {
            /* A malformed internal consumer result is a local transport error,
             * never truncated text or fabricated original business success. */
            if ((code == 4 || code == 5) && result.units > 0x3ffffffeu) {
                *status = -EOVERFLOW;
            } else {
                reply = gbinder_local_object_new_reply(object);
                if (reply) {
                    gbinder_local_reply_append_int32(reply, (guint32)result.status);
                    if (code == 4 || code == 5) {
                        GBinderWriter writer;
                        gbinder_local_reply_init_writer(reply, &writer);
                        gbinder_writer_append_string16_utf16(&writer,
                            result.text, (gssize)result.units);
                    } else if (code == 6) {
                        gbinder_local_reply_append_int32(reply, (guint32)result.index);
                    }
                } else *status = -ENOMEM;
            }
        }
        anco_imf_callback_message_clear(&message);
    }
    --self->busy;
    return reply;
}

AncoImfCallbackReceiver *anco_imf_callback_receiver_new(GBinderIpc *ipc,
    const AncoImfCallbackReceiverOps *ops, void *data)
{
    if (!ipc || !ops) return NULL;
    const GBinderRpcProtocol *protocol = gbinder_ipc_protocol(ipc);
    if (!protocol || strcmp(protocol->name, "aidl2")) return NULL;
    AncoImfCallbackReceiver *self = g_new0(AncoImfCallbackReceiver, 1);
    self->ops = *ops; self->data = data;
    self->owner = g_thread_self();
    const char *ifaces[] = {ANCO_IMF_CALLBACK_IFACE, NULL};
    self->object = gbinder_local_object_new(ipc, ifaces, receive, self);
    if (!self->object) { g_free(self); return NULL; }
    return self;
}
GBinderLocalObject *anco_imf_callback_receiver_ref_object(AncoImfCallbackReceiver *self)
{ return self ? gbinder_local_object_ref(self->object) : NULL; }
static int publication_guard(AncoImfCallbackReceiver *self,
                             AncoImfPublicationResult *result)
{
    if (!result) return -EINVAL;
    *result=(AncoImfPublicationResult){.service_status=-1};
    if (!self) return -EINVAL;
    if (self->owner != g_thread_self()) return -EPERM;
    return self->busy ? -EBUSY : 0;
}
int anco_imf_callback_receiver_publish(AncoImfCallbackReceiver *self,
                                     AncoImfPublicationResult *result)
{
    int error=publication_guard(self,result);
    if (error) return error;
    if (self->publication_retained) return -EALREADY;
    ++self->busy;
    error=anco_imf_publication_add(self->object->ipc,self->object,result);
    if (result->attempted) self->publication_retained=TRUE;
    --self->busy;
    return error;
}
int anco_imf_callback_receiver_withdraw(AncoImfCallbackReceiver *self,
                                      AncoImfPublicationResult *result)
{
    int error=publication_guard(self,result);
    if (error) return error;
    /* No add attempt means this object has nothing to remove. */
    if (!self->publication_retained) return -ENOENT;
    ++self->busy;
    error=anco_imf_publication_remove(self->object->ipc,result);
    if (result->attempted && !result->transport_status &&
        result->reply_decoded && !result->service_status)
        self->publication_retained=FALSE;
    --self->busy;
    return error;
}
int anco_imf_callback_receiver_free(AncoImfCallbackReceiver *self)
{
    if (!self) return 0;
    if (self->owner != g_thread_self()) return -EPERM;
    if (self->busy || self->publication_retained) return -EBUSY;
    gbinder_local_object_drop(self->object);
    g_free(self);
    return 0;
}
