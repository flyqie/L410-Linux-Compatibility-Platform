#include "anco_delegator_metadata.h"
#include <errno.h>

static gboolean send_metadata(GBinderRemoteObject *host, guint32 features,
    guint32 code, gint32 queue_size, guint32 value, AncoDelegatorResult *out)
{
    if (!out) return FALSE;
    *out = (AncoDelegatorResult){0, -1, FALSE, -1};
    if (!host) { out->local_error = -ENOTCONN; return FALSE; }
    GBinderClient *client = gbinder_client_new(host,
        (features & 2u) ? ANCO_DELEGATOR_IFACE : NULL);
    GBinderLocalRequest *request = client ? gbinder_client_new_request(client) : NULL;
    if (!request) {
        out->local_error = -ENOMEM;
    } else {
        GBinderWriter writer;
        gbinder_local_request_init_writer(request, &writer);
        if (code == 8u) gbinder_writer_append_int32(&writer, queue_size);
        /* libgbinder's existing append_int32 takes guint32 and preserves bits. */
        else gbinder_writer_append_int32(&writer, value);
        /* Original guest SendMessage flags0. No metadata application reply read. */
        GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(client,
            code, request, &out->transport);
        if (reply) gbinder_remote_reply_unref(reply);
        gbinder_local_request_unref(request);
    }
    if (client) gbinder_client_unref(client);
    return !out->local_error && !out->transport;
}

gboolean anco_delegator_queue_size_once(GBinderRemoteObject *host, guint32 features,
    gint32 size, AncoDelegatorResult *out)
{
    return send_metadata(host, features, 8u, size, 0u, out);
}

gboolean anco_delegator_transform_once(GBinderRemoteObject *host, guint32 features,
    guint32 transform, AncoDelegatorResult *out)
{
    return send_metadata(host, features, 9u, 0, transform, out);
}

gboolean anco_delegator_dataspace_once(GBinderRemoteObject *host, guint32 features,
    guint32 dataspace, AncoDelegatorResult *out)
{
    return send_metadata(host, features, 10u, 0, dataspace, out);
}
