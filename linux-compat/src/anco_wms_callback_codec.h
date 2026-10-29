/* Decode ORIGINAL IAncoWms callback bodies, after original interface checking.
 * Internal Linux data only; not a new host interface or a ready/resize receipt. */
#ifndef ANCO_WMS_CALLBACK_CODEC_H
#define ANCO_WMS_CALLBACK_CODEC_H
#include <gbinder.h>
#define ANCO_WMS_CALLBACK_SA 66870
#define ANCO_WMS_CALLBACK_IFACE "wms.anco.AncoWmsCallback"

typedef struct {
    gint32 strict_policy, work_source_uid;
} AncoCallbackHeader;
typedef AncoCallbackHeader AncoWmsCallbackHeader;
/* Original Parcel::readString16Inplace projection. Borrowed little-endian
 * UTF16 bytes, length in code units, no conversion/Unicode normalization.
 * NULL covers both original null and malformed input; length then is zero.
 * Empty non-null strings remain distinguishable. Reader/object-table contract
 * is the same as below. Returned storage cannot outlive the actual parcel. */
const guint8 *anco_callback_read_string16(GBinderReader *reader, gsize *length);
/* Original zero-default scalar projection; valid actual reader required. */
gint32 anco_callback_read_int32(GBinderReader *reader);
/* Common original Parcel interface header only. Expected descriptor is a
 * trusted ASCII original interface name, never a client-supplied permission.
 * Same raw-byte/object-table and exception-response contract documented below.
 * No sender authorization or business transaction dispatch is performed. */
gboolean anco_callback_read_header(GBinderReader *reader, guint flags,
    const char *expected_descriptor, AncoCallbackHeader *out);
/* Ordinary original ARM64 Binder path only, not Binder RPC sessions. Reader
 * MUST start at raw parcel byte0 with its actual complete object table, not at
 * libgbinder's already-decoded body. Consumes two header words and String16,
 * checking exact UTF16 length/content/terminator and original object ranges.
 * Metadata is projected even when interface matching fails; flags bit0 clears
 * the StrictMode policy as original (FLAG_ONEWAY=1). No global/TLS mutation.
 * FALSE is local interface rejection, NOT the original SecurityException wire
 * response. Receiver still owns that response, sender UID checks and dispatch.
 * Descriptor/unknown generic Binder transactions must not use this helper. */
gboolean anco_wms_callback_read_header(GBinderReader *reader, guint flags,
                                      AncoWmsCallbackHeader *out);
typedef struct {
    guint code;
    union {
        struct { gboolean focused; gint32 display; } container_focus;
        struct { gint32 task, mode; } mode;
        struct { gint32 task, left, top, right, bottom; gfloat scale; } bounds;
        struct { gboolean focused; gint32 task, display; } task_focus;
        struct { gint32 task; gboolean mid_scene; } mid_scene;
        struct { gint32 task, state; } visibility;
        struct { gboolean present; gint32 state, screen, left, top, width, height;
                 gdouble scale_x, scale_y; } smart_edge;
    } data;
} AncoWmsCallbackMessage;
/* Reader starts at the original body. No token/UID/SA checks here; the actual
 * receiver must enforce them before applying data to its real runtime.
 * Returns local diagnostic 0, -EINVAL, -ENOTSUP or -EBADMSG. These are NOT
 * original wire replies. For a truncated scalar body, output contains the
 * ORIGINAL zero-default projection and the result reports incomplete bytes.
 * Each missing scalar stays zero without advancing the reader. A scalar that
 * overlaps an actual Binder object defaults to zero AND advances, as original
 * ARM64 Parcel::validateReadData/readInt32/readFloat/readDouble. Such overlap
 * also reports -EBADMSG locally; objects are neither acquired nor consumed.
 * Invalid args or unknown code leave output untouched. Requires the retained
 * libgbinder reader ABI and its actual complete parcel object table.
 * Caller must not treat either decoded data or a diagnostic as window success.
 * No EOF requirement, bounds normalization, identity substitution or clamp.
 * Parsing success does not mean a callback was applied or even authorized. */
int anco_wms_callback_read(GBinderReader *reader,guint code,AncoWmsCallbackMessage *out);
#endif
