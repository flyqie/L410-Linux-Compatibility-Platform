/* Linux projection of original IWmsStub.Proxy.processRequest, not a new host
 * interface or a replacement for AncoWindowManager's admission/lifecycle.
 */
#ifndef ANCO_WMS_TRANSPORT_H
#define ANCO_WMS_TRANSPORT_H
#include <gbinder.h>

#define ANCO_WMS_SA 66822
#define ANCO_WMS_IFACE "OHOS.Anco.Service.Wms"
#define ANCO_WMS_PROCESS_TRANSACTION 1

typedef enum {
    ANCO_WMS_CREATE_WINDOW = 10,
    ANCO_WMS_SHOW_WINDOW = 11,
    ANCO_WMS_HIDE_WINDOW = 12,
    ANCO_WMS_REMOVE_WINDOW = 13,
    ANCO_WMS_UPDATE_LAYOUT = 14,
    ANCO_WMS_CREATE_ACTIVITY = 32,
    ANCO_WMS_REMOVE_ACTIVITY = 33,
    ANCO_WMS_MOVE_ACTIVITY_TO_TOP = 36,
    ANCO_WMS_FRAME_LAYOUT_FINISH = 80
} AncoWmsCode;

/* Already projected original AncoWmsLayoutParams. No guessed/default flags,
 * sizes, parent/type or orientation are supplied by this transport. This is
 * NOT the original fromLp/adjustFlags/adjustLayoutParams policy implementation.
 */
typedef struct {
    gint32 width, height, x, y, type, flags, layout_in_display_cutout_mode;
    gfloat screen_brightness;
    gint32 screen_orientation, system_ui_visibility, fit_insets_types, fit_insets_sides;
    gboolean fit_insets_ignoring_visibility;
    gint32 color_mode, parent_window;
} AncoWmsLayout;
typedef struct {
    AncoWmsCode code;
    union {
        struct { gint32 activity, task; } activity;
        struct { gint32 window, activity, type, parent, task; } create;
        struct { gint32 window, activity, task, reason; } visibility;
        struct { gint32 window, activity, task; } remove;
        struct { gint32 window, activity; AncoWmsLayout params; gint32 task; } layout;
        /* Original notification identifies the Task, not an individual Window.
         * The caller must preserve original draw-completion/feature gates.
         */
        struct { gint32 task; } frame_layout_finish;
    } data;
} AncoWmsMessage;
typedef struct {
    int local_error;
    int transport;
    gboolean decoded;
    gint32 service_result;
} AncoWmsResult;

/* Append the original Parcelable presence then raw WmsMessage body to an
 * existing original-interface request. NULL message is the original null
 * Parcelable branch. Unsupported codes fail BEFORE appending any bytes.
 * Transport preserves original scalar values including -1/-2 sizes and -1
 * parent; the trusted caller must derive identities, type/token/LP/reason from
 * actual admitted state, not title/app_id or a diagnostic argument vector.
 */
int anco_wms_append(GBinderLocalRequest *request, const AncoWmsMessage *message);

/* SINGLE original processRequest transaction on a borrowed, resolved original
 * endpoint. No SA discovery, UID switch, policy modification, auto-start,
 * reconnect/resend, lifecycle ordering or guest callback registration here.
 * Original AncoWmsBrokerProxy.sendRequest's outer resolver/retry is separate.
 * TRUE means transport+first-int result0, NOT host window creation/visibility.
 * Missing endpoint/undecodable reply cannot silently count as service success.
 * The original Proxy reads just the first int, without readException or EOF.
 */
gboolean anco_wms_process_request(GBinderRemoteObject *wms,
                                  const AncoWmsMessage *message, AncoWmsResult *out);
#endif
