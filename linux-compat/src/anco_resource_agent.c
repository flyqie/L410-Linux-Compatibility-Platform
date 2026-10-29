#define _GNU_SOURCE
#include "anco_resource_agent.h"

#include "gbinder_ipc.h"
#include "gbinder_local_request_p.h"
#include <errno.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#define ANCO_RESOURCE_PROVIDER_SA 66894
#define ANCO_RESOURCE_ICON_ID 0x01000001
#define ANCO_BROKER_UID 5557
#define ANCO_RESOURCE_IFACE "com.huawei.idl.AncoResourceManagerProxy"
#define ANCO_LINUX_BUNDLE_PREFIX "org.l410.linux."
#define ANCO_ICON_MAX_BYTES (2u * 1024u * 1024u)

struct anco_resource_agent {
    GBinderIpc *ipc;
    GBinderLocalObject *object;
    char *icon_directory;
    gboolean published;
};

static GBinderLocalRequest *samgr_request(GBinderIpc *ipc)
{
    GBinderLocalRequest *request = gbinder_local_request_new(
        gbinder_ipc_io(ipc), gbinder_ipc_protocol(ipc), NULL);
    if (request) {
        gbinder_local_request_append_int32(request, 0x100);
        gbinder_local_request_append_int32(request, 0);
        gbinder_local_request_append_string16(request, "ohos.samgr.accessToken");
    }
    return request;
}

static GBinderRemoteObject *lookup_provider(GBinderIpc *ipc, int *transport)
{
    GBinderLocalRequest *request = samgr_request(ipc);
    if (!request) {
        if (transport) *transport = -ENOMEM;
        return NULL;
    }
    gbinder_local_request_append_int32(request, ANCO_RESOURCE_PROVIDER_SA);
    int status = -1;
    GBinderRemoteReply *reply = gbinder_ipc_transact_sync_reply_main(
        ipc, 0, 2, request, &status);
    GBinderRemoteObject *object = reply && !status ?
        gbinder_remote_reply_read_object(reply) : NULL;
    if (transport) *transport = status;
    if (reply) gbinder_remote_reply_unref(reply);
    gbinder_local_request_unref(request);
    return object;
}

static int change_provider(AncoResourceAgent *agent, guint code)
{
    GBinderLocalRequest *request = samgr_request(agent->ipc);
    if (!request) return -ENOMEM;
    gbinder_local_request_append_int32(request, ANCO_RESOURCE_PROVIDER_SA);
    if (code == 3) {
        /* Original BpSystemAbilityManager::AddSystemAbility fields. */
        gbinder_local_request_append_local_object(request, agent->object);
        gbinder_local_request_append_int32(request, 0);
        gbinder_local_request_append_int32(request, 8);
        gbinder_local_request_append_string16(request, "");
        gbinder_local_request_append_string16(request, "");
    }
    int transport = -1;
    GBinderRemoteReply *reply = gbinder_ipc_transact_sync_reply_main(
        agent->ipc, 0, code, request, &transport);
    gint32 result = -1;
    const gboolean decoded = reply &&
        gbinder_remote_reply_read_int32(reply, &result);
    printf("RESOURCE_PROVIDER_CHANGE code=%u id=%d transport=%d decoded=%d result=%d\n",
        code, ANCO_RESOURCE_PROVIDER_SA, transport, decoded, result);
    if (reply) gbinder_remote_reply_unref(reply);
    gbinder_local_request_unref(request);
    return !transport && decoded ? result : transport ? transport : -EPROTO;
}

static gboolean valid_bundle(const char *bundle)
{
    if (!bundle || !g_str_has_prefix(bundle, ANCO_LINUX_BUNDLE_PREFIX) ||
        !bundle[strlen(ANCO_LINUX_BUNDLE_PREFIX)]) return FALSE;
    for (const unsigned char *p = (const unsigned char *)bundle; *p; ++p)
        if (!(g_ascii_islower(*p) || g_ascii_isdigit(*p) ||
              *p == '.' || *p == '_'))
            return FALSE;
    return TRUE;
}

static int read_icon(AncoResourceAgent *agent, gint32 resource_id,
                     const char *bundle, gint32 density,
                     gchar **bytes, gsize *length)
{
    *bytes = NULL;
    *length = 0;
    if (resource_id != ANCO_RESOURCE_ICON_ID || !valid_bundle(bundle) ||
        density < 0 || density > 1000) return 5;
    gchar *name = g_strconcat(bundle, ".png", NULL);
    gchar *path = g_build_filename(agent->icon_directory, name, NULL);
    g_free(name);
    GError *error = NULL;
    const gboolean loaded = g_file_get_contents(path, bytes, length, &error);
    g_free(path);
    if (!loaded || !*bytes || !*length || *length > ANCO_ICON_MAX_BYTES ||
        *length < 8 || memcmp(*bytes, "\x89PNG\r\n\x1a\n", 8)) {
        printf("RESOURCE_PROVIDER_ICON_REJECT bundle=%s id=%d density=%d error=%s bytes=%zu\n",
            bundle ? bundle : "", resource_id, density,
            error ? error->message : "invalid PNG", (size_t)*length);
        g_clear_error(&error);
        g_clear_pointer(bytes, g_free);
        *length = 0;
        return 5;
    }
    g_clear_error(&error);
    return 0;
}

