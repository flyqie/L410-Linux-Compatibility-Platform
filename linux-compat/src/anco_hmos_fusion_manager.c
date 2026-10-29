#include "anco_hmos_fusion_manager.h"
#include <errno.h>

gboolean anco_hmos_fusion_roidt_keepalive(GBinderRemoteObject *manager,
    AncoRoidtEnvState state, AncoHmosFusionResult *out)
{
    if (!out) return FALSE;
    *out = (AncoHmosFusionResult){-EINVAL, -1, FALSE, -1};
    if (!manager || (state != ANCO_ROIDT_ENV_START &&
        state != ANCO_ROIDT_ENV_END)) return FALSE;
    /* Harmony MessageParcel uses its own token header.  Suppress gbinder's
     * Android/AIDL header and reproduce WriteInterfaceToken exactly. */
    GBinderClient *client = gbinder_client_new(manager, NULL);
    GBinderLocalRequest *request = client ?
        gbinder_client_new_request(client) : NULL;
    if (!request) {
        out->local_error = -ENOMEM;
    } else {
        out->local_error = 0;
        gbinder_local_request_append_int32(request, 0x100);
        gbinder_local_request_append_int32(request, 0);
        gbinder_local_request_append_string16(request,
            ANCO_HMOS_FUSION_MANAGER_IFACE);
        gbinder_local_request_append_int32(request, state);
        GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(client,
            ANCO_HMOS_FUSION_ROIDT_KEEPALIVE_TRANSACTION, request,
            &out->transport);
        if (reply && !out->transport) {
            GBinderReader reader;
            gbinder_remote_reply_init_reader(reply, &reader);
            out->decoded = gbinder_reader_read_int32(&reader,
                &out->service_result);
        }
        if (reply) gbinder_remote_reply_unref(reply);
        gbinder_local_request_unref(request);
    }
    if (client) gbinder_client_unref(client);
    return !out->local_error && !out->transport && out->decoded &&
        !out->service_result;
}
