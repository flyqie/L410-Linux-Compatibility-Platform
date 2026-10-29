/* Linux consumer of the original HWC common-channel display query. */
#ifndef ANCO_GRAPHIC_DISPLAY_H
#define ANCO_GRAPHIC_DISPLAY_H
#include <stdint.h>
#include <stddef.h>
typedef struct {
    uint64_t id;
    int32_t width, height;
    float xdpi, ydpi;
    uint32_t refresh_hz;
} AncoGraphicDisplay;
/* One original common connection/query, closed on return. No version mask,
 * VSync, layers, buffers or host property changes. Returned displays are owned
 * by the caller (free). Actual service status is separate from local errno.
 * Original wire is little-endian Parcel scalars with four-byte alignment.
 * No synthetic resolution, DPI or refresh rate. Output unchanged on failure.
 * timeout bounds this RPC only, never an application lifetime or retry loop. */
int anco_graphic_query_displays(const char *socket_path, int timeout_ms,
    AncoGraphicDisplay **displays, size_t *count, int32_t *service_status);
#endif
