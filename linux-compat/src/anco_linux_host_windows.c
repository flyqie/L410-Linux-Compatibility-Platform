#include "anco_linux_host_windows.h"
#include <string.h>

static void window_free(gpointer data)
{
    AncoLinuxHostWindow *window = data;
    if (window->transform) g_array_unref(window->transform);
    if (window->hot_areas) g_array_unref(window->hot_areas);
    if (window->excluded_areas) g_array_unref(window->excluded_areas);
    g_free(window);
}
static GArray *read_array(const unsigned char **cursor, gsize *remaining,
                         guint32 count, guint element_size)
{
    if (count > *remaining / element_size) return NULL;
    GArray *array = g_array_sized_new(FALSE, FALSE, element_size, count);
    if (count) g_array_append_vals(array, *cursor, count);
    *cursor += (gsize)count * element_size;
    *remaining -= (gsize)count * element_size;
    return array;
}
GPtrArray *anco_linux_host_windows_decode(const void *data, gsize bytes)
{
    _Static_assert(sizeof(AncoNativeInputWindows) == 40, "snapshot ABI");
    _Static_assert(sizeof(AncoNativeHostWindowFields) == 32, "record ABI");
    _Static_assert(sizeof(AncoNativeHostRect) == 16, "rect ABI");
    if (!data || bytes < sizeof(AncoNativeInputWindows)) return NULL;
    AncoNativeInputWindows header;
    memcpy(&header, data, sizeof(header));
    if (header.state.code != 2 || header.state.bytes != bytes || header.reserved ||
        header.record_count != header.state.retained_windows) return NULL;
    const unsigned char *cursor = (const unsigned char *)data + sizeof(header);
    gsize remaining = bytes - sizeof(header);
    if (header.record_count > remaining / sizeof(AncoNativeHostWindowFields)) return NULL;
    GPtrArray *records = g_ptr_array_new_with_free_func(window_free);
    for (guint32 i = 0; i < header.record_count; ++i) {
        if (remaining < sizeof(AncoNativeHostWindowFields)) goto invalid;
        AncoLinuxHostWindow *window = g_new0(AncoLinuxHostWindow, 1);
        g_ptr_array_add(records, window);
        memcpy(&window->fields, cursor, sizeof(window->fields));
        cursor += sizeof(window->fields); remaining -= sizeof(window->fields);
        window->transform = read_array(&cursor, &remaining, window->fields.transform_count, sizeof(float));
        if (!window->transform) goto invalid;
        window->hot_areas = read_array(&cursor, &remaining, window->fields.hot_count, sizeof(AncoNativeHostRect));
        if (!window->hot_areas) goto invalid;
        window->excluded_areas = read_array(&cursor, &remaining, window->fields.excluded_count, sizeof(AncoNativeHostRect));
        if (!window->excluded_areas) goto invalid;
    }
    if (remaining) goto invalid;
    return records;
invalid:
    g_ptr_array_unref(records);
    return NULL;
}
