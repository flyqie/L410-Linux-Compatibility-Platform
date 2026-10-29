#define _GNU_SOURCE
#include "anco_bms_registration.h"
#include "gbinder_writer_p.h"
#include "gbinder_io.h"
#include <gutil_intarray.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int validate_length(gsize length)
{
    return length > 0 && length <= ANCO_BMS_MAX_BYTES ? 0 : -EMSGSIZE;
}

static void close_request_fd(gpointer pointer)
{
    close(GPOINTER_TO_INT(pointer));
}

/* Bridge the original OHOS FD object without libgbinder's borrowed-FD
 * fallback. Uses the retained libgbinder WriterData ABI, not a host interface.
 * Its writer private prefix is WriterData*,offset; copy the first pointer
 * without inventing a different layout or violating aliasing rules.
 */
static int append_dup_fd_checked(GBinderWriter *writer, int fd)
{
    GBinderWriterData *data;
    memcpy(&data, writer->d, sizeof(data));
    if (!data || data->io != &gbinder_io_64) return -ENOTSUP;
    if (data->bytes->len > G_MAXUINT - 24) return -EOVERFLOW;
    const int duplicated = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (duplicated < 0) return -errno;
    guint32 object[6];
    const guint written = data->io->encode_fd_object(object, duplicated);
    if (written != sizeof(object)) { close(duplicated); return -ENOTSUP; }
    const guint offset = data->bytes->len;
    gbinder_writer_append_bytes(writer, object, written);
    if (!data->offsets) data->offsets = gutil_int_array_new();
    gutil_int_array_append(data->offsets, offset);
    gbinder_writer_add_cleanup(writer, close_request_fd, GINT_TO_POINTER(duplicated));
    return 0;
}

int anco_bms_payload_fd(const void *utf8, gsize length, int *out)
{
    if (!utf8 || !out) return -EINVAL;
    int error = validate_length(length);
    if (error) return error;
    if (!g_utf8_validate(utf8, (gssize)length, NULL)) return -EILSEQ;
    int fd = memfd_create("installInfos", MFD_CLOEXEC);
    if (fd < 0) return -errno;
    if (ftruncate(fd, (off_t)length)) {
        error = -errno;
        close(fd);
        return error;
    }
    const unsigned char *bytes = utf8;
    gsize written = 0;
    while (written < length) {
        ssize_t n = pwrite(fd, bytes + written, length - written, (off_t)written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            error = n < 0 ? -errno : -EIO;
            close(fd);
            return error;
        }
        written += (gsize)n;
    }
    *out = fd;
    return 0;
}

int anco_bms_install_append(GBinderLocalRequest *request, int fd, gsize length)
{
    if (!request || fd < 0) return -EINVAL;
    int error = validate_length(length);
    if (error) return error;
    struct stat st;
    if (fstat(fd, &st)) return -errno;
    if (st.st_size < 0 || (uint64_t)st.st_size < (uint64_t)length) return -ENODATA;
    const int mode = fcntl(fd, F_GETFL);
    if (mode < 0) return -errno;
    if ((mode & O_ACCMODE) == O_WRONLY) return -EBADF;
    GBinderWriter writer;
    gbinder_local_request_init_writer(request, &writer);
    error = append_dup_fd_checked(&writer, fd);
    if (error) return error;
    gbinder_writer_append_int32(&writer, (gint32)length);
    return 0;
}

static gboolean decode_install_reply(GBinderRemoteReply *reply,
                                       AncoBmsRegistrationResult *out)
{
    if (!reply || out->transport) return FALSE;
    GBinderReader reader;
    gbinder_remote_reply_init_reader(reply, &reader);
    out->decoded = gbinder_reader_read_int32(&reader, &out->service_result) &&
        gbinder_reader_at_end(&reader);
    return out->decoded && !out->service_result;
}

GBinderRemoteReply *anco_bms_query_bundle(GBinderRemoteObject *bms, const char *bundle,
    gint32 flags, gint32 host_user, AncoBmsRegistrationResult *out)
{
    if (!out) return NULL;
    *out = (AncoBmsRegistrationResult){0, -1, FALSE, -1};
    if (!bms || !bundle || !*bundle || host_user < 0) {
        out->local_error = -EINVAL; return NULL;
    }
    gsize length = strlen(bundle);
    if (length > G_MAXINT32 - 1 || !g_utf8_validate(bundle, -1, NULL)) {
        out->local_error = -EINVAL; return NULL;
    }
    GBinderRemoteReply *result = NULL;
    GBinderClient *client = gbinder_client_new(bms, ANCO_BMS_IFACE);
    GBinderLocalRequest *req = client ? gbinder_client_new_request(client) : NULL;
    if (!req) out->local_error = -ENOMEM;
    else {
        GBinderWriter writer;
        gbinder_local_request_init_writer(req, &writer);
        gbinder_writer_append_int32(&writer, (gint32)length);
        gbinder_writer_append_string8(&writer, bundle);
        gbinder_writer_append_int32(&writer, flags);
        gbinder_writer_append_int32(&writer, host_user);
        GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(client,
            ANCO_BMS_GET_BUNDLE_TRANSACTION, req, &out->transport);
        if (reply && !out->transport) {
            GBinderReader reader;
            gbinder_remote_reply_init_reader(reply, &reader);
            out->decoded = gbinder_reader_read_int32(&reader, &out->service_result);
            if (out->decoded) { result = reply; reply = NULL; }
        }
        if (reply) gbinder_remote_reply_unref(reply);
        gbinder_local_request_unref(req);
    }
    if (client) gbinder_client_unref(client);
    return result;
}

