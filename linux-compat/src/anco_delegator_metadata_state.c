#include "anco_delegator_metadata_state.h"
#include <errno.h>

static void reset(AncoDelegatorMetadataUpdate *out)
{
    *out = (AncoDelegatorMetadataUpdate){FALSE, 0, {0, -1, FALSE, -1}};
}
static void finish(AncoDelegatorMetadataState *state, guint32 bit,
    GBinderRemoteObject *peer, AncoDelegatorMetadataUpdate *out)
{
    /* Stock SendMessage returns -1 for a missing peer; preserve the separate
     * Linux local error in wire rather than relabeling it transport success. */
    out->status = !peer ? -1 : out->wire.local_error ?
        out->wire.local_error : out->wire.transport;
    if (!out->status) state->dirty &= ~bit;
}
int anco_delegator_metadata_state_queue_size(AncoDelegatorMetadataState *state,
    GBinderRemoteObject *peer, guint32 features, gint32 size,
    AncoDelegatorMetadataUpdate *out)
{
    if (!out) return -EINVAL;
    reset(out);
    if (!state) return out->status = -EINVAL;
    if (!(state->dirty & 2u) && state->queue_size >= size) return 0;
    if (size > state->queue_size) state->queue_size = size;
    state->dirty |= 2u;
    out->attempted = TRUE;
    anco_delegator_queue_size_once(peer, features, state->queue_size, &out->wire);
    finish(state, 2u, peer, out);
    return out->status;
}
void anco_delegator_metadata_state_transform(AncoDelegatorMetadataState *state,
    GBinderRemoteObject *peer, guint32 features, guint32 value,
    AncoDelegatorMetadataUpdate *out)
{
    if (!out) return;
    reset(out);
    if (!state) { out->status = -EINVAL; return; }
    if (!(state->dirty & 1u) && state->transform == value) return;
    state->transform = value;
    state->dirty |= 1u;
    out->attempted = TRUE;
    anco_delegator_transform_once(peer, features, value, &out->wire);
    finish(state, 1u, peer, out);
}
void anco_delegator_metadata_state_dataspace(AncoDelegatorMetadataState *state,
    GBinderRemoteObject *peer, guint32 features, guint32 value,
    AncoDelegatorMetadataUpdate *out)
{
    if (!out) return;
    reset(out);
    if (!state) { out->status = -EINVAL; return; }
    if (!(state->dirty & 4u) && state->dataspace == value) return;
    state->dataspace = value;
    state->dirty |= 4u;
    out->attempted = TRUE;
    anco_delegator_dataspace_once(peer, features, value, &out->wire);
    finish(state, 4u, peer, out);
}
void anco_delegator_metadata_state_replay(AncoDelegatorMetadataState *state,
    GBinderRemoteObject *peer, guint32 features, AncoDelegatorMetadataReplay *out)
{
    if (!out) return;
    reset(&out->transform); reset(&out->queue_size); reset(&out->dataspace);
    if (!state) {
        out->transform.status = out->queue_size.status = out->dataspace.status = -EINVAL;
    } else if (state->dirty) {
        anco_delegator_metadata_state_transform(state, peer, features,
            state->transform, &out->transform);
        anco_delegator_metadata_state_queue_size(state, peer, features,
            state->queue_size, &out->queue_size);
        anco_delegator_metadata_state_dataspace(state, peer, features,
            state->dataspace, &out->dataspace);
    }
}
