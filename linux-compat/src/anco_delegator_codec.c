#define _GNU_SOURCE
#include "anco_delegator_codec.h"
#include "gbinder_writer_p.h"
#include "gbinder_io.h"
#include <gutil_intarray.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static void close_owned_fd(gpointer pointer)
{
    close(GPOINTER_TO_INT(pointer));
}

int anco_delegator_append_dequeue_flag(GBinderLocalRequest *request,
    gint32 request_flag, gint32 slot, const void *handle, gsize handle_bytes)
{
    if (!request || (guint32)slot >= 64 || (request_flag && !handle))
        return -EINVAL;
    if (!request_flag) {handle=NULL;handle_bytes=0;}
    gint32 header[3] = {0};
    const unsigned char *native = handle;
    if (handle) {
        if (handle_bytes < sizeof(header)) return -EMSGSIZE;
        memcpy(header, native, sizeof(header));
        /* Bounds are the original host ReadNativeHandle acceptance bounds.
         * Preserve version and metadata; converter compatibility is separate. */
        if ((guint32)header[1] > 16 || (guint32)header[2] > 256)
            return -EINVAL;
        const gsize required = sizeof(header) + 4u * (header[1] + header[2]);
        if (handle_bytes != required) return -EMSGSIZE;
    }
    GBinderWriter writer;
    gbinder_local_request_init_writer(request, &writer);
    GBinderWriterData *data;
    /* Retained private Writer prefix contains WriterData*, then offset. */
    memcpy(&data, writer.d, sizeof(data));
    if (!data || data->io != &gbinder_io_64) return -ENOTSUP;
    const guint body = 8u + (handle ? 12u + 24u * header[1] + 4u * header[2] : 0u);
    if (data->bytes->len > G_MAXUINT - body) return -EOVERFLOW;
    gint32 duplicates[16];
    guint count = 0;
    guint32 objects[16][6];
    for (gint32 i = 0; i < header[1]; ++i) {
        gint32 fd;
        memcpy(&fd, native + 12 + 4u * i, sizeof(fd));
        const int duplicated = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        if (duplicated < 0) {
            const int error = -errno;
            while (count) close(duplicates[--count]);
            return error;
        }
        duplicates[count++] = duplicated;
        if (data->io->encode_fd_object(objects[i], duplicated) != 24) {
            while (count) close(duplicates[--count]);
            return -ENOTSUP;
        }
        /* Original Android writeDupFileDescriptor flat object cookie is 1.
         * The OHOS BMS cookie0 convention must not leak into this protocol. */
        const guint64 cookie = 1;
        memcpy((unsigned char *)objects[i] + 16, &cookie, sizeof(cookie));
    }
    gbinder_writer_append_int32(&writer, request_flag);
    gbinder_writer_append_int32(&writer, slot);
    if (handle) {
        gbinder_writer_append_bytes(&writer, header, sizeof(header));
        for (guint i = 0; i < count; ++i) {
            const guint offset = data->bytes->len;
            gbinder_writer_append_bytes(&writer, objects[i], sizeof(objects[i]));
            if (!data->offsets) data->offsets = gutil_int_array_new();
            gutil_int_array_append(data->offsets, offset);
            gbinder_writer_add_cleanup(&writer, close_owned_fd,
                                       GINT_TO_POINTER(duplicates[i]));
        }
        if (header[2]) gbinder_writer_append_bytes(&writer,
            native + 12 + 4u * header[1], 4u * header[2]);
    }
    return 0;
}

int anco_delegator_append_dequeue(GBinderLocalRequest *request,gint32 slot,
    const void *handle,gsize handle_bytes)
{
    if (!handle && handle_bytes) return -EINVAL;
    return anco_delegator_append_dequeue_flag(request,handle!=NULL,slot,handle,handle_bytes);
}

static int append_queue_body(GBinderLocalRequest *request, gint32 slot,
    gboolean damage, const gint32 rect[4], int acquire_fence_fd, int *dup_error)
{
    if (dup_error) *dup_error=0;
    if (!request || (damage && !rect)) return -EINVAL;
    gint32 bounds[4];
    gboolean valid = FALSE;
    if (damage) {
        memcpy(bounds, rect, sizeof(bounds));
        /* Original Rect validity includes equality. It is not a positive-area
         * check, and guest left/top/right/bottom is not host x/y/w/h. */
        valid = bounds[2] >= bounds[0] && bounds[3] >= bounds[1];
    }
    GBinderWriter writer;
    gbinder_local_request_init_writer(request, &writer);
    GBinderWriterData *data;
    memcpy(&data, writer.d, sizeof(data));
    if (!data || data->io != &gbinder_io_64) return -ENOTSUP;
    const guint body = 4u + (damage ? 4u + (valid ? 20u : 0u) : 0u) +
        (acquire_fence_fd >= 0 ? 24u : 0u);
    if (data->bytes->len > G_MAXUINT - body) return -EOVERFLOW;
    int duplicated = -1;
    guint32 object[6];
    if (acquire_fence_fd >= 0) {
        duplicated = fcntl(acquire_fence_fd, F_DUPFD_CLOEXEC, 0);
        if (duplicated < 0) {
            /* Original writeDupFileDescriptor returns before appending; both
             * QueueBuffer callers log but continue notification/SendMessage. */
            if (dup_error) *dup_error=-errno;
        } else if (data->io->encode_fd_object(object, duplicated) != sizeof(object)) {
            close(duplicated);
            return -ENOTSUP;
        }
        if (duplicated >= 0) {
            const guint64 cookie = 1;
            memcpy((unsigned char *)object + 16, &cookie, sizeof(cookie));
        }
    }
    gbinder_writer_append_int32(&writer, slot);
    if (damage) {
        /* Original Android writeBool uses writeAligned<int32>. */
        gbinder_writer_append_int32(&writer, valid);
        if (valid) {
            gbinder_writer_append_int32(&writer, 4);
            gbinder_writer_append_bytes(&writer, bounds, sizeof(bounds));
        }
    }
    if (duplicated >= 0) {
        const guint offset = data->bytes->len;
        gbinder_writer_append_bytes(&writer, object, sizeof(object));
        if (!data->offsets) data->offsets = gutil_int_array_new();
        gutil_int_array_append(data->offsets, offset);
        gbinder_writer_add_cleanup(&writer, close_owned_fd, GINT_TO_POINTER(duplicated));
    }
    return 0;
}

int anco_delegator_append_queue(GBinderLocalRequest *request, gint32 slot,
    guint32 actual_features, const gint32 rect[4], int acquire_fence_fd)
{
    return append_queue_body(request, slot,
        (actual_features & ANCO_DELEGATOR_DAMAGE_FEATURE) != 0, rect, acquire_fence_fd, NULL);
}

int anco_delegator_append_queue_diagnostic(GBinderLocalRequest *request,gint32 slot,
    guint32 features,const gint32 rect[4],int fence,int *dup_error)
{
    return append_queue_body(request,slot,
        (features & ANCO_DELEGATOR_DAMAGE_FEATURE)!=0,rect,fence,dup_error);
}

int anco_delegator_append_cached_queue(GBinderLocalRequest *request, gint32 slot,
    int acquire_fence_fd)
{
    return append_queue_body(request, slot, FALSE, NULL, acquire_fence_fd, NULL);
}

int anco_delegator_append_cached_queue_diagnostic(GBinderLocalRequest *request,
    gint32 slot,int fence,int *dup_error)
{return append_queue_body(request,slot,FALSE,NULL,fence,dup_error);}
