/* Linux ownership bridge for the original ProducerSurfaceDelegator receiver. */
#ifndef ANCO_DELEGATOR_RECEIVER_H
#define ANCO_DELEGATOR_RECEIVER_H
#include "anco_delegator_release.h"
#define ANCO_DELEGATOR_IFACE "surf.TransactSurfaceDelegator"
/* Stock SP68 native SendSelfProxy/release callback token is directional.
 * Its ProducerDelegator::OnRemoteRequest still checks the guest token above.
 * Direct Linux/native Binder callbacks must check the native sender's token;
 * do not rewrite the host or change guest-to-host buffer/metadata tokens. */
#define ANCO_DELEGATOR_CALLBACK_IFACE "ohos.donghu.ITransactSurfaceDelegator"
#define ANCO_DELEGATOR_SET_CLIENT_TRANSACTION 5
#define ANCO_DELEGATOR_DESTROYED_TRANSACTION 12

typedef struct AncoDelegatorReceiver AncoDelegatorReceiver;
typedef struct {
    /* Actual release consumer status becomes code3's transport status.
     * To keep the real duplicated FD, transfer owned_fd and set it to -1.
     * Receiving a release does not itself authorize slot reuse or wl release. */
    int (*release)(AncoDelegatorRelease *release, void *data);
    /* The actual peer is borrowed for this call, including NULL, as stock.
     * Owner must replay real metadata and pending buffers. No ready receipt. */
    void (*set_client)(GBinderRemoteObject *actual_peer, void *data);
    /* Peer has already been cleared. Matches ClearOhPendingRelease(false):
     * replay actual pending release IDs to the real listener with fence -1;
     * FALSE keeps that listener. It does NOT suppress pending releases, nor
     * authorize invented IDs/GPU completion. The owner also marks destroyed. */
    void (*destroyed)(gboolean clear_listener, void *data);
    /* ORIGINAL NotifyOhSurfaceDestroyed sets +1d9 BEFORE SetClient(NULL).
     * Mark the actual owner first, without freeing peer or dispatching state.
     * destroyed above follows the actual strong-peer clear. Not a host message.
     */
    void (*mark_destroyed)(void *data);
} AncoDelegatorReceiverOps;

/* All calls and transactions belong to the same gbinder/Wayland main context.
 * Required consumers must remain valid until free, including nested callbacks.
 * This creates a real registered local Binder object, but does not send code2,
 * choose identity, obtain a buffer, replay metadata or assert GUI readiness. */
AncoDelegatorReceiver *anco_delegator_receiver_new(GBinderIpc *actual_ipc,
    guint32 actual_features, const AncoDelegatorReceiverOps *ops, void *data);
/* Returned actual references must be unreferenced by the caller. The peer may
 * be NULL; a non-NULL peer proves receipt of code5 only, not a working surface. */
GBinderLocalObject *anco_delegator_receiver_ref_object(AncoDelegatorReceiver *self);
GBinderRemoteObject *anco_delegator_receiver_ref_client(AncoDelegatorReceiver *self);
/* ORIGINAL base SetClient(NULL): clear the actual stored strong peer only.
 * Does not invoke OnSetClient/replay, destroyed or pending-release callbacks;
 * this is NOT an incoming synthetic code5/12 or a host transaction. */
void anco_delegator_receiver_clear_client(AncoDelegatorReceiver *self);
/* Drop disables callbacks even if the host retains the local object. Owner
 * separately retires its real buffers; free does not invent a code12 event.
 * May be called from a consumer; ongoing dispatch keeps its own lifetime. */
void anco_delegator_receiver_free(AncoDelegatorReceiver *self);
#endif
