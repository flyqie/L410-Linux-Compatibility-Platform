/* Original delegator metadata transactions only; no implicit state/replay. */
#ifndef ANCO_DELEGATOR_METADATA_H
#define ANCO_DELEGATOR_METADATA_H
#include "anco_delegator_transport.h"

/* Borrow the actual paired host and negotiated feature mask. Arguments must
 * come from the real producer state in Android's original parameter domain;
 * these functions do not translate Wayland transforms or infer colorspaces.
 * Original code8=int32, code9/code10=uint32, synchronous flags0, conditional
 * feature2 token. Original guest reads no application reply for these codes.
 * TRUE reports local construction and transport success only, not host
 * Surface availability, accepted metadata, buffer release or presentation.
 * No value clamping, synthetic defaults, cache mutation or retries here.
 */
gboolean anco_delegator_queue_size_once(GBinderRemoteObject *paired_host,
    guint32 actual_features, gint32 actual_queue_size, AncoDelegatorResult *out);
gboolean anco_delegator_transform_once(GBinderRemoteObject *paired_host,
    guint32 actual_features, guint32 actual_android_transform,
    AncoDelegatorResult *out);
gboolean anco_delegator_dataspace_once(GBinderRemoteObject *paired_host,
    guint32 actual_features, guint32 actual_android_dataspace,
    AncoDelegatorResult *out);
#endif
