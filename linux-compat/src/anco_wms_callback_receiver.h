/* Original guest WMS callback receiving boundary, not a window manager. */
#ifndef ANCO_WMS_CALLBACK_RECEIVER_H
#define ANCO_WMS_CALLBACK_RECEIVER_H
#include "anco_wms_callback_codec.h"
typedef struct AncoWmsCallbackReceiver AncoWmsCallbackReceiver;
typedef struct {
    /* Actual common-runtime services only. FALSE means unavailable/failed;
     * TRUE corresponds to the original void consumer returning, not evidence
     * that a window changed. NULL means the original service is unavailable.
     * Runs locally under the runtime's own authority after sender validation;
     * never uses WorkSource as caller identity or impersonates an Android UID.
     * No nested dispatch, receiver free or synthetic success consumer. */
    gboolean (*window)(const AncoWmsCallbackMessage *,
                        const AncoWmsCallbackHeader *, void *);
    /* Supply only when actual SmartEdge support/manager exists. Non-NULL info
     * has passed the original broker gate (isCalledFromBroker=true semantics). */
    gboolean (*smart_edge)(const AncoWmsCallbackMessage *, void *);
    /* Optional borrowed ACTUAL diagnostic trace, only if the runtime's original
     * trace policy/rate limit permits it. NULL -> original zero stack length.
     * Do not generate fictitious Java frames or Android process identities. */
    const char *(*exception_trace)(void *);
} AncoWmsCallbackReceiverOps;
/* Actual prepared aidl2 IPC; registers a local Binder object, NOT SA66870.
 * Same serialized default context as AMS/Wayland. ops copied; data borrowed.
 * Original common runtime still owns SA registration, real state and teardown.
 * No new thread, property, global security policy, window or ready Boolean. */
AncoWmsCallbackReceiver *anco_wms_callback_receiver_new(GBinderIpc *,
    const AncoWmsCallbackReceiverOps *, void *);
GBinderLocalObject *anco_wms_callback_receiver_ref_object(AncoWmsCallbackReceiver *);
typedef struct {
    gboolean attempted, reply_decoded;
    int transport_status;
    gint32 service_status;
} AncoWmsPublicationResult;
/* Original SAMgr add3/remove4, actual receiver Binder, SA66870. These APIs do
 * not discover/replace another owner or declare consumers ready. Before add,
 * common runtime must establish namespace ownership and accurately expose its
 * available consumers. The original endpoint can exist while getService is
 * unavailable: leave missing consumers NULL and preserve the original -1,
 * never install a recording-only consumer that reports successful application.
 * Before remove, it must establish that this is STILL its own publication.
 * Never withdraw by ID after another owner has replaced it. No automatic retry.
 * Same creation thread/context; reentrant mutation is refused. An attempted
 * add retains the receiver even on ambiguous failure. Only decoded successful
 * withdrawal clears retention; caller must resolve ambiguous ownership first.
 * Results are local diagnostics, not a new wire interface or GUI readiness.
 */
int anco_wms_callback_receiver_publish(AncoWmsCallbackReceiver *, AncoWmsPublicationResult *);
int anco_wms_callback_receiver_withdraw(AncoWmsCallbackReceiver *, AncoWmsPublicationResult *);
/* Owner must remove SA66870 before dropping an exported receiver. Busy returns
 * -EBUSY without freeing, also while publication is retained. Wrong thread
 * returns -EPERM. Drop removes handlers even if remote refs survive. */
int anco_wms_callback_receiver_free(AncoWmsCallbackReceiver *);
#endif
