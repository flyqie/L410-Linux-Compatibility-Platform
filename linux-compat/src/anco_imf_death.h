#ifndef ANCO_IMF_DEATH_H
#define ANCO_IMF_DEATH_H
#include "anco_imf_lifecycle.h"
typedef struct AncoImfDeath AncoImfDeath;
/* Owns this broker's sole listener, not the broker. Owner/default GLib context.
 * Install before callback SA publication. Actual death schedules one 50ms hide
 * request per notification, like original Handler.postDelayed; no SA polling,
 * restart, attach, deduplication or host hide echo. Original wrapper holds the
 * SAME service from construction; it does not select another service at death.
 * Service view/data are borrowed and must outlive this owner/pending callbacks.
 * A present view requires actual request_hide admission; NULL is a no-op.
 * No recursive context dispatch. The service manages its own current editor. */
AncoImfDeath *anco_imf_death_new(AncoImfBroker *,const AncoImeServiceView *);
/* Use after confirmed SA withdrawal, before broker/data destruction. Cancels
 * pending hide work and unregisters listener. Wrong thread: -EPERM; active
 * service callback: -EBUSY without freeing. Must not be called by another
 * listener which has replaced this object's listener. */
int anco_imf_death_free(AncoImfDeath *);
#endif
