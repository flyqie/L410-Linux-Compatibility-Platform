/* Standard IBus engine adapter for X11 clients using the original AnCo IMF.
 * IBus remains the Linux application protocol; this file adds no host wire. */
#ifndef ANCO_IBUS_BRIDGE_H
#define ANCO_IBUS_BRIDGE_H

#include "anco_imf_callback_receiver.h"
#include "anco_linux_imf_runtime.h"
#include <glib.h>

typedef struct AncoIbusBridge AncoIbusBridge;

typedef struct {
    /* Return TRUE only when the current host-focused Linux window is an
     * admitted Xwayland toplevel. */
    gboolean (*focus)(void *data, gboolean focused);
    void (*panel)(void *data, gboolean shown);
    void (*cursor)(void *data, gint x, gint y, gint width, gint height);
    void (*surrounding)(void *data, const char *text,
                        guint cursor_chars, guint anchor_chars);
} AncoIbusBridgeOps;

AncoIbusBridge *anco_ibus_bridge_new(AncoLinuxImfRuntime *imf,
    const AncoIbusBridgeOps *ops, void *data);
void anco_ibus_bridge_free(AncoIbusBridge *bridge);

/* Original AnCo removeWindow resets the host IMF connection. A desktop X11
 * transient can disappear without changing the still-focused IBus XIC, so
 * rebuild that same editor connection from its retained standard IBus state. */
void anco_ibus_bridge_host_imf_reset(AncoIbusBridge *bridge);

/* Called synchronously by the sole original SA66875 callback receiver.
 * TRUE means the callback belonged to the active IBus editor and was handled;
 * service/status callbacks deliberately return FALSE for the common consumer. */
gboolean anco_ibus_bridge_consume(AncoIbusBridge *bridge,
    const AncoImfCallbackMessage *message, AncoImfCallbackResult *result);

#endif
