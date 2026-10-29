/* Linux ownership bridge for ORIGINAL normal producer release bookkeeping. */
#ifndef ANCO_DELEGATOR_RELEASE_STATE_H
#define ANCO_DELEGATOR_RELEASE_STATE_H
#include "anco_delegator_release.h"

typedef struct AncoDelegatorReleaseState AncoDelegatorReleaseState;
/* The original callback carries two opaque uint64 values. Supply the actual
 * acquired-buffer/transaction identity, never a fabricated completion ID. */
typedef struct { guint64 first, second; } AncoDelegatorReleaseId;
typedef void (*AncoDelegatorBufferReleased)(const AncoDelegatorReleaseId *id,
    int owned_fence_fd, void *data);

/* Same serialized producer context as receiver and dequeue. The real consumer
 * takes ownership of positive fence_fd; -1 carries no descriptor. data must
 * survive ongoing callbacks. No threads, nested state dispatch or fake listener.
 * NULL listener models original absence; receiving code3 then returns -1.
 * These are Linux internal functions, not extra Binder messages/host interfaces.
 */
AncoDelegatorReleaseState *anco_delegator_release_state_new(
    AncoDelegatorBufferReleased actual_listener, void *data);
/* After an ACTUAL successful SendDequeuedBufferInfoLocked: insert registered
 * slot; discard old saved fence and pending release ID. Code0 reply10 is not a
 * transport error. Caller must distinguish real transport from missing reply;
 * this hook makes no IPC call and cannot decide whether code0 succeeded. */
void anco_delegator_release_state_dequeued(AncoDelegatorReleaseState *self,
    gint32 actual_slot);
/* Original successful full-handle code0 reply10 inserts converter-failed slot
 * BEFORE registration/old fence/pending cleanup. Other replies and no-handle
 * reuse leave this set unchanged, including after release. Not GPU readiness. */
void anco_delegator_release_state_dequeued_reply(AncoDelegatorReleaseState *self,
    gint32 actual_slot, gboolean actual_reply10);
gboolean anco_delegator_release_state_registered(AncoDelegatorReleaseState *self,
    gint32 actual_slot);
gboolean anco_delegator_release_state_conversion_failed(AncoDelegatorReleaseState *self,
    gint32 actual_slot);
/* Original ClearBufferSlot/Reset caller performs these local set operations;
 * no IPC, registered/pending/fence cleanup or complete Reset implied. */
void anco_delegator_release_state_clear_conversion(AncoDelegatorReleaseState *self,
    gint32 actual_slot);
void anco_delegator_release_state_reset_conversion(AncoDelegatorReleaseState *self);
/* ORIGINAL AddOhPendingRelease gates and return, for the normal path only.
 * Flags come from actual producer state (+1d0/+1d9), not defaults inferred here.
 * TRUE defers the real buffer item; FALSE lets its original release path use
 * take_fence. A return value is not GPU completion or permission to wl_release.
 */
gboolean anco_delegator_release_state_add_pending(AncoDelegatorReleaseState *self,
    gboolean actual_active, gboolean actual_destroyed,
    const AncoDelegatorReleaseId *actual_id, gint32 actual_slot);
/* Consumer of successfully decoded ORIGINAL code3 (or original no-peer queue
 * ReleaseBuffer(slot,-1)). Takes owned_fd only when stored/transferred; otherwise
 * caller still clears the decoded release. Includes raw signed slots as stock.
 * Duplicate saved fence keeps the first original value. The unused Linux-owned
 * duplicate remains with the caller for cleanup, not leaked or used as completion.
 */
int anco_delegator_release_state_consume(AncoDelegatorReleaseState *self,
    AncoDelegatorRelease *actual_release);
/* ORIGINAL GetReleaseFenceBySlot removes the entry and transfers the actual
 * positive owned FD; absent or saved -1 returns -1. Caller owns a returned FD.
 */
int anco_delegator_release_state_take_fence(AncoDelegatorReleaseState *self,
    gint32 actual_slot);
/* ORIGINAL ClearOhPendingRelease: snapshot IDs in signed slot order; call real
 * listener with -1. FALSE keeps listener; TRUE clears it AFTER notifications.
 * Original map is not erased here. No synthesized IDs or fence signaling.
 */
void anco_delegator_release_state_clear_pending(AncoDelegatorReleaseState *self,
    gboolean clear_listener);
/* ORIGINAL regReleaseCallback. Nonempty replacement leaves peer/maps alone.
 * Empty replacement closes positive cached fences and sets their values to -1
 * WITHOUT erasing keys, then invokes actual base SetClient(NULL). The required
 * clear operation is local ownership, not synthetic code5/12 or pending replay.
 * A missing clear operation for empty replacement returns -EINVAL unchanged. */
int anco_delegator_release_state_set_listener(AncoDelegatorReleaseState *self,
    AncoDelegatorBufferReleased actual_listener, void *data,
    void (*actual_clear_client)(void *), void *actual_client_owner);
/* Release-record portion of ORIGINAL Reset, not a complete producer reset:
 * notify pending with -1, clear listener, pending and saved fences; retain
 * registered slots and conversion-failed set (full Reset separately clears it).
 * Does not clear peer or pending first-frame/producer flags. */
void anco_delegator_release_state_reset_records(AncoDelegatorReleaseState *self);
/* No synthetic code12/callbacks. Releases all Linux-owned cached descriptors.
 * Safe from a callback; ongoing call retains self. No callback-data destructor.
 */
void anco_delegator_release_state_free(AncoDelegatorReleaseState *self);
#endif
