/* Linux ownership bridge for ORIGINAL normal no-client pending frame. */
#ifndef ANCO_DELEGATOR_PENDING_FRAME_H
#define ANCO_DELEGATOR_PENDING_FRAME_H
#include "anco_delegator_metadata_state.h"
#include "anco_delegator_release_state.h"
typedef struct AncoDelegatorPendingFrame AncoDelegatorPendingFrame;
typedef struct {
    gint32 request_flag, slot, original_unused; /* original DequeueBuffer ints */
    const void *handle;gsize handle_bytes;
} AncoDelegatorDequeueCall;
typedef struct {
    gboolean shared_skipped,pending_cached;
    int cache_error;
    gint32 request_flag;
    AncoDelegatorMetadataReplay metadata;
    AncoDelegatorResult wire;
} AncoDelegatorDequeueReport;
/* Original normal DequeueBuffer outer call. Borrowed actual producer states,
 * peer and input remain alive/nonaliasing on the serialized producer context.
 * Shared mode returns0 with no action; no peer caches clone/slot and returns
 * original0, keeping cache_error separate. With peer set actual_active=TRUE,
 * replay dirty metadata, force flag1 only if actual slot is unregistered,
 * preserve caller's raw flag otherwise, send code0 then record actual reply10.
 * original_unused is genuinely unused by the retained original method.
 * Does not allocate slots/GPU buffers, pair a peer, queue, signal or claim ready.
 */
int anco_delegator_normal_dequeue(AncoDelegatorPendingFrame *pending,
    AncoDelegatorMetadataState *metadata,AncoDelegatorReleaseState *records,
    gboolean actual_shared_mode,gboolean *actual_active,
    GBinderRemoteObject *actual_peer,guint32 actual_features,
    const AncoDelegatorDequeueCall *actual_call,AncoDelegatorDequeueReport *report);

typedef struct {
    gint32 slot;
    const gint32 *rect; /* actual Android L/T/R/B, borrowed */
    int acquire_fence_fd;
} AncoDelegatorQueueCall;
typedef struct {
    gboolean pending_cached,release_called,notify_attempted;
    int release_status,notify_status,fence_dup_error;
    AncoDelegatorResult wire;
} AncoDelegatorQueueReport;
/* Original normal QueueBuffer outer call. No peer: transfer actual owned FD
 * into pending cache, then call actual release records with (slot,-1) and
 * return ReleaseBuffer status. Shared WITH peer releases without caching; shared
 * WITHOUT peer still caches as stock. Neither sends or notifies in that branch.
 * Ordinary peer branch borrows the input FD, encodes damage/FD then notifies
 * actual Task before code1. Dup failure records errno but still sends, exactly
 * as stock; neither notification nor transport status proves GPU completion.
 * Actual producer states, peer and callbacks survive the serialized call with
 * no nested state dispatch; caller retains all input ownership on validation
 * failure. No wl_buffer.release synthesis or slot/handle allocation here.
 */
int anco_delegator_normal_queue(AncoDelegatorPendingFrame *pending,
    AncoDelegatorReleaseState *records,gboolean actual_shared_mode,
    GBinderRemoteObject *actual_peer,guint32 actual_features,
    const AncoDelegatorQueueCall *actual_call,gint32 actual_task,
    gboolean *actual_task_notify_pending,int (*actual_notify)(gint32,void *),
    void *actual_notify_data,AncoDelegatorQueueReport *report);
typedef struct {
    AncoDelegatorMetadataReplay metadata;
    gboolean dequeue_attempted, queue_attempted, notify_attempted;
    AncoDelegatorResult dequeue, queue;
    int notify_status;
    int fence_dup_error;
} AncoDelegatorPendingReplay;

AncoDelegatorPendingFrame *anco_delegator_pending_frame_new(void);
/* Original DelegatorController.Remove clears the retained clone for every
 * found entry BEFORE decrementing its use count, even when users remain.
 * This operation drops only the locally owned clone: preserve pending flags,
 * slot and cached acquire fence. It is not Reset/free, a release notification,
 * peer removal, or permission to reuse an application buffer. Caller supplies
 * the original controller trigger on the same serialized producer context.
 */
void anco_delegator_pending_frame_clear_handle(AncoDelegatorPendingFrame *self);
/* Actual no-peer Dequeue caller only, not a complete Dequeue implementation:
 * set pending-dequeue, clear pending-queue, save raw slot, discard old clone,
 * clone the real native_handle. Original clone accepts counts0..1024. Return
 * is a Linux clone diagnostic; original outer Dequeue itself returns0 here.
 * Input bytes/FDs remain borrowed; all real dup copies belong to this owner.
 * No active flag, slot registration, buffer acquisition or fake identity here.
 */
int anco_delegator_pending_frame_cache_dequeue(AncoDelegatorPendingFrame *self,
    gint32 actual_slot, const void *actual_handle, gsize actual_handle_bytes);
/* Actual no-peer Queue caller only. Transfer the ACTUAL owned acquire FD.
 * Caller must separately perform original ReleaseBuffer(queue_slot,-1), not
 * synthesize wl_buffer.release. This stores pending-queue/fence, not queue slot.
 * Linux-owned replaced fence is closed, never signaled or used as completion.
 */
int anco_delegator_pending_frame_cache_queue_owned(AncoDelegatorPendingFrame *self,
    int actual_owned_acquire_fd);
/* Normal OnSetClient consumer portion after actual strong-peer replacement.
 * Same serialized context, no nested state dispatch. Required borrowed state,
 * peer and callback data survive the call. notify callback must perform ORIGINAL
 * NotifyTaskStarted on the actual task and return its actual status. It is only
 * required when the real pending flag and task != -1 make that call necessary.
 * Metadata replay runs even without a cached frame, then pending fields are
 * moved/cleared before IPC. Actual code0 failure skips queue; success registers
 * actual slot and converter-failed set for actual full-handle reply10.
 * Missing reply is reported, not invented; original readInt32 defaults still
 * leave transport-success path. Notify failure retains flag but does not skip
 * queue. No NDK support, ready receipt, feature defaults or retries here.
 * Retained cached-fence parsing discrepancy remains unverified on device.
 */
int anco_delegator_pending_frame_replay(AncoDelegatorPendingFrame *self,
    AncoDelegatorMetadataState *actual_metadata,
    AncoDelegatorReleaseState *actual_release_records,
    GBinderRemoteObject *actual_peer, guint32 actual_features,
    gint32 actual_task, gboolean *actual_task_notify_pending,
    int (*actual_notify_task_started)(gint32, void *), void *actual_notify_data,
    AncoDelegatorPendingReplay *report);
/* Local owner teardown only, not original Reset (which preserves pending frame
 * fields). Closes all Linux-owned clone/fence FDs including0, no notifications.
 */
void anco_delegator_pending_frame_free(AncoDelegatorPendingFrame *self);
#endif
