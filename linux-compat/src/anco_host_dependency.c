/* Original current-host SAMgr APIs, not implementations of host services.
 * Check/Get cannot start a NOT_STARTED process. Load uses transaction7 and
 * the original ISystemAbilityLoadCallback; current proxy maximum wait is4s.
 */
#include <gbinder.h>
#include "anco_host_dependency.h"
#include "gbinder_ipc.h"
#include "gbinder_local_request_p.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

static GBinderLocalRequest *samgr_request(GBinderIpc *ipc, gint32 id)
{
    GBinderLocalRequest *req = gbinder_local_request_new(
        gbinder_ipc_io(ipc), gbinder_ipc_protocol(ipc), NULL);
    if (req) {
        gbinder_local_request_append_int32(req, 0x100);
        gbinder_local_request_append_int32(req, 0);
        gbinder_local_request_append_string16(req, "ohos.samgr.accessToken");
        gbinder_local_request_append_int32(req, id);
    }
    return req;
}

GBinderRemoteObject *anco_host_dependency_get(GBinderIpc *ipc, gint32 id)
{
    if (!ipc || id <= 0 || id >= 0x1000000) return NULL;
    GBinderRemoteObject *object = NULL;
    for (unsigned attempt = 0; attempt < 7 && !object; ++attempt) {
        GBinderLocalRequest *req = samgr_request(ipc, id);
        if (!req) break;
        gbinder_local_request_append_int32(req, 0); /* original initial bool false */
        int status = -1;
        GBinderRemoteReply *reply = gbinder_ipc_transact_sync_reply_main(
            ipc, 0, 12, req, &status);
        gint32 exists = 0;
        gboolean decoded = FALSE;
        if (reply && !status) {
            GBinderReader reader;
            gbinder_remote_reply_init_reader(reply, &reader);
            object = gbinder_reader_read_object(&reader);
            if (object) {
                decoded = gbinder_reader_read_int32(&reader, &exists);
                if (!decoded) {
                    gbinder_remote_object_unref(object);
                    object = NULL;
                }
            }
        }
        printf("HOST_DEPENDENCY_GET id=%d attempt=%u transport=%d object=%d "
               "bool_decoded=%d isExist=%d\n", id, attempt + 1, status,
               object != NULL, decoded, exists != 0);
        if (reply) gbinder_remote_reply_unref(reply);
        gbinder_local_request_unref(req);
        /* Status7 includes missing, loading and unloading. A NOT_STARTED
         * process requires the original Load API, not more Check retries. */
        if (status == 1 || object) break;
        if (attempt < 6) g_usleep(200000);
    }
    return object;
}

#define LOAD_CALLBACK_IFACE "OHOS.ISystemAbilityLoadCallback"
typedef struct {
    gint32 id;
    GMainLoop *loop;
    GBinderRemoteObject *object;
    gboolean expired;
} LoadCallback;

static GBinderLocalReply *on_load(GBinderLocalObject *self,
    GBinderRemoteRequest *request, guint code, guint flags, int *status,
    void *data)
{
    LoadCallback *load = data;
    const char *iface = gbinder_remote_request_interface(request);
    (void)self;
    (void)flags;
    *status = 1; /* original token-mismatch return */
    if (!iface || strcmp(iface, LOAD_CALLBACK_IFACE)) return NULL;
    *status = -ENOSYS;
    if (code != 1 && code != 2) return NULL; /* local load only */
    GBinderReader reader;
    gint32 id = -1;
    gbinder_remote_request_init_reader(request, &reader);
    *status = 22; /* original ERR_INVALID_VALUE */
    if (!gbinder_reader_read_int32(&reader, &id) || id < 0 || id >= 0x1000000)
        return NULL;
    GBinderRemoteObject *object = code == 1 ?
        gbinder_reader_read_object(&reader) : NULL;
    printf("HOST_DEPENDENCY_CALLBACK id=%d expected=%d code=%u object=%d\n",
           id, load->id, code, object != NULL);
    /* Each callback object belongs to this one local Load request. Do not
     * transfer a returned reference for another request into our runtime. */
    if (id == load->id) {
        if (load->object) gbinder_remote_object_unref(load->object);
        load->object = object;
        object = NULL;
        /* Original synchronous proxy waits for a non-null object, including
         * after OnFail/null callbacks. Failure is never synthetic readiness. */
        if (load->object) g_main_loop_quit(load->loop);
    }
    if (object) gbinder_remote_object_unref(object);
    *status = 0; /* original void callback; no extra reply payload */
    return NULL;
}

