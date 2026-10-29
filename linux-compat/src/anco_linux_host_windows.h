#ifndef ANCO_LINUX_HOST_WINDOWS_H
#define ANCO_LINUX_HOST_WINDOWS_H
#include <glib.h>
#include "anco_native_input_relay.h"
typedef struct {
    AncoNativeHostWindowFields fields;
    GArray *transform, *hot_areas, *excluded_areas;
} AncoLinuxHostWindow;
/* Original order/IDs/flags/areas, separate from Linux Task/Window identities. */
GPtrArray *anco_linux_host_windows_decode(const void *data, gsize bytes);
#endif
