#include "anco_delegator_release_state.h"
#include <errno.h>
#include <unistd.h>

struct AncoDelegatorReleaseState {
    guint refs;
    gboolean closed;
    GHashTable *registered, *pending, *fences, *conversion_failed;
    AncoDelegatorBufferReleased listener;
    void *data;
};

static void fence_free(gpointer value)
{
    int *fd = value;
    if (*fd >= 0) close(*fd);
    g_free(fd);
}

static void unref(AncoDelegatorReleaseState *self)
{
    if (!--self->refs) {
        g_hash_table_destroy(self->registered);
        g_hash_table_destroy(self->pending);
        g_hash_table_destroy(self->fences);
        g_hash_table_destroy(self->conversion_failed);
        g_free(self);
    }
}

AncoDelegatorReleaseState *anco_delegator_release_state_new(
    AncoDelegatorBufferReleased listener, void *data)
{
    AncoDelegatorReleaseState *self = g_new0(AncoDelegatorReleaseState, 1);
    self->refs = 1;
    self->registered = g_hash_table_new(g_direct_hash, g_direct_equal);
    self->conversion_failed = g_hash_table_new(g_direct_hash, g_direct_equal);
    self->pending = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    self->fences = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, fence_free);
    self->listener = listener;
    self->data = data;
    return self;
}

void anco_delegator_release_state_dequeued(AncoDelegatorReleaseState *self,
    gint32 slot)
{
    if (self && !self->closed) {
        gpointer key = GINT_TO_POINTER(slot);
        g_hash_table_add(self->registered, key);
        g_hash_table_remove(self->fences, key);
        g_hash_table_remove(self->pending, key);
    }
}

void anco_delegator_release_state_dequeued_reply(AncoDelegatorReleaseState *self,
    gint32 slot,gboolean reply10)
{
    if (self && !self->closed) {
        if (reply10) g_hash_table_add(self->conversion_failed,GINT_TO_POINTER(slot));
        anco_delegator_release_state_dequeued(self,slot);
    }
}
gboolean anco_delegator_release_state_registered(AncoDelegatorReleaseState *self,gint32 slot)
{return self&&!self->closed&&g_hash_table_contains(self->registered,GINT_TO_POINTER(slot));}
gboolean anco_delegator_release_state_conversion_failed(AncoDelegatorReleaseState *self,gint32 slot)
{return self&&!self->closed&&g_hash_table_contains(self->conversion_failed,GINT_TO_POINTER(slot));}
void anco_delegator_release_state_clear_conversion(AncoDelegatorReleaseState *self,gint32 slot)
{if(self&&!self->closed)g_hash_table_remove(self->conversion_failed,GINT_TO_POINTER(slot));}
void anco_delegator_release_state_reset_conversion(AncoDelegatorReleaseState *self)
{if(self&&!self->closed)g_hash_table_remove_all(self->conversion_failed);}

gboolean anco_delegator_release_state_add_pending(AncoDelegatorReleaseState *self,
    gboolean active, gboolean destroyed, const AncoDelegatorReleaseId *id, gint32 slot)
{
    if (!self || self->closed || !id || !active || destroyed) return FALSE;
    gpointer key = GINT_TO_POINTER(slot);
    if (!g_hash_table_contains(self->registered, key))
        return g_hash_table_contains(self->pending, key);
    if (g_hash_table_contains(self->fences, key)) return FALSE;
    AncoDelegatorReleaseId *copy = g_new(AncoDelegatorReleaseId, 1);
    *copy = *id;
    g_hash_table_replace(self->pending, key, copy);
    return TRUE;
}