static gboolean load_timeout(void *data)
{
    LoadCallback *load = data;
    load->expired = TRUE;
    g_main_loop_quit(load->loop);
    return G_SOURCE_REMOVE;
}

GBinderRemoteObject *anco_host_dependency_load(GBinderIpc *ipc, gint32 id)
{
    if (!ipc || id <= 0 || id >= 0x1000000) return NULL;
    LoadCallback load = { .id = id, .loop = g_main_loop_new(NULL, FALSE) };
    const char *ifaces[] = { LOAD_CALLBACK_IFACE, NULL };
    GBinderLocalObject *callback = gbinder_local_object_new(ipc, ifaces,
        on_load, &load);
    GBinderLocalRequest *req = callback ? samgr_request(ipc, id) : NULL;
    if (req) {
        gbinder_local_request_append_local_object(req, callback);
        int transport = -1;
        GBinderRemoteReply *reply = gbinder_ipc_transact_sync_reply_main(
            ipc, 0, 7, req, &transport);
        gint32 result = -1;
        gboolean decoded = reply && !transport &&
            gbinder_remote_reply_read_int32(reply, &result);
        printf("HOST_DEPENDENCY_LOAD id=%d transport=%d decoded=%d result=%d\n",
               id, transport, decoded, result);
        if (reply) gbinder_remote_reply_unref(reply);
        gbinder_local_request_unref(req);
        if (decoded && !result) {
            if (!load.object) {
                /* GLib dispatch is the Linux-side equivalent of the original
                 * Binder callback thread/condition wait. Never block callbacks
                 * behind a sleep, and never hold this temporary stub afterward. */
                guint timer = g_timeout_add(4000, load_timeout, &load);
                g_main_loop_run(load.loop);
                if (!load.expired) g_source_remove(timer);
            }
        } else if (load.object) {
            gbinder_remote_object_unref(load.object);
            load.object = NULL;
        }
    }
    if (callback) gbinder_local_object_drop(callback); /* disables queued calls */
    g_main_loop_unref(load.loop);
    printf("HOST_DEPENDENCY id=%d original_load=1 object=%d timeout=%d\n",
           id, load.object != NULL, load.expired);
    return load.object;
}

#ifndef ANCO_HOST_DEPENDENCY_EMBEDDED
int main(int argc, char **argv)
{
    char *end = NULL;
    const gboolean check = argc == 3 && !strcmp(argv[1], "--check");
    if (argc != 2 && !check) return 64;
    errno = 0;
    long id = strtol(argv[check ? 2 : 1], &end, 10);
    if (errno || !end || *end || id <= 0 || id >= 0x1000000) return 64;
    GBinderIpc *ipc = gbinder_ipc_new("/dev/binder", "aidl2");
    if (!ipc) return 65;
    GBinderRemoteObject *object = check ? anco_host_dependency_get(ipc, (gint32)id) :
        anco_host_dependency_load(ipc, (gint32)id);
    int result = 66;
    if (object) {
        GBinderClient *client = gbinder_client_new(object, NULL);
        int status = -1;
        GBinderRemoteReply *reply = client ? gbinder_client_transact_sync_reply(
            client, GBINDER_FOURCC('_', 'N', 'T', 'F'), NULL, &status) : NULL;
        char *descriptor = reply && !status ?
            gbinder_remote_reply_read_string16(reply) : NULL;
        printf("HOST_DESCRIPTOR id=%ld transport=%d value=%s\n", id, status,
            descriptor ? descriptor : "<missing>");
        if (descriptor && *descriptor) result = 0;
        g_free(descriptor);
        if (reply) gbinder_remote_reply_unref(reply);
        if (client) gbinder_client_unref(client);
        gbinder_remote_object_unref(object);
    }
    gbinder_ipc_unref(ipc);
    return result;
}
#endif
