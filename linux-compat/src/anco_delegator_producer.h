/* Common Linux owner of ORIGINAL normal producer state and receiver lifetime. */
#ifndef ANCO_DELEGATOR_PRODUCER_H
#define ANCO_DELEGATOR_PRODUCER_H
#include "anco_delegator_pending_frame.h"
#include "anco_delegator_receiver.h"
#include "anco_graphic_transport.h"

typedef struct AncoDelegatorProducer AncoDelegatorProducer;
typedef struct {
    gint32 task;
    guint32 features;
    gboolean active,notify_pending,destroyed,shared;
    AncoDelegatorMetadataState metadata;
} AncoDelegatorProducerSnapshot;
/* Prepared actual host IPC remains borrowed/alive until owner is freed and
 * ongoing calls return. Actual negotiated features and real mapped Task are
 * supplied by caller; no identity/default feature allocation or host bootstrap.
 * Original Create starts with no release listener, no peer, metadata INIT,
 * active/destroyed/shared FALSE and notify TRUE. Real local Binder is registered.
 * Common serialized IPC/Wayland context, no nested state dispatch; borrowed
 * callback data remain valid until calls return. Local free is safe in listener.
 * Normal mode only; no NDK support or final Wayland/native-buffer allocator.
 */
AncoDelegatorProducer *anco_delegator_producer_new(GBinderIpc *actual_ipc,
    guint32 actual_features,gint32 actual_task);
/* Linux-local strong ownership for original registry/core/temporary-getter
 * lifetimes. new returns one reference. ref returns another or NULL if retired;
 * unref consumes one valid owned reference, keeping the receiver operational
 * while any reference or ongoing callback remains. Same serialized context.
 * Last-reference disposal performs local retirement only, not host removal.
 * Caller must retain an owner for every live original binding/callback route;
 * an exported Binder object reference is NOT a producer ownership reference.
 * No original registry policy, automatic remote-reference mirroring or GPU
 * completion is inferred. No access after giving away the final reference.
 */
AncoDelegatorProducer *anco_delegator_producer_ref(AncoDelegatorProducer *self);
void anco_delegator_producer_unref(AncoDelegatorProducer *self);
/* Original registry Remove's clone-only cleanup, before count decrement.
 * No Reset, fence/cache flag clearing, IPC or release notification. */
int anco_delegator_producer_clear_pending_handle(AncoDelegatorProducer *self);
GBinderLocalObject *anco_delegator_producer_ref_object(AncoDelegatorProducer *self);
GBinderRemoteObject *anco_delegator_producer_ref_client(AncoDelegatorProducer *self);
/* Linux owner notification after an ACTUAL original SetClient and its stock
 * pending replay. Not a host protocol/callback or readiness assertion. Same
 * serialized context; callback must only schedule work, not reenter IPC. */
int anco_delegator_producer_observe_client(AncoDelegatorProducer *self,
    void (*changed)(void *), void *data);
int anco_delegator_producer_snapshot(AncoDelegatorProducer *self,
    AncoDelegatorProducerSnapshot *out);
int anco_delegator_producer_set_listener(AncoDelegatorProducer *self,
    AncoDelegatorBufferReleased actual_listener,void *actual_data);
int anco_delegator_producer_set_shared(AncoDelegatorProducer *self,gboolean actual_shared);
/* Original public metadata calls, in the same serialized owner context.
 * Android-domain values remain caller supplied. Void stock setters expose only
 * a Linux diagnostic report; this is not a new host return or ready signal.
 * Dirty/max/skip rules and 9/8/10 messages use the existing original state code.
 */
void anco_delegator_producer_set_transform(AncoDelegatorProducer *self,
    guint32 actual_transform,AncoDelegatorMetadataUpdate *out);
int anco_delegator_producer_set_queue_size(AncoDelegatorProducer *self,
    gint32 actual_size,AncoDelegatorMetadataUpdate *out);
void anco_delegator_producer_set_dataspace(AncoDelegatorProducer *self,
    guint32 actual_dataspace,AncoDelegatorMetadataUpdate *out);
int anco_delegator_producer_dequeue(AncoDelegatorProducer *self,
    const AncoDelegatorDequeueCall *actual_call,AncoDelegatorDequeueReport *out);
int anco_delegator_producer_queue(AncoDelegatorProducer *self,
    const AncoDelegatorQueueCall *actual_call,AncoDelegatorQueueReport *out);
gboolean anco_delegator_producer_add_pending(AncoDelegatorProducer *self,
    const AncoDelegatorReleaseId *actual_id,gint32 actual_slot);
int anco_delegator_producer_take_fence(AncoDelegatorProducer *self,gint32 actual_slot);
gboolean anco_delegator_producer_registered(AncoDelegatorProducer *self,gint32 actual_slot);
gboolean anco_delegator_producer_conversion_failed(AncoDelegatorProducer *self,gint32 actual_slot);
/* Original shared skip; nonshared ClearSlot removes conversion failure BEFORE
 * code4 even on missing peer/error; ClearAll code100 leaves local sets alone.
 */
int anco_delegator_producer_clear_slot(AncoDelegatorProducer *self,gint32 actual_slot,
    AncoDelegatorResult *out);
int anco_delegator_producer_clear_all(AncoDelegatorProducer *self,AncoDelegatorResult *out);
/* Original Reset retains peer, shared, registered and pending-frame fields.
 * Clear pending(TRUE) while prior flags remain observable, clear release and
 * conversion records, metadata INIT, then active FALSE/task -1/notify TRUE/
 * destroyed FALSE. Does NOT call SetClient(NULL), ClearAll or remove last frame.
 */
int anco_delegator_producer_reset(AncoDelegatorProducer *self);
/* Explicit local retirement drops real receiver immediately and consumes one
 * owned reference. Other holders remain valid but retired; release them with
 * unref. Owned FDs survive until the last holder/ongoing call returns.
 * Without Reset, synthetic code12, host messages, release or completion.
 * Use unref, not free, when dropping only one original shared ownership. */
void anco_delegator_producer_free(AncoDelegatorProducer *self);
#endif
