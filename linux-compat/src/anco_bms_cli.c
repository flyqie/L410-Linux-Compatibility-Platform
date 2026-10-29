/* Real original BMS transport entry. Optional Linux package-manager role
 * uses the original type1 caller UID1000; never copies another process token.
 * Uses the original SAMgr demand-load API and actual callback for BMS. */
#define _DEFAULT_SOURCE
#include "anco_bms_registration.h"
#include "anco_host_dependency.h"
#include "gbinder_ipc.h"
#include "gbinder_local_request_p.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <grp.h>
#include <errno.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const gboolean describe = argc == 2 && !strcmp(argv[1], "--describe");
    const gboolean query = argc == 5 && !strcmp(argv[1], "--manager-query");
    const gboolean unregister = argc == 3 && !strcmp(argv[1], "--manager-unregister");
    const gboolean notify = argc == 5 && !strcmp(argv[1], "--manager-notify");
    const gboolean manager = query || unregister || notify ||
        (argc == 3 && !strcmp(argv[1], "--manager-register"));
    gint32 host_user = -1;
    gint32 notify_status = 0;
    gint32 guest_user = -1;
    if (query) {
        char *end = NULL;
        errno = 0;
        long value = strtol(argv[3], &end, 10);
        if (errno || !end || *end || value < 0 || value > G_MAXINT32) return 64;
        host_user = (gint32)value;
    }
    if (notify) {
        char *status_end = NULL;
        char *user_end = NULL;
        errno = 0;
        long status_value = strtol(argv[3], &status_end, 10);
        if (errno || !status_end || *status_end || status_value < G_MININT32 ||
            status_value > G_MAXINT32) return 64;
        errno = 0;
        long user_value = strtol(argv[4], &user_end, 10);
        if (errno || !user_end || *user_end || user_value < 0 ||
            user_value > G_MAXINT32) return 64;
        notify_status = (gint32)status_value;
        guest_user = (gint32)user_value;
    }
    if (!describe && !manager && (argc != 3 || strcmp(argv[1], "--register"))) {
        fprintf(stderr, "usage: %s --describe | --register FILE | --manager-register FILE | --manager-unregister BUNDLE | --manager-query BUNDLE HOST_USER OUTPUT | --manager-notify BUNDLE STATUS GUEST_USER\n", argv[0]);
        return 64;
    }
    gchar *payload = NULL;
    gsize length = 0;
    if (!describe && !query && !unregister && !notify) {
        GError *error = NULL;
        if (!g_file_get_contents(argv[2], &payload, &length, &error)) {
            fprintf(stderr, "METADATA_READ_ERROR %s\n", error->message);
            g_error_free(error);
            return 65;
        }
    }
    /* All metadata is read before dropping root. Binder is opened only after
     * the actual process credentials change, not by altering parcel identity.
     * This option is for the platform manager, never an app launch wrapper. */
    if (manager) {
        if (getuid() != 0 || geteuid() != 0 || setgroups(0, NULL) ||
            setgid(1000) || setuid(1000)) {
            fprintf(stderr, "MANAGER_CREDENTIALS_FAILED\n");
            g_free(payload);
            return 70;
        }
        printf("LINUX_PACKAGE_MANAGER uid=%u gid=%u\n",
            (unsigned)getuid(), (unsigned)getgid());
    }
    GBinderIpc *ipc = gbinder_ipc_new("/dev/binder", "aidl2");
    if (!ipc) { g_free(payload); return 66; }
    GBinderRemoteObject *bms = anco_host_dependency_load(ipc, ANCO_BMS_SA);
    int transport = -1;
    GBinderRemoteReply *reply = NULL;
    printf("BMS_LOOKUP original_demand=1 object=%d\n", bms != NULL);
    int result = 68;
    if (bms) {
        GBinderClient *client = gbinder_client_new(bms, NULL);
        reply = client ? gbinder_client_transact_sync_reply(client, 0x5f4e5446,
            NULL, &transport) : NULL;
        GBinderReader reader;
        char *descriptor = NULL;
        if (reply && !transport) {
            gbinder_remote_reply_init_reader(reply, &reader);
            descriptor = gbinder_reader_read_string16(&reader);
        }
        const gboolean matches = descriptor && !strcmp(descriptor, ANCO_BMS_IFACE);
        printf("BMS_DESCRIPTOR transport=%d value=%s\n", transport,
            descriptor ? descriptor : "<missing>");
        if (reply) gbinder_remote_reply_unref(reply);
        if (client) gbinder_client_unref(client);
        g_free(descriptor);
        if (matches && describe) result = 0;
        else if (matches && query) {
            AncoBmsRegistrationResult status;
            /* Original current InnerBundleInfoExt::GetBundleInfo: bits0/1/2
             * request ApplicationInfo, HapModuleInfo and module AbilityInfo. */
            GBinderRemoteReply *query_reply = anco_bms_query_bundle(bms, argv[2], 7, host_user, &status);
            gsize size = 0;
            const void *data = NULL;
            if (query_reply) {
                GBinderReader query_reader;
                gbinder_remote_reply_init_reader(query_reply, &query_reader);
                data = gbinder_reader_get_data(&query_reader, &size);
            }
            GError *error = NULL;
            /* This file is a diagnostic envelope, not an FD-preserving replay
             * or an accepted catalog record. The library retains the real reply. */
            gboolean saved = data && g_file_set_contents(argv[4], data, size, &error);
            printf("BMS_QUERY local=%d transport=%d result_decoded=%d service=%d reply_bytes=%zu saved=%d\n",
                status.local_error, status.transport, status.decoded,
                status.service_result, (size_t)size, saved);
            if (error) { fprintf(stderr, "QUERY_SAVE_ERROR %s\n", error->message); g_error_free(error); }
            if (query_reply) gbinder_remote_reply_unref(query_reply);
            result = saved && status.decoded && !status.service_result ? 0 : 69;
        } else if (matches && unregister) {
            AncoBmsRegistrationResult status;
            const gboolean removed = anco_bms_unregister(bms, argv[2], FALSE, 0,
                &status);
            printf("BMS_UNREGISTER local=%d transport=%d decoded=%d service=%d removed=%d\n",
                status.local_error, status.transport, status.decoded,
                status.service_result, removed);
            result = removed ? 0 : 69;
        } else if (matches && notify) {
            AncoBmsRegistrationResult status;
            const gboolean published = anco_bms_send_update_status(
                bms, argv[2], notify_status, guest_user, &status);
            printf("BMS_UPDATE_STATUS local=%d transport=%d decoded=%d service=%d published=%d status=%d guest_user=%d\n",
                status.local_error, status.transport, status.decoded,
                status.service_result, published, notify_status, guest_user);
            result = published ? 0 : 69;
        } else if (matches) {
            AncoBmsRegistrationResult status;
            const gboolean accepted = anco_bms_register(bms, payload, length, &status);
            printf("BMS_REGISTER local=%d transport=%d decoded=%d service=%d accepted=%d\n",
                status.local_error, status.transport, status.decoded,
                status.service_result, accepted);
            result = accepted ? 0 : 69;
        }
        gbinder_remote_object_unref(bms);
    }
    gbinder_ipc_unref(ipc);
    g_free(payload);
    return result;
}
