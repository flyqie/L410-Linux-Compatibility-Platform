#include "anco_delegator_receiver.h"
#include "gbinder_reader_p.h"
#include "gbinder_buffer_p.h"
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

struct AncoDelegatorReceiver {
    guint refs;
    gboolean closed;
    guint32 features;
    GBinderLocalObject *object;
    GBinderRemoteObject *client;
    AncoDelegatorReceiverOps ops;
    void *data;
};

/* Exact checked libgbinder reader.c prefix, not a host wire/interface change.
 * Without feature2 stock ignores interface headers and reads byte0. Generic
 * AIDL decoding can mistake raw code3 words for an empty header; retain the
 * actual reader's registry/object offsets and rebase its body to byte0. */
typedef struct {
    const guint8 *start, *end, *ptr;
    const GBinderReaderData *data;
} CheckedReaderPrefix;
G_STATIC_ASSERT(sizeof(GBinderReader) >= sizeof(CheckedReaderPrefix));
G_STATIC_ASSERT(offsetof(CheckedReaderPrefix, data) == 3 * sizeof(void *));

static void unref(AncoDelegatorReceiver *self)
{
    if (!--self->refs) g_free(self);
}

static void replace_client(AncoDelegatorReceiver *self, GBinderRemoteObject *peer)
{
    GBinderRemoteObject *old = self->client;
    self->client = gbinder_remote_object_ref(peer);
    gbinder_remote_object_unref(old);
}

static GBinderLocalReply *receive(GBinderLocalObject *object,
    GBinderRemoteRequest *request, guint code, guint flags, int *status, void *data)
{
    AncoDelegatorReceiver *self = data;
    (void)object; (void)flags;
    ++self->refs;
    *status = 0;
    GBinderReader observed;
    CheckedReaderPrefix header;
    gbinder_remote_request_init_reader(request, &observed);
    memcpy(&header, &observed, sizeof(header));
    const char *actual_interface = gbinder_remote_request_interface(request);
    printf("ANCO_ORIGINAL_CALLBACK code=%u flags=%u features=%u iface=%s size=%zu header=",
        code, flags, self->features, actual_interface ? actual_interface : "(null)",
        header.data && header.data->buffer ? header.data->buffer->size : 0);
    if (header.data && header.data->buffer) {
        const GBinderBuffer *buffer = header.data->buffer;
        for (gsize i = 0; i < MIN(buffer->size, (gsize)80); ++i)
            printf("%02x", ((const guint8 *)buffer->data)[i]);
    }
    putchar('\n');
    if (self->closed) {
        *status = -ESHUTDOWN;
    } else if ((self->features & 2) &&
        g_strcmp0(gbinder_remote_request_interface(request), ANCO_DELEGATOR_CALLBACK_IFACE)) {
        *status = -1; /* original checkInterface failure */
    } else if (code == ANCO_DELEGATOR_RELEASE_TRANSACTION ||
               code == ANCO_DELEGATOR_SET_CLIENT_TRANSACTION) {
        GBinderReader reader;
        gbinder_remote_request_init_reader(request, &reader);
        if (!(self->features & 2)) {
            CheckedReaderPrefix prefix;
            memcpy(&prefix, &reader, sizeof(prefix));
            if (prefix.data && prefix.data->buffer)
                gbinder_reader_init(&reader, (GBinderReaderData *)prefix.data,
                    0, prefix.data->buffer->size);
        }
        if (code == ANCO_DELEGATOR_RELEASE_TRANSACTION) {
            AncoDelegatorRelease release = ANCO_DELEGATOR_RELEASE_INIT;
            *status = anco_delegator_read_release(&reader, &release);
            if (!*status) *status = self->ops.release(&release, self->data);
            anco_delegator_release_clear(&release);
        } else {
            GBinderRemoteObject *peer = NULL;
            /* Stock OnSetClient ignores readStrongBinder's status and passes
             * its (possibly NULL) result to SetClient and pending replay. */
            gbinder_reader_read_nullable_object(&reader, &peer);
            printf("ANCO_ORIGINAL_SET_CLIENT receiver=%p code=%u peer=%p features=%u\n",
                (void *)self,code,(void *)peer,self->features);
            replace_client(self, peer);
            self->ops.set_client(peer, self->data);
            gbinder_remote_object_unref(peer);
        }
    } else if (code == ANCO_DELEGATOR_DESTROYED_TRANSACTION) {
        self->ops.mark_destroyed(self->data);
        if (!self->closed) {
            replace_client(self, NULL);
            if (!self->closed) self->ops.destroyed(FALSE, self->data);
        }
    }
    /* Original code5/12 and ordinary unknown codes write no application reply.
     * libgbinder emits status only for synchronous calls; ONE WAY has no reply. */
    unref(self);
    return NULL;
}

AncoDelegatorReceiver *anco_delegator_receiver_new(GBinderIpc *ipc,
    guint32 features, const AncoDelegatorReceiverOps *ops, void *data)
{
    if (!ipc || !ops || !ops->release || !ops->set_client || !ops->destroyed || !ops->mark_destroyed)
        return NULL;
    AncoDelegatorReceiver *self = g_new0(AncoDelegatorReceiver, 1);
    self->refs = 1;
    self->features = features;
    self->ops = *ops;
    self->data = data;
    const char *ifaces[] = {ANCO_DELEGATOR_IFACE, NULL};
    self->object = gbinder_local_object_new(ipc, ifaces, receive, self);
    if (!self->object) { g_free(self); return NULL; }
    return self;
}

GBinderLocalObject *anco_delegator_receiver_ref_object(AncoDelegatorReceiver *self)
{ return self && !self->closed ? gbinder_local_object_ref(self->object) : NULL; }

GBinderRemoteObject *anco_delegator_receiver_ref_client(AncoDelegatorReceiver *self)
{ return self && !self->closed ? gbinder_remote_object_ref(self->client) : NULL; }

void anco_delegator_receiver_clear_client(AncoDelegatorReceiver *self)
{
    if (self && !self->closed) replace_client(self, NULL);
}

void anco_delegator_receiver_free(AncoDelegatorReceiver *self)
{
    if (self && !self->closed) {
        self->closed = TRUE;
        gbinder_local_object_drop(self->object);
        self->object = NULL;
        replace_client(self, NULL);
        unref(self);
    }
}
