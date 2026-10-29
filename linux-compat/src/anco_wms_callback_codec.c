#include "anco_wms_callback_codec.h"
#include "gbinder_reader_p.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Checked libgbinder reader.c prefix, as used by the actual remote reader.
 * data->objects is the original whole parcel object table, not the current
 * object cursor. No Binder object is acquired, removed or interpreted here. */
typedef struct {
    const guint8 *start, *end, *ptr;
    const GBinderReaderData *data;
} CheckedReaderPrefix;
G_STATIC_ASSERT(sizeof(GBinderReader) >= sizeof(CheckedReaderPrefix));
G_STATIC_ASSERT(offsetof(CheckedReaderPrefix, data) == 3 * sizeof(void *));

static gboolean object_free_range(const CheckedReaderPrefix *p, gsize size)
{
    gboolean valid = TRUE;
    /* Retained ARM64 Parcel::validateReadData uses exactly 24 bytes for each
     * flat object. Scan without sorting/mutating the borrowed object table.
     * Subtraction avoids wrapping the end address in the overlap comparison. */
    if (p->data && p->data->objects) {
        const uintptr_t cursor = (uintptr_t)p->ptr;
        for (void **obj = p->data->objects; *obj; ++obj) {
            const uintptr_t begin = (uintptr_t)*obj;
            if (begin <= cursor ? cursor - begin < 24 : begin - cursor < size) {
                valid = FALSE;
                break;
            }
        }
    }
    return valid;
}
static gboolean scalar(GBinderReader *r, void *out, gsize size)
{
    memset(out, 0, size);
    if (gbinder_reader_bytes_remaining(r) < size) return FALSE;
    CheckedReaderPrefix p;
    memcpy(&p, r, sizeof(p));
    gboolean valid = object_free_range(&p, size);
    if (valid) memcpy(out, p.ptr, size);
    /* Original getters advance even on object overlap, unlike short data.
     * Use actual reader operations to retain its private state/ownership. */
    for (gsize n = 0; n < size; n += 4) gbinder_reader_read_uint32(r, NULL);
    return valid;
}

gint32 anco_callback_read_int32(GBinderReader *r)
{ gint32 value; scalar(r, &value, 4); return value; }

const guint8 *anco_callback_read_string16(GBinderReader *r, gsize *length)
{
    if (!length) return NULL;
    *length = 0;
    if (!r) return NULL;
    gint32 count;
    /* Original scalar failure supplies zero and still attempts the empty
     * string payload; overlapping length words advance, short ones do not. */
    scalar(r, &count, 4);
    if (count < 0 || (guint32)count > 0x3ffffffeu) return NULL;
    const gsize bytes = ((gsize)count + 1) * 2;
    const gsize padded = (bytes + 3) & ~(gsize)3;
    if (gbinder_reader_bytes_remaining(r) < padded) return NULL;
    CheckedReaderPrefix p;
    memcpy(&p, r, sizeof(p));
    const guint8 *text = p.ptr;
    const gboolean valid = object_free_range(&p, padded);
    p.ptr += padded;
    memcpy((guint8 *)r + offsetof(CheckedReaderPrefix, ptr), &p.ptr, sizeof(p.ptr));
    if (!valid || text[bytes - 2] || text[bytes - 1]) return NULL;
    *length = (gsize)count;
    return text;
}

gboolean anco_callback_read_header(GBinderReader *r, guint flags,
    const char *expected, AncoCallbackHeader *out)
{
    if (!r || !out || !expected) return FALSE;
    AncoCallbackHeader header = {0};
    gint32 length;
    /* Original getter defaults are used even for short/overlapping headers.
     * StrictMode and WorkSource are metadata, NOT sender authorization. */
    scalar(r, &header.strict_policy, 4);
    if (flags & 1) header.strict_policy = 0;
    scalar(r, &header.work_source_uid, 4);
    *out = header;
    scalar(r, &length, 4);
    if (length < 0 || (guint32)length > 0x3ffffffeu) return FALSE;
    const gsize bytes = ((gsize)length + 1) * 2;
    const gsize padded = (bytes + 3) & ~(gsize)3;
    if (gbinder_reader_bytes_remaining(r) < padded) return FALSE;
    CheckedReaderPrefix p;
    memcpy(&p, r, sizeof(p));
    const guint8 *text = p.ptr;
    const gboolean valid = object_free_range(&p, padded);
    /* Original readInplace advances after sufficient bytes even if validation
     * fails. Update only the checked reader cursor, retaining its object table.
     * Avoid allocating or iterating through an untrusted string length. */
    p.ptr += padded;
    memcpy((guint8 *)r + offsetof(CheckedReaderPrefix, ptr), &p.ptr, sizeof(p.ptr));
    if (!valid || text[bytes - 2] || text[bytes - 1]) return FALSE;
    const gsize expected_length = strlen(expected);
    if ((gsize)length != expected_length) return FALSE;
    for (gsize i = 0; i < expected_length; ++i)
        if (text[2*i] != (guint8)expected[i] || text[2*i + 1]) return FALSE;
    return TRUE;
}
gboolean anco_wms_callback_read_header(GBinderReader *r, guint flags,
                                      AncoWmsCallbackHeader *out)
{ return anco_callback_read_header(r, flags, ANCO_WMS_CALLBACK_IFACE, out); }
static gboolean boolean(GBinderReader *r,gboolean *out)
{gint32 value;gboolean valid=scalar(r,&value,4);*out=value!=0;return valid;}
int anco_wms_callback_read(GBinderReader *r,guint code,AncoWmsCallbackMessage *out)
{
    if(!r||!out)return -EINVAL;
    AncoWmsCallbackMessage m={.code=code};
    gboolean complete=TRUE;
#define I(field) do { if(!scalar(r,&m.data.field,4))complete=FALSE; } while(0)
#define B(field) do { if(!boolean(r,&m.data.field))complete=FALSE; } while(0)
    switch(code){
    case 1:B(container_focus.focused);I(container_focus.display);break;
    case 2:I(mode.task);I(mode.mode);break;
    case 3:
        I(bounds.task);I(bounds.left);I(bounds.top);I(bounds.right);I(bounds.bottom);
        if(!scalar(r,&m.data.bounds.scale,4))complete=FALSE;
        break;
    case 4:B(task_focus.focused);I(task_focus.task);I(task_focus.display);break;
    case 5:I(mid_scene.task);B(mid_scene.mid_scene);break;
    case 6:I(visibility.task);I(visibility.state);break;
    case 7:
        B(smart_edge.present);
        if(m.data.smart_edge.present){
            I(smart_edge.state);I(smart_edge.screen);I(smart_edge.left);I(smart_edge.top);
            I(smart_edge.width);I(smart_edge.height);
            if(!scalar(r,&m.data.smart_edge.scale_x,8))complete=FALSE;
            if(!scalar(r,&m.data.smart_edge.scale_y,8))complete=FALSE;
        }
        break;
    default:return -ENOTSUP;
    }
#undef I
#undef B
    *out=m;return complete?0:-EBADMSG;
}
