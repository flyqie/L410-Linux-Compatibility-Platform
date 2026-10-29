#include "anco_delegator_transport.h"
#include <errno.h>

static gboolean send_once(GBinderRemoteObject *host, guint32 features, gint32 slot,
    const void *handle, gsize size, const gint32 rect[4], int fence,
    guint code, gboolean cached_queue, gint32 request_flag,
    gboolean explicit_flag, AncoDelegatorTaskNotify *notify,int *dup_error,
    AncoDelegatorResult *out)
{
    if (!out) return FALSE;
    *out = (AncoDelegatorResult){0, -1, FALSE, -1};
    if (dup_error) *dup_error=0;
    if (notify) {
        notify->attempted=FALSE;notify->status=0;
        if (!notify->pending ||
            (*notify->pending && notify->task!=-1 && !notify->notify)) {
            out->local_error=-EINVAL;return FALSE;
        }
    }
    if (!host) { out->local_error = -ENOTCONN; return FALSE; }
    GBinderClient *client = gbinder_client_new(host,
        (features & 2u) ? ANCO_DELEGATOR_IFACE : NULL);
    GBinderLocalRequest *request = client ? gbinder_client_new_request(client) : NULL;
    if (!request) {
        out->local_error = -ENOMEM;
    } else {
        if (code == ANCO_DELEGATOR_DEQUEUE_TRANSACTION)
            out->local_error = explicit_flag ?
                anco_delegator_append_dequeue_flag(request, request_flag, slot, handle, size) :
                anco_delegator_append_dequeue(request, slot, handle, size);
        else if (code == ANCO_DELEGATOR_QUEUE_TRANSACTION)
            out->local_error = cached_queue ?
                anco_delegator_append_cached_queue_diagnostic(request, slot, fence,dup_error) :
                anco_delegator_append_queue_diagnostic(request, slot, features, rect, fence,dup_error);
        else if (code == 4) {
            GBinderWriter writer;
            gbinder_local_request_init_writer(request, &writer);
            gbinder_writer_append_int32(&writer, slot);
        } /* code100 has an empty body. */
        if (!out->local_error) {
            if (notify && *notify->pending && notify->task!=-1) {
                notify->attempted=TRUE;
                notify->status=notify->notify(notify->task,notify->data);
                if (!notify->status) *notify->pending=FALSE;
            }
            /* Original SendMessage(...,flags0), including code4/code100. */
            GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(client,
                code,
                request, &out->transport);
            if (!out->transport && code == ANCO_DELEGATOR_DEQUEUE_TRANSACTION && request_flag && reply) {
                GBinderReader reader;
                gbinder_remote_reply_init_reader(reply, &reader);
                out->reply_decoded = gbinder_reader_read_int32(&reader, &out->reply_value);
            }
            if (reply) gbinder_remote_reply_unref(reply);
        }
        gbinder_local_request_unref(request);
    }
    if (client) gbinder_client_unref(client);
    return !out->local_error && !out->transport;
}

gboolean anco_delegator_dequeue_once(GBinderRemoteObject *host, guint32 features,
    gint32 slot, const void *handle, gsize size, AncoDelegatorResult *out)
{
    return send_once(host, features, slot, handle, size, NULL, -1,
        ANCO_DELEGATOR_DEQUEUE_TRANSACTION, FALSE, handle!=NULL, FALSE, NULL,NULL,out);
}

gboolean anco_delegator_dequeue_flag_once(GBinderRemoteObject *host,guint32 features,
    gint32 request_flag,gint32 slot,const void *handle,gsize size,AncoDelegatorResult *out)
{
    return send_once(host,features,slot,handle,size,NULL,-1,
        ANCO_DELEGATOR_DEQUEUE_TRANSACTION,FALSE,request_flag,TRUE,NULL,NULL,out);
}

gboolean anco_delegator_queue_once(GBinderRemoteObject *host, guint32 features,
    gint32 slot, const gint32 rect[4], int fence, AncoDelegatorResult *out)
{
    return send_once(host, features, slot, NULL, 0, rect, fence, ANCO_DELEGATOR_QUEUE_TRANSACTION, FALSE, 0, FALSE, NULL,NULL,out);
}

gboolean anco_delegator_queue_notify_once(GBinderRemoteObject *host,guint32 features,
    gint32 slot,const gint32 rect[4],int fence,gboolean cached,
    AncoDelegatorTaskNotify *notify,int *dup_error,AncoDelegatorResult *out)
{
    return send_once(host,features,slot,NULL,0,rect,fence,
        ANCO_DELEGATOR_QUEUE_TRANSACTION,cached,0,FALSE,notify,dup_error,out);
}

gboolean anco_delegator_cached_queue_once(GBinderRemoteObject *host, guint32 features,
    gint32 slot, int fence, AncoDelegatorResult *out)
{
    return send_once(host, features, slot, NULL, 0, NULL, fence,
        ANCO_DELEGATOR_QUEUE_TRANSACTION, TRUE, 0, FALSE, NULL,NULL,out);
}

gboolean anco_delegator_clear_slot_once(GBinderRemoteObject *host, guint32 features,
    gint32 slot, AncoDelegatorResult *out)
{
    return send_once(host, features, slot, NULL, 0, NULL, -1, 4, FALSE, 0, FALSE, NULL,NULL,out);
}

gboolean anco_delegator_clear_all_once(GBinderRemoteObject *host, guint32 features,
    AncoDelegatorResult *out)
{
    return send_once(host, features, 0, NULL, 0, NULL, -1, 100, FALSE, 0, FALSE, NULL,NULL,out);
}
