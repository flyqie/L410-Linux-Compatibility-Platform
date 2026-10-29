#include "anco_graphic_registration.h"
#include <errno.h>
#include <string.h>

int anco_graphic_append_create_surface_node(GBinderLocalRequest *request,
    guint64 identity, gint32 window_id, const char *name,
    GBinderLocalObject *delegator)
{
    if (!request || !name) return -EINVAL;
    if (!delegator) return -ENOSYS; /* original NULL strong object branch */
    const gsize length = strlen(name);
    if (length > G_MAXINT32) return -EOVERFLOW;
    GBinderWriter writer;
    gbinder_local_request_init_writer(request, &writer);
    gbinder_writer_append_int64(&writer, identity); /* actual guint64 parameter */
    gbinder_writer_append_int32(&writer, window_id);
    /* Android writeString8 writes length, then NUL-terminated padded bytes.
     * gbinder append_string8_len only writes those bytes, not the length. */
    gbinder_writer_append_int32(&writer, (gint32)length);
    gbinder_writer_append_string8_len(&writer, name, length);
    gbinder_writer_append_local_object(&writer, delegator);
    return 0;
}

gboolean anco_graphic_create_surface_node_once(GBinderRemoteObject *graphic,
    guint64 identity, gint32 window_id, const char *name,
    GBinderLocalObject *delegator, AncoGraphicResult *out)
{
    if (!out) return FALSE;
    *out = (AncoGraphicResult){0, -1, FALSE, -1};
    if (!graphic) { out->local_error = -ENOTCONN; return FALSE; }
    if (!name) { out->local_error = -EINVAL; return FALSE; }
    if (!delegator) { out->local_error = -ENOSYS; return FALSE; }
    GBinderLocalObject *held = gbinder_local_object_ref(delegator);
    GBinderClient *client = gbinder_client_new(graphic, ANCO_GRAPHIC_IFACE);
    GBinderLocalRequest *request = client ? gbinder_client_new_request(client) : NULL;
    if (!request) {
        out->local_error = -ENOMEM;
    } else {
        out->local_error = anco_graphic_append_create_surface_node(request,
            identity, window_id, name, held);
        if (!out->local_error) {
            GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(client,
                ANCO_GRAPHIC_CREATE_SURFACE_NODE, request, &out->transport);
            if (reply) {
                GBinderReader reader;
                gbinder_remote_reply_init_reader(reply, &reader);
                out->decoded = gbinder_reader_read_int32(&reader, &out->service_result);
            }
            if (reply) gbinder_remote_reply_unref(reply);
        }
        gbinder_local_request_unref(request);
    }
    if (client) gbinder_client_unref(client);
    gbinder_local_object_unref(held);
    return !out->local_error && !out->transport && out->decoded;
}

gboolean anco_graphic_remove_surface_node_once(GBinderRemoteObject *graphic,
    guint64 identity, gint32 window_id, AncoGraphicRemoveResult *out)
{
    if (!out) return FALSE;
    *out = (AncoGraphicRemoveResult){{0, -1, FALSE, -1}, 0};
    if (!graphic) { out->wire.local_error = -ENOTCONN; return FALSE; }
    GBinderClient *client = gbinder_client_new(graphic, ANCO_GRAPHIC_IFACE);
    GBinderLocalRequest *request = client ? gbinder_client_new_request(client) : NULL;
    if (!request) {
        out->wire.local_error = -ENOMEM;
    } else {
        GBinderWriter writer;
        gbinder_local_request_init_writer(request, &writer);
        gbinder_writer_append_int64(&writer, identity);
        gbinder_writer_append_int32(&writer, window_id);
        GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(client,
            ANCO_GRAPHIC_REMOVE_SURFACE_NODE, request, &out->wire.transport);
        if (reply) {
            GBinderReader reader;
            gbinder_remote_reply_init_reader(reply, &reader);
            out->wire.decoded = gbinder_reader_read_int32(&reader, &out->wire.service_result);
            if (out->wire.decoded) out->original_return = out->wire.service_result;
            gbinder_remote_reply_unref(reply);
        }
        gbinder_local_request_unref(request);
    }
    if (client) gbinder_client_unref(client);
    return !out->wire.local_error && !out->wire.transport && out->wire.decoded;
}
