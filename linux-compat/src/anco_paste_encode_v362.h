/* Original AnCo PasteData TLV, limited to one local plain-text record.
 * Mirrors guest PasteData/PasteDataProperty/PasteDataRecord/PasteDataEntry.
 */
#ifndef L410_ANCO_PASTE_ENCODE_V362_H
#define L410_ANCO_PASTE_ENCODE_V362_H
#include <glib.h>
#include <stdint.h>
#include <string.h>

static void anco_tlv_field(GByteArray *out, uint16_t tag, const void *value, guint32 length)
{
    guint8 head[6] = { tag & 255, tag >> 8, length & 255,
        (length >> 8) & 255, (length >> 16) & 255, (length >> 24) & 255 };
    g_byte_array_append(out, head, sizeof head);
    if (length) g_byte_array_append(out, value, length);
}
static void anco_tlv_nested(GByteArray *out, uint16_t tag, GByteArray *inner)
{ anco_tlv_field(out, tag, inner->data, (guint32)inner->len); }
static void anco_tlv_string(GByteArray *out, uint16_t tag, const char *text)
{ anco_tlv_field(out, tag, text, (guint32)strlen(text)); }
static void anco_tlv_u32(GByteArray *out, uint16_t tag, guint32 value)
{
    guint8 n[4] = { value & 255, (value >> 8) & 255,
        (value >> 16) & 255, (value >> 24) & 255 };
    anco_tlv_field(out, tag, n, sizeof n);
}
static void anco_tlv_bool(GByteArray *out, uint16_t tag, int value)
{
    guint8 n = value ? 1 : 0;
    anco_tlv_field(out, tag, &n, 1);
}
static GByteArray *anco_encode_plain_text(const guint8 *text, gsize length)
{
    if ((!text && length) || length > 4u * 1024u * 1024u || length > G_MAXUINT32 - 256) return NULL;
    if (!text) text = (const guint8 *)"";
    GByteArray *property = g_byte_array_new(), *mime_list = g_byte_array_new();
    GByteArray *variant = g_byte_array_new(), *entry = g_byte_array_new();
    GByteArray *entries = g_byte_array_new(), *record = g_byte_array_new();
    GByteArray *records = g_byte_array_new(), *top = g_byte_array_new();
    anco_tlv_field(property, 257, "", 0); /* empty additions, defaults retained */
    anco_tlv_string(mime_list, 0, "text/plain");
    anco_tlv_nested(property, 258, mime_list);
    anco_tlv_string(property, 259, "");
    anco_tlv_bool(property, 260, 1); /* stay on this device */
    guint8 timestamp[8] = {0};
    anco_tlv_field(property, 261, timestamp, sizeof timestamp);
    anco_tlv_u32(property, 262, 1); /* ShareOption::LocalDevice */
    anco_tlv_u32(property, 263, 0);
    anco_tlv_bool(property, 264, 0);
    anco_tlv_string(record, 257, "text/plain");
    anco_tlv_field(record, 259, "", 0); /* null Want */
    anco_tlv_field(record, 260, text, (guint32)length); /* old plain field */
    anco_tlv_field(record, 261, "", 0); /* null URI */
    anco_tlv_bool(record, 265, 0);
    anco_tlv_u32(variant, 4, 5); /* String variant */
    anco_tlv_field(variant, 5, text, (guint32)length);
    anco_tlv_string(entry, 257, "general.plain-text");
    anco_tlv_string(entry, 258, "text/plain");
    anco_tlv_nested(entry, 259, variant);
    anco_tlv_nested(entries, 0, entry);
    anco_tlv_nested(record, 271, entries);
    anco_tlv_nested(records, 0, record);
    anco_tlv_nested(top, 257, property);
    anco_tlv_nested(top, 258, records);
    anco_tlv_bool(top, 259, 0);
    anco_tlv_bool(top, 260, 0);
    anco_tlv_u32(top, 267, 0); /* original guest userId */
    g_byte_array_unref(property); g_byte_array_unref(mime_list);
    g_byte_array_unref(variant); g_byte_array_unref(entry);
    g_byte_array_unref(entries); g_byte_array_unref(record);
    g_byte_array_unref(records);
    return top;
}
#endif
