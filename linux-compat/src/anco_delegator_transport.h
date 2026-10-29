/* Single original ProducerSurfaceDelegator transactions, not its state machine. */
#ifndef ANCO_DELEGATOR_TRANSPORT_H
#define ANCO_DELEGATOR_TRANSPORT_H
#include "anco_delegator_codec.h"

typedef struct {
    int local_error;
    int transport;
    gboolean reply_decoded;
    gint32 reply_value;
} AncoDelegatorResult;

/* Borrow the already paired ORIGINAL host remote object and actual negotiated
 * feature mask. Only feature2 controls the original interface-token prefix.
 * TRUE means local encoding and synchronous transport succeeded, not dequeue,
 * queue, visibility, fence signaling, GPU completion or presentation success.
 * Only full-handle code0 reads reply int (10 is meaningful original slot state).
 * No-handle code0 and code1 do not interpret reply payload, as in original code.
 * Missing/truncated full-handle reply is explicitly reply_decoded=FALSE.
 * No object discovery, retries, synthetic IDs, slot cache or GUI caller here.
 */
gboolean anco_delegator_dequeue_once(GBinderRemoteObject *paired_host,
    guint32 actual_features, gint32 slot, const void *handle, gsize handle_bytes,
    AncoDelegatorResult *out);
/* Same original code0 with actual raw request flag. Nonzero reads reply int;
 * flag0 ignores handle/bytes and reply. Caller owns slot/converter state. */
gboolean anco_delegator_dequeue_flag_once(GBinderRemoteObject *paired_host,
    guint32 actual_features, gint32 actual_request_flag, gint32 actual_slot,
    const void *actual_handle, gsize actual_handle_bytes, AncoDelegatorResult *out);
gboolean anco_delegator_queue_once(GBinderRemoteObject *paired_host,
    guint32 actual_features, gint32 slot, const gint32 rect[4], int acquire_fence_fd,
    AncoDelegatorResult *out);
/* Linux internal original NotifyTaskStart call, not a new host callback.
 * Borrowed actual state/data survive the serialized queue operation without
 * nested state dispatch. Missing required callback is rejected before encoding.
 * Notify only AFTER actual request encoding, BEFORE code1. Failure keeps the
 * flag and does not skip queue; original proxy return0 clears it, even if its
 * raw delivery diagnosis failed. attempted means leaf call, not host readiness.
 */
typedef struct {
    gint32 task;
    gboolean *pending;
    int (*notify)(gint32,void *);
    void *data;
    gboolean attempted;
    int status;
} AncoDelegatorTaskNotify;
gboolean anco_delegator_queue_notify_once(GBinderRemoteObject *paired_host,
    guint32 actual_features,gint32 actual_slot,const gint32 actual_rect[4],
    int actual_acquire_fence_fd,gboolean actual_cached,
    AncoDelegatorTaskNotify *actual_notify,int *actual_fence_dup_error,
    AncoDelegatorResult *out);
/* Exact ORIGINAL normal cached code1 leaf, distinct from ordinary queue.
 * Actual features still choose token; no damage fields and no reply read.
 * Caller must own the real pending clone/slot/fence, satisfy actual dequeue
 * status and notify-task gates. This performs none of those state operations.
 * Retained host/guest cached-fence format discrepancy remains unverified. */
gboolean anco_delegator_cached_queue_once(GBinderRemoteObject *paired_host,
    guint32 actual_features, gint32 actual_slot, int actual_acquire_fence_fd,
    AncoDelegatorResult *out);
/* ORIGINAL ClearBufferSlot(code4) and ClearAllBuffers(code100): flags0,
 * conditional feature2 token; raw signed slot or empty body, no application
 * reply read. Real caller handles original shared-mode skip. These senders do
 * not alter local registered/pending/fence state, fake releases, retry, or
 * establish that actual host buffers/processes/mounts have been cleaned. */
gboolean anco_delegator_clear_slot_once(GBinderRemoteObject *paired_host,
    guint32 actual_features, gint32 actual_slot, AncoDelegatorResult *out);
gboolean anco_delegator_clear_all_once(GBinderRemoteObject *paired_host,
    guint32 actual_features, AncoDelegatorResult *out);
#endif
