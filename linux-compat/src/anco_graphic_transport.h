/* Linux transport for original BpGraphicBroker alpha and task-start messages. */
#ifndef ANCO_GRAPHIC_TRANSPORT_H
#define ANCO_GRAPHIC_TRANSPORT_H
#include <gbinder.h>

#define ANCO_GRAPHIC_SA 66829
#define ANCO_GRAPHIC_IFACE "ohos.anco.IGraphicBroker"
#define ANCO_GRAPHIC_SET_ALPHA_WITH_WINDOW_ID 17
#define ANCO_GRAPHIC_NOTIFY_TASK_STARTED 3

typedef struct { int local_error,transport; gboolean resolved; } AncoGraphicLookup;
/* One original HOST SAMgr query for SA66829. Returns an owned real endpoint,
 * or NULL with diagnostics. Prepared IPC; no cache/retry/startup/ready policy. */
GBinderRemoteObject *anco_graphic_lookup_once(GBinderIpc *actual_ipc,
    AncoGraphicLookup *out);

typedef struct {
    int local_error;
    int transport;
    gboolean decoded;
    gint32 service_result;
} AncoGraphicResult;

/* Original body is float then int32, without Parcelable presence/command.
 * Append to an original-interface request. Preserve bits and scalar values;
 * there is no clamping, guessed WindowId or policy here.
 */
int anco_graphic_append_alpha(GBinderLocalRequest *request,
                              gfloat alpha, gint32 window_id);

/* One synchronous transaction on a borrowed original resolved endpoint.
 * TRUE means a reply int was received with transport status0. The int is NOT
 * necessarily a success/error code: original version exchange sends
 * (-999.0f,115) through this very method and receives feature bits.
 * Reply is read even on transport failure, as stock. After an actual send,
 * original proxy return is decoded?service_result:0; this is not an ack.
 * No SA discovery, reconnect, resend, version/cache/death state, real window
 * admission, buffer transport or automatic GUI caller is implemented here.
 */
gboolean anco_graphic_set_alpha_with_window_id(GBinderRemoteObject *graphic,
    gfloat alpha, gint32 window_id, AncoGraphicResult *out);

typedef struct { guint8 token_supported; guint32 features; } AncoGraphicVersionState;
#define ANCO_GRAPHIC_VERSION_STATE_INIT {255,0}
typedef struct {
    guint attempts;
    AncoGraphicResult wire[2];
    gint32 original_return[2];
    gboolean original_result;
} AncoGraphicVersionExchange;
/* Original SendAncoGraphicVersionToBroker leaf, not CheckIsTokenSupported.
 * Actual borrowed endpoint and serialized controller-owned state. Sends original
 * (-999,115); only first original return0 resends once. 0/5/8 clear token byte
 * but retain feature bits. Other values replace bits, setting token byte only
 * when bit2 is present. Preserve signed replies as raw original bit patterns.
 * Local pre-send errors are diagnostics and do not count as a stock exchange.
 * original_result is stock control flow, NOT transport/feature/GUI readiness.
 * No SA lookup, cached check, death subscription or automatic registry startup.
 */
int anco_graphic_exchange_version(GBinderRemoteObject *actual_graphic,
    AncoGraphicVersionState *actual_state,AncoGraphicVersionExchange *out);

typedef struct AncoGraphicVersionController AncoGraphicVersionController;
typedef struct {
    gboolean cached,original_result,watch_installed;
    AncoGraphicLookup lookup;
    AncoGraphicVersionExchange exchange;
    AncoGraphicVersionState state;
} AncoGraphicVersionCheck;
/* Original outer unknown-cache check and death lifecycle. Borrowed prepared
 * IPC survives the controller. Serialized owning GLib context, no nested
 * check/free. No host activation or constructor-time transaction. check queries
 * only for token255, exchanges, then attaches death notification only once
 * token becomes known. Death drops the watch, NOT token/features. Cached return
 * is token!=0; an uncached return is the original exchange result. Reports are
 * diagnostics, not graphics readiness. Local watcher retains the actual remote
 * GObject for libgbinder notification lifetime; no endpoint reconnect policy.
 */
AncoGraphicVersionController *anco_graphic_version_controller_new(GBinderIpc *actual_ipc);
int anco_graphic_version_controller_check(AncoGraphicVersionController *self,
    AncoGraphicVersionCheck *out);
void anco_graphic_version_controller_free(AncoGraphicVersionController *self);

typedef struct {
    AncoGraphicResult wire;
    gint32 original_return; /* original readInt32 default0 is NOT an ack */
} AncoGraphicTaskStartedResult;
/* ORIGINAL BpGraphicBroker NotifyTaskStarted: mandatory descriptor, int32 Task,
 * code3/flags0. Original ignores transact status and reads reply int regardless;
 * missing/truncated reply gives original_return0. Preserve raw diagnostics too.
 * TRUE requires actual transport0 and decoded reply; does not require service0.
 * TaskStarted reports startup timing, NOT window/buffer/GPU readiness. No retry,
 * ID allocation, SA lookup or synthesized success/ready receipt here. */
gboolean anco_graphic_notify_task_started_once(GBinderRemoteObject *actual_graphic,
    gint32 actual_task, AncoGraphicTaskStartedResult *out);
typedef struct {
    GBinderRemoteObject *actual_graphic; /* borrowed actual resolved SA66829 */
    AncoGraphicTaskStartedResult last;
} AncoGraphicTaskStartedOperation;
/* Linux internal callback for normal pending replay. Caller performs original
 * SA resolution and retains the actual endpoint/data for the whole call.
 * Local missing/allocation errors leave the task flag pending. Otherwise return
 * ORIGINAL proxy int, including0 without verified reply, as original control
 * flow. Caller MUST inspect last.wire for delivery proof; flag clear is not proof.
 * No default endpoint, fixed Task, service start or readiness policy. */
int anco_graphic_task_started_callback(gint32 actual_task, void *operation);

typedef struct {
    int lookup_local_error;
    int lookup_transport;
    gboolean resolved;
    gboolean notification_attempted;
    AncoGraphicTaskStartedResult notification;
} AncoGraphicTaskStartResult;
/* ORIGINAL ProducerSurfaceDelegator::NotifyTaskStart lookup/call lifetime.
 * Borrowed, prepared actual host IPC, used on the common IPC main context.
 * Resolve SA66829 once via original HOST SAMgr header/handle0/code2, read a
 * real strong object only on transport0, then notify once and unref it.
 * No endpoint cache/death policy, retry, guest bootstrap, readiness or ID
 * allocation. Caller still owns original task/flag and decides WHEN to call.
 * TRUE proves decoded notification with transport0, including nonzero reply;
 * neither TRUE nor original_return0 means Task/window creation or GPU ready. */
gboolean anco_graphic_task_start_once(GBinderIpc *actual_ipc, gint32 actual_task,
    AncoGraphicTaskStartResult *out);
typedef struct {
    GBinderIpc *actual_ipc; /* borrowed, caller holds it for the entire call */
    AncoGraphicTaskStartResult last;
} AncoGraphicTaskStartOperation;
/* Linux pending replay adapter. Missing manager/remote leaves pending flag;
 * otherwise returns original proxy int, even0 after missing reply. Inspect
 * last for delivery evidence. This is not a new host interface. */
int anco_graphic_task_start_callback(gint32 actual_task, void *operation);
#endif