static GBinderLocalReply *media_reply(GBinderLocalObject *self,
    AncoResourceAgent *agent, GBinderRemoteRequest *request, gboolean with_user,
    int *status)
{
    GBinderReader reader;
    gint32 resource_id = 0, density = 0, user = -1;
    const char *bundle = NULL;
    gbinder_remote_request_init_reader(request, &reader);
    const gboolean decoded =
        gbinder_reader_read_int32(&reader, &resource_id) &&
        gbinder_reader_read_nullable_string8(&reader, &bundle, NULL) &&
        gbinder_reader_read_int32(&reader, &density) &&
        (!with_user || gbinder_reader_read_int32(&reader, &user)) &&
        gbinder_reader_at_end(&reader);
    gchar *bytes = NULL;
    gsize length = 0;
    const int result = decoded ? read_icon(agent, resource_id, bundle,
        density, &bytes, &length) : 5;
    GBinderLocalReply *reply = gbinder_local_object_new_reply(self);
    if (reply) {
        GBinderWriter writer;
        gbinder_local_reply_init_writer(reply, &writer);
        /* Original ResmgrServicesGetMediaProxy reply: method result,
         * media byte length, then WritePodArray<int8_t>
         * (uint32 vector length + unpadded bytes). */
        gbinder_writer_append_int32(&writer, result);
        gbinder_writer_append_int32(&writer,
            result ? 0 : (gint32)length);
        gbinder_writer_append_int32(&writer, result ? 0 : (gint32)length);
        if (!result) gbinder_writer_append_bytes(&writer, bytes, length);
    }
    printf("RESOURCE_PROVIDER_MEDIA code=%d bundle=%s id=%d density=%d user=%d bytes=%zu result=%d\n",
        with_user ? 6 : 2, bundle ? bundle : "", resource_id, density, user,
        (size_t)length, result);
    g_free(bytes);
    *status = 0;
    return reply;
}

static GBinderLocalReply *string_reply(GBinderLocalObject *self,
    GBinderRemoteRequest *request, gboolean with_user, int *status)
{
    GBinderReader reader;
    gint32 resource_id = 0, user = -1;
    const char *bundle = NULL;
    gbinder_remote_request_init_reader(request, &reader);
    const gboolean decoded =
        gbinder_reader_read_int32(&reader, &resource_id) &&
        gbinder_reader_read_nullable_string8(&reader, &bundle, NULL) &&
        (!with_user || gbinder_reader_read_int32(&reader, &user)) &&
        gbinder_reader_at_end(&reader);
    GBinderLocalReply *reply = gbinder_local_object_new_reply(self);
    if (reply) {
        /* Labels stay literal (labelId 0). Preserve the original reply shape
         * for completeness without inventing a Linux label resource. */
        gbinder_local_reply_append_int32(reply, 5);
        gbinder_local_reply_append_string8(reply, "");
    }
    printf("RESOURCE_PROVIDER_STRING code=%d bundle=%s id=%d user=%d decoded=%d result=5\n",
        with_user ? 5 : 1, bundle ? bundle : "", resource_id, user, decoded);
    *status = 0;
    return reply;
}

static GBinderLocalReply *on_resource_transaction(GBinderLocalObject *self,
    GBinderRemoteRequest *request, guint code, guint flags, int *status,
    void *user_data)
{
    AncoResourceAgent *agent = user_data;
    const char *interface = gbinder_remote_request_interface(request);
    const uid_t uid = gbinder_remote_request_sender_euid(request);
    if (uid != ANCO_BROKER_UID || !interface ||
        strcmp(interface, ANCO_RESOURCE_IFACE)) {
        printf("RESOURCE_PROVIDER_REJECT code=%u uid=%lu flags=%u iface=%s\n",
            code, (unsigned long)uid, flags, interface ? interface : "");
        *status = -1;
        return NULL;
    }
    if (code == 2 || code == 6)
        return media_reply(self, agent, request, code == 6, status);
    if (code == 1 || code == 5)
        return string_reply(self, request, code == 5, status);
    printf("RESOURCE_PROVIDER_UNIMPLEMENTED code=%u uid=%lu flags=%u\n",
        code, (unsigned long)uid, flags);
    *status = -1;
    return NULL;
}

AncoResourceAgent *anco_resource_agent_new(GBinderIpc *ipc,
                                           const char *icon_directory)
{
    if (!ipc || !icon_directory || *icon_directory != '/' ||
        !g_file_test(icon_directory, G_FILE_TEST_IS_DIR)) return NULL;
    AncoResourceAgent *agent = g_new0(AncoResourceAgent, 1);
    agent->ipc = gbinder_ipc_ref(ipc);
    agent->icon_directory = g_strdup(icon_directory);
    const char *interfaces[] = { ANCO_RESOURCE_IFACE, NULL };
    agent->object = gbinder_local_object_new(agent->ipc, interfaces,
        on_resource_transaction, agent);
    if (!agent->object) {
        gbinder_ipc_unref(agent->ipc);
        g_free(agent->icon_directory);
        g_free(agent);
        return NULL;
    }
    return agent;
}

int anco_resource_agent_register(AncoResourceAgent *agent)
{
    if (!agent) return -EINVAL;
    if (agent->published) return -EALREADY;
    int transport = -1;
    GBinderRemoteObject *existing = lookup_provider(agent->ipc, &transport);
    if (existing) {
        gbinder_remote_object_unref(existing);
        return -EADDRINUSE;
    }
    if (transport != 7) return transport ? transport : -EPROTO;
    const int result = change_provider(agent, 3);
    if (!result) agent->published = TRUE;
    return result;
}

int anco_resource_agent_free(AncoResourceAgent *agent)
{
    if (!agent) return 0;
    if (agent->published) {
        const int result = change_provider(agent, 4);
        if (result) return result;
        agent->published = FALSE;
    }
    gbinder_local_object_drop(agent->object);
    gbinder_ipc_unref(agent->ipc);
    g_free(agent->icon_directory);
    g_free(agent);
    return 0;
}
