/* Linux semantic bridge of original producer metadata cache, not buffer state. */
#ifndef ANCO_DELEGATOR_METADATA_STATE_H
#define ANCO_DELEGATOR_METADATA_STATE_H
#include "anco_delegator_metadata.h"

typedef struct {
    guint32 dirty, transform;
    gint32 queue_size;
    guint32 dataspace;
} AncoDelegatorMetadataState;
/* Exact original Create's fields at +150/+154/+158/+15c. */
#define ANCO_DELEGATOR_METADATA_STATE_INIT {0u, 0xffffffffu, -1, 0u}

typedef struct {
    gboolean attempted; /* includes an attempt with no paired peer, as stock */
    int status;         /* skipped=0; absent peer=-1; otherwise actual error */
    AncoDelegatorResult wire;
} AncoDelegatorMetadataUpdate;
typedef struct {
    AncoDelegatorMetadataUpdate transform, queue_size, dataspace;
} AncoDelegatorMetadataReplay;

/* Same serialized producer context as receiver/Wayland. State and report are
 * required and must not alias. peer is borrowed from the actual SetClient
 * ownership; no lookup, substitute peer, fixed feature mask or retry timer.
 * Original Android-domain values only. A clean unchanged property is skipped;
 * failure keeps its dirty bit. Queue size preserves original signed maximum.
 * Reports are Linux diagnostics, not new host replies/readiness signals.
 */
int anco_delegator_metadata_state_queue_size(AncoDelegatorMetadataState *state,
    GBinderRemoteObject *peer, guint32 features, gint32 size,
    AncoDelegatorMetadataUpdate *report);
void anco_delegator_metadata_state_transform(AncoDelegatorMetadataState *state,
    GBinderRemoteObject *peer, guint32 features, guint32 transform,
    AncoDelegatorMetadataUpdate *report);
void anco_delegator_metadata_state_dataspace(AncoDelegatorMetadataState *state,
    GBinderRemoteObject *peer, guint32 features, guint32 dataspace,
    AncoDelegatorMetadataUpdate *report);
/* Only the metadata portion of original OnSetClient. If any dirty bit is set,
 * invoke setters in original transform/queue/dataspace order, never abort on
 * one failure. Do not force clean fields dirty just because peer changed.
 * Caller still MUST execute actual pending-buffer replay afterward, including
 * when dirty==0. This function is NOT a complete receiver SetClient consumer.
 */
void anco_delegator_metadata_state_replay(AncoDelegatorMetadataState *state,
    GBinderRemoteObject *actual_peer, guint32 features,
    AncoDelegatorMetadataReplay *report);
#endif
