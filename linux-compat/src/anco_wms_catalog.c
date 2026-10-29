#include "anco_wms_catalog.h"
#include <stdio.h>

void anco_wms_catalog_activity_created(AncoLinuxCatalog *catalog,
    const AncoLinuxLaunch *activity, void *user_data)
{
    /* Trusted catalog callback, not an entry accepting client-supplied IDs.
     * Copy scalars before synchronous Binder can dispatch reverse callbacks.
     * No liveness re-query: the original event describes committed admission,
     * not a new Task selection or a promise that the process is still alive.
     */
    g_return_if_fail(catalog && activity && user_data);
    const AncoWmsMessage message = {
        .code = ANCO_WMS_CREATE_ACTIVITY,
        .data.activity = {activity->activity_id, activity->task_id}
    };
    AncoWmsSendResult result;
    const gboolean delivered = anco_wms_broker_send(user_data, &message, &result);
    printf("WMS_CATALOG_CREATE_ACTIVITY activity=%d task=%d delivered=%d attempts=%u "
           "local=%d transport=%d decoded=%d service=%d\n",
        message.data.activity.activity, message.data.activity.task, delivered,
        result.attempts, result.last.local_error, result.last.transport,
        result.last.decoded, result.last.service_result);
}
