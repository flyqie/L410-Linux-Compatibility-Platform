/* Text-only decoder for the original AnCo PasteData TLV.
 * All lengths are bounded by the caller's 4 MiB Parcel limit. No payload is
 * logged. The returned text points into the supplied Parcel byte array.
 */
#ifndef L410_ANCO_PASTE_TEXT_V361_H
#define L410_ANCO_PASTE_TEXT_V361_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct paste_span { const uint8_t *data; size_t size; };

static uint16_t paste_u16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1] << 8; }
static uint32_t paste_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int paste_next(struct paste_span *list, uint16_t *tag, struct paste_span *value)
{
    if (list->size < 6) return 0;
    *tag = paste_u16(list->data);
    uint32_t n = paste_u32(list->data + 2);
    if (n > list->size - 6) return 0;
    value->data = list->data + 6;
    value->size = n;
    list->data += 6 + n;
    list->size -= 6 + n;
    return 1;
}

static int paste_equals(struct paste_span s, const char *str)
{
    size_t n = strlen(str);
    return s.size == n && !memcmp(s.data, str, n);
}

static int paste_utf8(struct paste_span s)
{
    for (size_t i = 0; i < s.size;) {
        uint8_t c = s.data[i++];
        if (!c) return 0;
        if (c < 0x80) continue;
        unsigned continuation = c >= 0xc2 && c <= 0xdf ? 1 :
            c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        if (!continuation || continuation > s.size - i) return 0;
        uint8_t first = s.data[i];
        if ((c == 0xe0 && first < 0xa0) || (c == 0xed && first >= 0xa0) ||
            (c == 0xf0 && first < 0x90) || (c == 0xf4 && first >= 0x90)) return 0;
        for (unsigned j = 0; j < continuation; j++)
            if ((s.data[i++] & 0xc0) != 0x80) return 0;
    }
    return 1;
}

static int paste_entry_value(struct paste_span entry, struct paste_span *text)
{
    struct paste_span utd = {0}, mime = {0}, variant = {0}, field;
    uint16_t tag;
    while (entry.size) {
        if (!paste_next(&entry, &tag, &field)) return 0;
        if (tag == 257) utd = field;
        else if (tag == 258) mime = field;
        else if (tag == 259) variant = field;
    }
    if (!paste_equals(utd, "general.plain-text") || !paste_equals(mime, "text/plain")) return 0;
    int type_seen = 0;
    while (variant.size) {
        if (!paste_next(&variant, &tag, &field)) return 0;
        if (tag == 4 && field.size == 4) type_seen = paste_u32(field.data) == 5;
        if (tag == 5 && type_seen && paste_utf8(field)) { *text = field; return 1; }
    }
    return 0;
}

static int paste_record_text(struct paste_span record, struct paste_span *text)
{
    struct paste_span mime = {0}, old_text = {0}, entries = {0}, field;
    uint16_t tag;
    while (record.size) {
        if (!paste_next(&record, &tag, &field)) return 0;
        if (tag == 257) mime = field;
        else if (tag == 260) old_text = field;
        else if (tag == 271) entries = field;
    }
    if (!paste_equals(mime, "text/plain")) return 0;
    while (entries.size) {
        if (!paste_next(&entries, &tag, &field)) return 0;
        if (tag == 0 && paste_entry_value(field, text)) return 1;
    }
    if (old_text.data && paste_utf8(old_text)) { *text = old_text; return 1; }
    return 0;
}

static int anco_paste_text(const uint8_t *bytes, size_t length, struct paste_span *text)
{
    struct paste_span top = { bytes, length }, records = {0}, field;
    uint16_t tag;
    int properties = 0, record_lists = 0;
    *text = (struct paste_span){0};
    while (top.size) {
        if (!paste_next(&top, &tag, &field)) return 0;
        if (tag == 257) properties++;
        if (tag == 258) { records = field; record_lists++; }
    }
    if (properties != 1 || record_lists != 1) return 0;
    while (records.size) {
        if (!paste_next(&records, &tag, &field)) return 0;
        if (tag == 0 && paste_record_text(field, text)) return 1;
    }
    return 0;
}
#endif
