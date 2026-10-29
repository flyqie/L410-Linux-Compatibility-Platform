#ifndef ANCO_IMF_PUBLICATION_H
#define ANCO_IMF_PUBLICATION_H
#include <gbinder.h>
/* Local diagnostics, not a new host protocol. service_status is meaningful
 * only when reply_decoded is true; transport failure is not proven absence. */
typedef struct {
    gboolean attempted, reply_decoded;
    int transport_status;
    gint32 service_status;
} AncoImfPublicationResult;
/* Synchronous original SAMgr add3/remove4 for 66875 on prepared aidl2 IPC.
 * Return local error, transport error, -1 for unreadable reply, or original
 * service result. Caller owns IPC/receiver and serializes the common runtime.
 * These functions do not claim exclusive ownership, retry, replace, or probe
 * absence. Only the publication owner may remove; withdraw before dropping the
 * receiver. A failed/ambiguous call must be resolved before releasing it. */
int anco_imf_publication_add(GBinderIpc *, GBinderLocalObject *,
                           AncoImfPublicationResult *);
int anco_imf_publication_remove(GBinderIpc *, AncoImfPublicationResult *);
#endif
