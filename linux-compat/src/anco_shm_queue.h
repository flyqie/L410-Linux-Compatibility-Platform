/* Actual Linux SHM client buffer -> ORIGINAL gralloc and AnCo normal queue.
 * This CPU upload is not capture, a video transport, or GPU rendering proof. */
#ifndef ANCO_SHM_QUEUE_H
#define ANCO_SHM_QUEUE_H
#include "anco_delegator_producer.h"
struct wlr_buffer;
typedef struct AncoShmQueue AncoShmQueue;
typedef struct {
    /* Physical buffer-pixel margins. The compositor has already negotiated
     * SERVER decoration and converted its logical frame to this buffer's
     * sampling density. The title is borrowed for the synchronous submit. */
    guint32 left, top, right, bottom;
    const char *title;
} AncoShmDecoration;
/* Holds one producer reference. The callback only schedules serialized work
 * after real SetClient or actual release/fence availability; no polling. */
AncoShmQueue *anco_shm_queue_new(AncoDelegatorProducer *producer,
    void (*changed)(void *),void *data);
/* Borrows a locked application buffer. On success the pixels have been read
 * and copied into a real gralloc allocation, code0/1 sent. Source may then be
 * unlocked; native allocation cannot be rewritten until code3 AND its fence.
 * EAGAIN: no actual peer, ENOBUFS: no free original slot. Other negative errors
 * are diagnostics, not successful submission. Only normal transform is
 * currently admitted by caller. No presentation/completion callback here. */
int anco_shm_queue_submit(AncoShmQueue *self,struct wlr_buffer *buffer,
    guint32 actual_commit_sequence,const AncoShmDecoration *decoration);
typedef struct {
    const void *handle;
    size_t handle_bytes;
    guint32 width, height, sequence;
} AncoShmSubmission;
/* Last successfully delivered ORIGINAL queue submission. The handle is the
 * same versioned transport handle used by dequeue, with real allocation FDs.
 * Borrowed until the next submit or free; no extra FD ownership, pixel read,
 * buffer-release, or presentation assertion. ENODATA before real submission.
 * The caller must pair sequence with that commit's geometry/configuration. */
int anco_shm_queue_submission(AncoShmQueue *self,AncoShmSubmission *out);
/* Drop Linux ownership after original window removal. Original host imports
 * hold their independent FD/strong references. This does not assert GPU idle
 * or turn pending retirement callbacks into frame completion. */
void anco_shm_queue_free(AncoShmQueue *self);
#endif
