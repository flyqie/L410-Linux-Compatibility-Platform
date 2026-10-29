/* Linux semantic bridge for original AncoWmsBrokerProxy endpoint lifetime. */
#ifndef ANCO_WMS_BROKER_H
#define ANCO_WMS_BROKER_H
#include "anco_wms_transport.h"
#include "anco_wms_layout.h"

typedef struct AncoWmsBroker AncoWmsBroker;
typedef struct {
    guint attempts;
    gint32 original_return;
    AncoWmsResult last;
} AncoWmsSendResult;

/* Borrow the caller's actual host IPC, retain a reference, and do the original
 * eager SA66822 lookup. Never open another binder device or start a service.
 * All calls, including free, belong to this thread and the common default GLib
 * context used by libgbinder death callbacks/AMS/Wayland. No worker use.
 * A non-NULL object does not imply endpoint readiness or window acceptance.
 */
AncoWmsBroker *anco_wms_broker_new(GBinderIpc *ipc);
int anco_wms_broker_free(AncoWmsBroker *broker);

/* Supported original messages only. Freeze the message once, then send;
 * nonzero original Proxy return permits exactly ONE resend after original
 * getRemoteBroker. Cached endpoint is reused until its death callback clears
 * it. Missing endpoint preserves original_return0/attempts0 but returns FALSE;
 * zero original_return on exception likewise never proves successful send.
 * Native UNKNOWN_TRANSACTION (-EBADMSG) is original transact false -> Proxy-1,
 * so it permits that same one resend. Other local/transport/decoding failures
 * do not trigger retries or masquerade as service results. Short reply's native
 * readInt returns0; original_return preserves0 but TRUE still requires a real
 * decoded final result0. Preserve raw transport separately in last.
 * No timers, reconnection polling, activation/type/token/IME policy here.
 */
gboolean anco_wms_broker_send(AncoWmsBroker *broker,
                             const AncoWmsMessage *message, AncoWmsSendResult *out);

/* Trusted WindowState-equivalent snapshot. Type is the actual admitted type,
 * NOT inferred here from xdg parent/title. The special Task getter result must
 * come from actual Task membership; a matching title alone is insufficient.
 * Identities remain separate; missing Task/Activity/parent use stock -1 rules.
 */
typedef struct {
    gboolean anco_enabled;
    gint32 window, type;
    gboolean has_task, task_is_home;
    gint32 task;
    gboolean has_activity;
    gint32 activity;
    gboolean has_parent;
    gint32 parent;
    const char *last_title;
    gboolean task_has_wechat_splash_activity;
    gboolean activity_exists_process_died;
} AncoWmsCreateState;

typedef enum {
    ANCO_WMS_CREATE_NOT_ENTERED,
    ANCO_WMS_CREATE_INVALID_WINDOW,
    ANCO_WMS_CREATE_SKIPPED,
    ANCO_WMS_CREATE_SEND_ENTERED
} AncoWmsCreateDisposition;

typedef struct {
    AncoWmsCreateDisposition disposition;
    AncoWmsSendResult send;
} AncoWmsCreateResult;

/* Original createWindow entry, using the existing original resolver/transport.
 * NULL state is the original invalid-window branch. Missing broker/output or
 * wrong owner thread is a local API error, before changing borrowed state.
 * Valid, unskipped windows clear process-died BEFORE send, even if delivery
 * fails. The original method ignores its proxy return; this Boolean only
 * preserves transport diagnostics, never host creation/input/GPU readiness.
 * No Activity creation, type selection, auto-retry beyond original broker,
 * surface allocation, IME policy or guessed token admission is added.
 * Runtime must serialize state and keep it alive throughout this call.
 */
gboolean anco_wms_broker_create_window(AncoWmsBroker *broker,
    AncoWmsCreateState *state, AncoWmsCreateResult *out);

/* Original showWindow/hideWindow entry. The caller supplies the actual
 * WindowState snapshot and the reason selected by the original lifecycle
 * source (relayout visibility uses reason0; Surface shown uses reason1).
 * This helper only projects WMS11/12 and preserves the broker's original
 * endpoint/retry behavior. It does not infer mapping, focus or IME state.
 */
gboolean anco_wms_broker_set_visibility(AncoWmsBroker *broker,
    const AncoWmsCreateState *state, gboolean visible, gint32 reason,
    AncoWmsSendResult *out);

/* Original updateLayoutParams: NULL attrs or invalid Window is skipped;
 * otherwise actual fromLp projection precedes original WMS14 send. No special
 * create-skip predicate, show, IME update or process-died mutation belongs here.
 * state and geometry MUST describe the same actual Window; inconsistent
 * duplicated getter values fail locally before sending. Missing geometry is
 * an adapter error, distinct from an actual Window with has_frame=FALSE.
 * Incoming attrs.type may differ from state.type, as in the original method.
 * FALSE/attempts0/local_error0 denotes an original skipped call, not delivery.
 */
gboolean anco_wms_broker_update_layout(AncoWmsBroker *broker,
    const AncoWmsCreateState *state, const AncoWmsLayout *attrs,
    const AncoWmsLayoutState *geometry, AncoWmsSendResult *out);
#endif
