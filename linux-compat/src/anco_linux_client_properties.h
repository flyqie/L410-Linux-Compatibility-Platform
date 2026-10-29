/* Linux bridge-internal projection of original native WindowProperties.
 * No AnCo/Binder/Wayland protocol is defined or changed by this module. */
#ifndef ANCO_LINUX_CLIENT_PROPERTIES_H
#define ANCO_LINUX_CLIENT_PROPERTIES_H
#include "anco_wms_layout.h"
#include <gio/gio.h>
typedef struct AncoClientPropertiesObserver AncoClientPropertiesObserver;
typedef struct {
    gint32 frontend_pid, task, guest_window, native_id, mode, status;
    guint64 frontend_start_ticks, close_request_serial;
    AncoWmsRect outer, drawable;
    double vpr;
    const char *bundle; /* borrowed during update only */
} AncoClientProperties;
typedef void (*AncoClientPropertiesUpdate)(const AncoClientProperties *,gpointer);
typedef void (*AncoClientPropertiesRemove)(gint32 frontend_pid,
    guint64 frontend_start_ticks,gint32 task,gint32 guest_window,
    gint32 native_id,gpointer);
AncoClientPropertiesObserver *anco_client_properties_observe(const char *directory,
    gint32 frontend_pid_or_zero,AncoClientPropertiesUpdate,AncoClientPropertiesRemove,
    gpointer data,GError **error);
void anco_client_properties_observer_free(AncoClientPropertiesObserver *);
#endif
