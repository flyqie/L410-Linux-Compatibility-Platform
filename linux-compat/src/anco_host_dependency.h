#ifndef ANCO_HOST_DEPENDENCY_H
#define ANCO_HOST_DEPENDENCY_H
#include <gbinder.h>
/* Original current-host SAMgr LoadSystemAbility(id,4) local path. Caller
 * owns the returned real Binder reference and releases it after its use.
 * Original transaction7 and callbacks1/2, no property writes, service
 * implementation or Android guest activation.
 * A reference is NOT a promise that SAMgr cannot unload the service. */
GBinderRemoteObject *anco_host_dependency_load(GBinderIpc *, gint32 original_sa);
/* Original GetSystemAbilityWrapper seven attempts/status1 abort. Does not
 * cold-start a NOT_STARTED host process. Returned reference is owned. */
GBinderRemoteObject *anco_host_dependency_get(GBinderIpc *, gint32 original_sa);
#endif
