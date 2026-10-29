/* Linux semantic bridge into the original AnCo Pasteboard services.
 * Linux applications keep the standard Wayland wl_data_device protocol;
 * HarmonyOS keeps the original SA66871/SA66823 Binder interfaces. */
#ifndef ANCO_LINUX_PASTEBOARD_H
#define ANCO_LINUX_PASTEBOARD_H

#ifndef WLR_USE_UNSTABLE
#define WLR_USE_UNSTABLE
#endif
#include <gbinder.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_seat.h>

typedef struct AncoLinuxPasteboard AncoLinuxPasteboard;

/* IPC, display and seat are borrowed and must outlive the bridge. The caller
 * must be the sole owner of the private Wayland session and SA66871 namespace.
 * This publishes the original SA66871 receiver and accepts real wl_data_device
 * selection requests. It does not publish a privileged data-control global. */
AncoLinuxPasteboard *anco_linux_pasteboard_new(GBinderIpc *,
    struct wl_display *, struct wlr_seat *);

/* Cancels in-flight fd transfers, detaches the seat listener and withdraws
 * SA66871 before dropping its local object. Returns an original/local error;
 * zero is the only successful retirement. */
int anco_linux_pasteboard_free(AncoLinuxPasteboard *);

#endif
