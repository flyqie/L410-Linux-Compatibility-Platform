#ifndef ANCO_IMF_BROKER_H
#define ANCO_IMF_BROKER_H
#include <gbinder.h>
#include "anco_imf_client_v340.h"
typedef struct AncoImfBroker AncoImfBroker;
typedef void (*AncoImfBrokerDied)(AncoImfBroker *, void *);
/* Create/use/free on the thread dispatching this IPC's GLib death callbacks.
 * IPC is retained. Constructor eagerly queries; absence does not destroy cache.
 * No timer, service restart, attach-support state or implicit IMF cleanup. */
AncoImfBroker *anco_imf_broker_new(GBinderIpc *ipc);
/* Owned reference; caller unrefs it. NULL may mean unavailable or wrong thread. */
GBinderRemoteObject *anco_imf_broker_get(AncoImfBroker *broker);
/* Owned temporary transport proxy for the cached service. close releases it
 * without implicitly changing the input session. May outlive cache retirement. */
AncoImfClient *anco_imf_broker_client(AncoImfBroker *broker);
int anco_imf_broker_set_listener(AncoImfBroker *broker,
                               AncoImfBrokerDied callback, void *data);
/* -EPERM on wrong thread, -EBUSY in a query/death callback. */
int anco_imf_broker_free(AncoImfBroker *broker);
#endif