int anco_bms_uninstall_append(GBinderLocalRequest *request, const char *bundle,
                              gboolean with_user, gint32 user_id)
{
    if (!request || !bundle || !*bundle || (with_user && user_id < 0))
        return -EINVAL;
    const gsize length = strlen(bundle);
    if (length > G_MAXINT32 - 1) return -EMSGSIZE;
    if (!g_utf8_validate(bundle, (gssize)length, NULL)) return -EILSEQ;
    GBinderWriter writer;
    gbinder_local_request_init_writer(request, &writer);
    /* Retained libgbinder string8 writes NUL/padding but not the original
     * Parcel.WriteString/writeString8 int32 byte-length prefix. */
    gbinder_writer_append_int32(&writer, (gint32)length);
    gbinder_writer_append_string8(&writer, bundle);
    if (with_user) gbinder_writer_append_int32(&writer, user_id);
    gbinder_writer_append_bool(&writer, FALSE);
    return 0;
}

gboolean anco_bms_unregister(GBinderRemoteObject *bms, const char *bundle,
                             gboolean with_user, gint32 user_id,
                             AncoBmsRegistrationResult *out)
{
    if (!out) return FALSE;
    *out = (AncoBmsRegistrationResult){0, -1, FALSE, -1};
    if (!bms) { out->local_error = -ENOTCONN; return FALSE; }
    GBinderClient *client = gbinder_client_new(bms, ANCO_BMS_IFACE);
    GBinderLocalRequest *request = client ? gbinder_client_new_request(client) : NULL;
    if (!request) {
        out->local_error = -ENOMEM;
    } else {
        out->local_error = anco_bms_uninstall_append(request, bundle, with_user, user_id);
        if (!out->local_error) {
            const guint32 code = with_user ? ANCO_BMS_UNINSTALL_WITH_USER_TRANSACTION :
                ANCO_BMS_UNINSTALL_TRANSACTION;
            GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(
                client, code, request, &out->transport);
            decode_install_reply(reply, out);
            if (reply) gbinder_remote_reply_unref(reply);
        }
        gbinder_local_request_unref(request);
    }
    if (client) gbinder_client_unref(client);
    return !out->local_error && !out->transport && out->decoded && !out->service_result;
}

int anco_bms_update_status_append(GBinderLocalRequest *request,
                                  const char *bundle, gint32 status,
                                  gint32 guest_user_id)
{
    if (!request || !bundle || !*bundle || guest_user_id < 0) return -EINVAL;
    const gsize length = strlen(bundle);
    if (length > G_MAXINT32 - 1) return -EMSGSIZE;
    if (!g_utf8_validate(bundle, (gssize)length, NULL)) return -EILSEQ;
    GBinderWriter writer;
    gbinder_local_request_init_writer(request, &writer);
    gbinder_writer_append_int32(&writer, (gint32)length);
    gbinder_writer_append_string8(&writer, bundle);
    gbinder_writer_append_int32(&writer, status);
    gbinder_writer_append_int32(&writer, guest_user_id);
    return 0;
}

gboolean anco_bms_send_update_status(GBinderRemoteObject *bms,
                                     const char *bundle, gint32 status,
                                     gint32 guest_user_id,
                                     AncoBmsRegistrationResult *out)
{
    if (!out) return FALSE;
    *out = (AncoBmsRegistrationResult){0, -1, FALSE, -1};
    if (!bms) { out->local_error = -ENOTCONN; return FALSE; }
    GBinderClient *client = gbinder_client_new(bms, ANCO_BMS_IFACE);
    GBinderLocalRequest *request = client ? gbinder_client_new_request(client) : NULL;
    if (!request) {
        out->local_error = -ENOMEM;
    } else {
        out->local_error = anco_bms_update_status_append(request, bundle, status,
                                                          guest_user_id);
        if (!out->local_error) {
            GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(
                client, ANCO_BMS_UPDATE_STATUS_TRANSACTION, request,
                &out->transport);
            decode_install_reply(reply, out);
            if (reply) gbinder_remote_reply_unref(reply);
        }
        gbinder_local_request_unref(request);
    }
    if (client) gbinder_client_unref(client);
    return !out->local_error && !out->transport && out->decoded &&
        !out->service_result;
}

gboolean anco_bms_register(GBinderRemoteObject *bms, const void *utf8,
                           gsize length, AncoBmsRegistrationResult *out)
{
    if (!out) return FALSE;
    *out = (AncoBmsRegistrationResult){0, -1, FALSE, -1};
    if (!bms) { out->local_error = -ENOTCONN; return FALSE; }
    int fd = -1;
    out->local_error = anco_bms_payload_fd(utf8, length, &fd);
    if (out->local_error) return FALSE;
    GBinderClient *client = gbinder_client_new(bms, ANCO_BMS_IFACE);
    GBinderLocalRequest *request = client ? gbinder_client_new_request(client) : NULL;
    if (!request) {
        out->local_error = -ENOMEM;
    } else {
        out->local_error = anco_bms_install_append(request, fd, length);
        if (!out->local_error) {
            GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(
                client, ANCO_BMS_INSTALL_TRANSACTION, request, &out->transport);
            decode_install_reply(reply, out);
            if (reply) gbinder_remote_reply_unref(reply);
        }
        /* The request owns only a successful dup; a dup failure sends nothing.
         * Drop its reference before closing our separate primary FD. */
        gbinder_local_request_unref(request);
    }
    if (client) gbinder_client_unref(client);
    close(fd);
    return !out->local_error && !out->transport && out->decoded &&
        !out->service_result;
}