int anco_delegator_release_state_consume(AncoDelegatorReleaseState *self,
    AncoDelegatorRelease *release)
{
    if (!self || !release) return -EINVAL;
    if (self->closed) return -ESHUTDOWN;
    if (!self->listener) return -1;
    if (release->fence_fd == 0 ||
        (release->fence_fd > 0 && release->fence_fd != release->owned_fd))
        return -EINVAL; /* not a decoded/transferred owned fence */
    gpointer key = GINT_TO_POINTER(release->slot);
    const AncoDelegatorReleaseId *pending = g_hash_table_lookup(self->pending, key);
    g_hash_table_remove(self->registered, key);
    if (pending) {
        const AncoDelegatorReleaseId id = *pending;
        const int fence = release->fence_fd;
        if (fence > 0) release->owned_fd = -1;
        ++self->refs;
        self->listener(&id, fence, self->data);
        g_hash_table_remove(self->pending, key);
        unref(self);
    } else if (!g_hash_table_contains(self->fences, key)) {
        int *fence = g_new(int, 1);
        *fence = release->fence_fd;
        if (*fence > 0) release->owned_fd = -1;
        g_hash_table_insert(self->fences, key, fence);
    }
    return 0;
}

int anco_delegator_release_state_take_fence(AncoDelegatorReleaseState *self,
    gint32 slot)
{
    if (!self || self->closed) return -1;
    gpointer key = GINT_TO_POINTER(slot);
    int *saved = g_hash_table_lookup(self->fences, key);
    if (!saved) return -1;
    const int fd = *saved;
    *saved = -1;
    g_hash_table_remove(self->fences, key);
    return fd;
}

typedef struct { gint32 slot; AncoDelegatorReleaseId id; } PendingCopy;
static gint compare_slot(gconstpointer first, gconstpointer second)
{
    const PendingCopy *a = first, *b = second;
    return (a->slot > b->slot) - (a->slot < b->slot);
}

void anco_delegator_release_state_clear_pending(AncoDelegatorReleaseState *self,
    gboolean clear_listener)
{
    if (!self || self->closed) return;
    ++self->refs;
    GArray *snapshot = g_array_new(FALSE, FALSE, sizeof(PendingCopy));
    GHashTableIter iterator;
    gpointer key, value;
    g_hash_table_iter_init(&iterator, self->pending);
    while (g_hash_table_iter_next(&iterator, &key, &value)) {
        const PendingCopy copy = {GPOINTER_TO_INT(key), *(AncoDelegatorReleaseId *)value};
        g_array_append_val(snapshot, copy);
    }
    g_array_sort(snapshot, compare_slot);
    for (guint i = 0; i < snapshot->len && !self->closed && self->listener; ++i)
        self->listener(&g_array_index(snapshot, PendingCopy, i).id, -1, self->data);
    if (clear_listener) self->listener = NULL;
    g_array_free(snapshot, TRUE);
    unref(self);
}

int anco_delegator_release_state_set_listener(AncoDelegatorReleaseState *self,
    AncoDelegatorBufferReleased listener, void *data,
    void (*clear_client)(void *), void *client_owner)
{
    if (!self || (!listener && !clear_client)) return -EINVAL;
    if (self->closed) return -ESHUTDOWN;
    ++self->refs;
    self->listener = listener;
    self->data = data;
    if (!listener) {
        GHashTableIter iterator;
        gpointer value;
        g_hash_table_iter_init(&iterator, self->fences);
        while (g_hash_table_iter_next(&iterator, NULL, &value)) {
            int *fd = value;
            if (*fd > 0) close(*fd);
            *fd = -1;
        }
        clear_client(client_owner);
    }
    unref(self);
    return 0;
}

void anco_delegator_release_state_reset_records(AncoDelegatorReleaseState *self)
{
    if (!self || self->closed) return;
    ++self->refs;
    anco_delegator_release_state_clear_pending(self, TRUE);
    g_hash_table_remove_all(self->pending);
    g_hash_table_remove_all(self->fences);
    unref(self);
}

void anco_delegator_release_state_free(AncoDelegatorReleaseState *self)
{
    if (self && !self->closed) {
        self->closed = TRUE;
        self->listener = NULL;
        unref(self);
    }
}
