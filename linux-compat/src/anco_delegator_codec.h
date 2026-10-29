/* Linux encoding bridge for original ProducerSurfaceDelegator code0 body. */
#ifndef ANCO_DELEGATOR_CODEC_H
#define ANCO_DELEGATOR_CODEC_H
#include <gbinder.h>

#define ANCO_DELEGATOR_IFACE "surf.TransactSurfaceDelegator"
#define ANCO_DELEGATOR_DEQUEUE_TRANSACTION 0
#define ANCO_DELEGATOR_QUEUE_TRANSACTION 1
#define ANCO_DELEGATOR_DAMAGE_FEATURE 0x40u

/* The caller supplies an original request with its negotiated conditional
 * interface token already prepared. Append hasHandle,slot and (if supplied)
 * original native_handle version,numFds,numInts, dup FD objects, then ints.
 * handle_bytes includes the three int32 header words and its data words.
 * Input is borrowed for this call; successful request owns all duplicated
 * FDs (Android cookie1). Nothing is appended if validation/duplication fails.
 * NULL,0 means actual reuse of a previously registered slot, not a fake handle.
 * This does not send, negotiate, allocate GPU memory, pick a slot, cache buffers,
 * receive release callbacks, or prove a slot/Surface exists in the host.
 */
int anco_delegator_append_dequeue(GBinderLocalRequest *request, gint32 slot,
                                 const void *handle, gsize handle_bytes);
/* Original writes raw request int32, not a normalized presence bool. Nonzero
 * requires actual handle; flag0 ignores handle/bytes. Existing convenience API
 * keeps its full-handle/reuse behavior. No slot-selection policy here. */
int anco_delegator_append_dequeue_flag(GBinderLocalRequest *request,
    gint32 actual_request_flag, gint32 actual_slot,
    const void *actual_handle, gsize actual_handle_bytes);

/* Original code1 body: slot, negotiated optional damage bool/vector, then
 * optional actual acquire-fence FD object (Android cookie1). No hasFence field.
 * rect is Android left,top,right,bottom; required only with damage feature.
 * Negative fence means no FD bytes, as in the original negative-dup branch.
 * Actual FD duplication failure omits the FD object, as original Parcel does;
 * original QueueBuffer still sends the body. Diagnostic variant preserves the
 * negative errno separately. Raw signed slot is written without normalization.
 * This does not submit, wait, release, or mark a buffer/display frame complete.
 */
int anco_delegator_append_queue(GBinderLocalRequest *request, gint32 slot,
    guint32 actual_features, const gint32 rect[4], int acquire_fence_fd);
int anco_delegator_append_queue_diagnostic(GBinderLocalRequest *request, gint32 slot,
    guint32 actual_features, const gint32 rect[4], int acquire_fence_fd,
    int *actual_fence_dup_error);
/* Exact ORIGINAL UpdateLastBufferNoClientLocked normal cached code1 body:
 * slot and optional actual FD, WITHOUT ordinary queue's damage fields.
 * Does not change the negotiated feature mask/token. The retained SP68 host
 * damage reader can consume this differently; this is not verified delivery
 * of the fence, and must not be advertised as a completed replay/state machine.
 */
int anco_delegator_append_cached_queue(GBinderLocalRequest *request, gint32 slot,
    int acquire_fence_fd);
int anco_delegator_append_cached_queue_diagnostic(GBinderLocalRequest *request,
    gint32 slot, int acquire_fence_fd, int *actual_fence_dup_error);
#endif
