/* Linux semantic bridge into the original AnCo IMF services. The public wire
 * remains standard Wayland text-input on the Linux side and the original
 * SA66827/SA66875 Binder interfaces on the Harmony side. */
#ifndef ANCO_LINUX_IMF_RUNTIME_H
#define ANCO_LINUX_IMF_RUNTIME_H
#include "anco_native_input_relay.h"
#include "anco_imf_callback_receiver.h"
#include "anco_text_receiver.h"
#include "anco_wayland_context.h"
#include <gbinder.h>

typedef struct AncoLinuxImfRuntime AncoLinuxImfRuntime;
typedef void (*AncoLinuxImfKeyDone)(void *,const AncoNativeInputKey *,
                                    bool consumed,bool stale);
typedef gboolean (*AncoLinuxImfExternalConsumer)(void *,
    const AncoImfCallbackMessage *,AncoImfCallbackResult *);

AncoLinuxImfRuntime *anco_linux_imf_runtime_new(GBinderIpc *,AncoTextSeat *,
    AncoWaylandContext *,AncoLinuxImfKeyDone,void *);
/* New generation for every actual editor boundary. Generic text fields are
 * accepted here only after standard Wayland admission selected that editor. */
int anco_linux_imf_focus(AncoLinuxImfRuntime *,bool show_keyboard,
    int32_t pattern,int32_t enter_key_type,int32_t option);
void anco_linux_imf_blur(AncoLinuxImfRuntime *);
bool anco_linux_imf_enabled(const AncoLinuxImfRuntime *);
bool anco_linux_imf_dispatch_key(AncoLinuxImfRuntime *,const AncoNativeInputKey *);
/* Deliver an original IMF current-connection editor key through the same
 * focused wl_keyboard route used after the original consumption gate. */
int anco_linux_imf_runtime_editor_key(AncoLinuxImfRuntime *,int32_t android_key);
void anco_linux_imf_cursor(AncoLinuxImfRuntime *,double x,double y,double height);
/* Forward the standard Wayland surrounding-text selection to the original
 * AnCo manager transaction 7. cursor/anchor are UTF-8 byte boundaries; this
 * bridge converts them to Android/Java UTF-16 selection end/start units. */
int anco_linux_imf_selection(AncoLinuxImfRuntime *,const char *text,
    int32_t cursor,int32_t anchor);
void anco_linux_imf_panel(AncoLinuxImfRuntime *,bool show);
void anco_linux_imf_runtime_set_external_consumer(AncoLinuxImfRuntime *,
    AncoLinuxImfExternalConsumer,void *);
/* Withdraws SA66875 before dropping the local callback and death listener. */
int anco_linux_imf_runtime_free(AncoLinuxImfRuntime *);
#endif
