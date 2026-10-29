#ifndef ANCO_IMF_CALLBACK_CODEC_H
#define ANCO_IMF_CALLBACK_CODEC_H
#include "anco_wms_callback_codec.h"
#define ANCO_IMF_CALLBACK_IFACE "ohos.anco.system.imf.IAncoInputMethod"
typedef struct {
    gboolean present, boolean;
    const guint8 *key, *text;
    gsize key_units, text_units;
    gint32 type, integer;
} AncoImfReceivedPrivateCommand;
typedef struct {
    guint code;
    union {
        struct { const guint8 *utf16; gsize units; gint32 cursor; } insert;
        struct { gint32 before, after; } deletion;
        struct { gint32 length, flags; } query;
        gint32 action, status, height, direction;
        struct { gint32 start, end; } selection;
        struct { gint32 key, skip; } select;
        struct { gsize count; AncoImfReceivedPrivateCommand *items; } commands;
    } data;
} AncoImfCallbackMessage;
/* Reader at body, AFTER original interface enforcement. Uses original getter
 * defaults, including null strings; no extra EOF/UID/length policy. Returns 0
 * for a supported projection, not byte validity, business success or wire reply.
 * -EINVAL/-ENOTSUP leave output/reader untouched. -ENOMEM leaves output
 * untouched after consuming the count. Code14 owns an allocated item array;
 * call clear after use, before overwriting output. Strings stay borrowed from
 * actual parcel. No consumer or SA registration. */
int anco_imf_callback_read(GBinderReader *, guint, AncoImfCallbackMessage *);
void anco_imf_callback_message_clear(AncoImfCallbackMessage *);
#endif
