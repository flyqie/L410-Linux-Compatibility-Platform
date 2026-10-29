/* Linux-local original DelegatorController record ownership, not bootstrap. */
#ifndef ANCO_DELEGATOR_REGISTRY_H
#define ANCO_DELEGATOR_REGISTRY_H
#include "anco_delegator_producer.h"
#include "anco_graphic_registration.h"
typedef struct AncoDelegatorRegistry AncoDelegatorRegistry;
typedef struct { guint32 first; guint64 second; } AncoDelegatorSurfaceKey;
typedef struct { gboolean found; guint32 remaining; } AncoDelegatorRemoveRecord;
/* Prepared borrowed IPC and actual negotiated features must outlive all
 * returned producers, including after registry free. No token negotiation,
 * host startup, inferred key identities or original global controller here.
 * Serialized common context; no nested state dispatch or concurrent mutation.
 */
AncoDelegatorRegistry *anco_delegator_registry_new(GBinderIpc *actual_ipc,
    guint32 actual_features);
typedef struct {
    int check_status;
    AncoGraphicVersionCheck check;
} AncoDelegatorRegistryInit;
/* Original controller construction: initial features0, exactly one version
 * check, then Add uses that controller's resulting bits. Own one instance per
 * common runtime, not per window; do not recreate to force renegotiation.
 * Original ctor ignores Check's boolean; failed lookup/legacy result therefore
 * does not manufacture a supported mask or prevent local registry creation.
 * Keep raw diagnostics; non-NULL registry is NOT host admission/readiness.
 * Retains original death lifecycle, without cache reset or automatic retry.
 * Free releases version listener BEFORE table references, as original D2.
 */
AncoDelegatorRegistry *anco_delegator_registry_new_original(GBinderIpc *actual_ipc,
    AncoDelegatorRegistryInit *out);
/* Exact key reuse increments original 32-bit count; first record starts at1.
 * Original task is used only for a new producer. Reuse doesn't reset/rebind it.
 * out receives an owned reference, replacing/unrefing its previous owned value.
 * Return is a Linux local error; original count is separate. No Graphic code2,
 * Reset/ClearAll or listener installation: those belong to the original caller.
 */
int anco_delegator_registry_add(AncoDelegatorRegistry *self,
    AncoDelegatorSurfaceKey key,gint32 actual_task,
    AncoDelegatorProducer **out,guint32 *count);
/* Clear found producer's cached clone BEFORE decrement; erase only at0.
 * Missing key returns original count0 with local found=FALSE, not an error.
 * No code5 node removal/code12, queue binding mutation or buffer completion.
 */
int anco_delegator_registry_remove(AncoDelegatorRegistry *self,
    AncoDelegatorSurfaceKey key,AncoDelegatorRemoveRecord *out);
typedef struct {
    gboolean skipped,reused,create_attempted,binding_changed;
    guint32 count;
    AncoGraphicLookup lookup;
    AncoGraphicResult create;
    gint32 original_create_return;
    int reset_status,clear_status;
    AncoDelegatorResult clear;
} AncoDelegatorRegistration;
/* Original registration caller sequence, given ACTUAL retained identities,
 * current buffershared property string, name and real release consumer.
 * binding is the real Linux queue's owned producer reference (initially NULL).
 * It changes only at the original successful install point, separately from
 * the registry reference. Property/key gates, signed count>=2 reuse, Reset ->
 * ClearAll -> listener -> binding are preserved. First use queries SA66829,
 * creates once, then installs on original reply/default0, retaining delivery
 * diagnostics. Missing endpoint/nonzero reply leaves the Add record as stock.
 * Return is a local diagnostic, never host admission or GUI/GPU readiness.
 * No synthetic buffers, automatic retry/Remove, WMS or global property edits.
 * Borrowed listener data must outlive installed callbacks. Serialized context;
 * callbacks must not mutate/free this registry or the binding during the call.
 */
int anco_delegator_registry_register(AncoDelegatorRegistry *self,
    AncoDelegatorSurfaceKey key,gint32 actual_task,const char *actual_buffershared,
    const char *actual_name,AncoDelegatorBufferReleased actual_listener,void *data,
    AncoDelegatorProducer **binding,AncoDelegatorRegistration *out);
typedef struct {
    AncoDelegatorRemoveRecord record;
    int listener_status;
    gboolean listener_cleared,lookup_attempted,remove_attempted;
    AncoGraphicLookup lookup;
    AncoGraphicRemoveResult remove;
} AncoDelegatorUnregistration;
/* Original concrete queue teardown prefix: Remove record FIRST, obtain a
 * temporary strong ref from actual queue binding (not registry lookup), and
 * only when remaining==0 and binding exists clear listener, query Graphic once,
 * RemoveSurfaceNode. Original missing record also returns count0. Retains raw
 * transport/decode/service diagnostics; no retry/compensation/code12/release.
 * Queue binding stays owned and unchanged; caller drops it at actual later core
 * destruction. Call exactly at real teardown, not every process/focus event.
 * Key is that queue's retained original pair, never regenerated on teardown.
 * Same serialized/nonreentrant contract as register. Return is local error;
 * original remote return is separate in out->remove. */
int anco_delegator_registry_unregister(AncoDelegatorRegistry *,
    AncoDelegatorSurfaceKey,AncoDelegatorProducer *actual_binding,
    AncoDelegatorUnregistration *out);
/* Release registry-owned refs only; outstanding queue/getter refs stay valid. */
void anco_delegator_registry_free(AncoDelegatorRegistry *self);
#endif
