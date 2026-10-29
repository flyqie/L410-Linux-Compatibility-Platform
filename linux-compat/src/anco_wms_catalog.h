/* Linux normal-application Activity admission to original WMS. */
#ifndef ANCO_WMS_CATALOG_H
#define ANCO_WMS_CATALOG_H
#include "anco_linux_catalog.h"
#include "anco_wms_broker.h"

/* Actual catalog creation observer. Attach using on_activity_created BEFORE
 * launching normal application Tasks; user_data is the shared AncoWmsBroker.
 * The runtime must establish normal (non-Home) Task semantics, not infer them
 * from title/package, mission or surface. This adapter does not classify Tasks
 * and is not suitable for a catalog containing Home/Recents role admission.
 * Catalog and broker outlive registration. Same serialized owner thread.
 * Original WMS32 uses committed Activity/Task identities, ignoring its proxy
 * result for creation control flow. Diagnostics retain transport provenance.
 * No custom retries, rollback, mission wait, visibility or buffer admission.
 * Consumer must perform original window/input/IME/Graphic retirement before
 * WMS33 separately; child exit is not an automatic Activity removal trigger.
 * This hook alone is not a complete launch/window runtime.
 */
void anco_wms_catalog_activity_created(AncoLinuxCatalog *catalog,
    const AncoLinuxLaunch *activity, void *user_data);
#endif
