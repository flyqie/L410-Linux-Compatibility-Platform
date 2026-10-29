#define WLR_USE_UNSTABLE
#include "anco_wayland_display.h"
#include <json-glib/json-glib.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

int anco_wayland_display_query(const char *native_query,
    const AncoGraphicDisplay *displays, size_t count, AncoWaylandDisplayInfo *info)
{
    if (!native_query || !displays || !count || !info) return -EINVAL;
    char *argv[] = {(char *)native_query, NULL};
    char *output = NULL, *errors = NULL;
    int wait_status = 0;
    GError *error = NULL;
    gboolean ok = g_spawn_sync(NULL, argv, NULL, 0, NULL, NULL,
        &output, &errors, &wait_status, &error);
    if (ok) ok = g_spawn_check_wait_status(wait_status, &error);
    printf("RUNTIME_NATIVE_DISPLAY_QUERY completed=%d wait_status=%d stderr_bytes=%zu\n",
        ok, wait_status, errors ? strlen(errors) : 0);
    g_free(errors);
    if (!ok) {
        if (error) fprintf(stderr, "RUNTIME_NATIVE_DISPLAY_ERROR %s\n", error->message);
        g_clear_error(&error); g_free(output); return -EIO;
    }
    JsonParser *parser = json_parser_new();
    ok = output && json_parser_load_from_data(parser, output, -1, &error);
    g_free(output);
    JsonNode *root = ok ? json_parser_get_root(parser) : NULL;
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) ok = FALSE;
    const AncoGraphicDisplay *matched = NULL;
    AncoWaylandDisplayInfo result = {0};
    if (ok) {
        JsonObject *object = json_node_get_object(root);
        const char *fields[] = {"id", "width", "height", "virtualPixelRatio",
            "densityDpi", "scaledDensity"};
        for (size_t i = 0; i < G_N_ELEMENTS(fields); ++i)
            if (!json_object_has_member(object, fields[i])) ok = FALSE;
        if (ok) {
            const gint64 id = json_object_get_int_member(object, "id");
            const gint64 width = json_object_get_int_member(object, "width");
            const gint64 height = json_object_get_int_member(object, "height");
            const gint64 density = json_object_get_int_member(object, "densityDpi");
            result.scale = json_object_get_double_member(object, "virtualPixelRatio");
            result.scaled_density = json_object_get_double_member(object, "scaledDensity");
            ok = id >= 0 && density > 0 && density <= INT_MAX &&
                isfinite(result.scale) && result.scale > 0 && result.scale <= INT_MAX &&
                isfinite(result.scaled_density) && result.scaled_density > 0;
            result.density_dpi = ok ? (int)density : 0;
            if (json_object_has_member(object, "rotation")) {
                const gint64 rotation = json_object_get_int_member(object, "rotation");
                if (rotation < 0 || rotation > 3) ok = FALSE;
                else { result.rotation = (int)rotation; result.rotation_known = TRUE; }
            }
            for (size_t i = 0; ok && i < count; ++i)
                if (displays[i].id == (uint64_t)id && displays[i].width == width &&
                    displays[i].height == height) { matched = &displays[i]; break; }
        }
    }
    g_clear_error(&error); g_object_unref(parser);
    if (!ok) return -EPROTO;
    if (!matched) return -ESTALE;
    result.display = *matched;
    printf("RUNTIME_NATIVE_DISPLAY_STATE rotation_known=%d rotation=%d vpr=%.9g\n",
        result.rotation_known, result.rotation_known ? result.rotation : -1, result.scale);
    *info = result;
    return 0;
}

/* Metadata only. Real rendering/presentation belongs to the original AnCo
 * per-window queue, not a whole Wayland desktop framebuffer. Reject commits;
 * never fabricate scanout, frame events, presentation or fence completion. */
static bool metadata_commit(struct wlr_output *output,
    const struct wlr_output_state *state)
{
    (void)output; (void)state;
    return false;
}
static const struct wlr_output_impl metadata_impl = {.commit = metadata_commit};

struct wlr_output *anco_wayland_display_create(struct wl_display *display,
    struct wlr_output_layout *layout, const AncoWaylandDisplayInfo *info)
{
    if (!display || !layout || !info || info->display.width <= 0 ||
        info->display.height <= 0 || !info->display.refresh_hz ||
        info->display.refresh_hz > INT_MAX / 1000) return NULL;
    struct wlr_output *output = calloc(1, sizeof(*output));
    if (!output) return NULL;
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_state_set_custom_mode(&state, info->display.width,
        info->display.height, (int)info->display.refresh_hz * 1000);
    /* Xwayland allocates its root framebuffer from the output's effective
     * resolution. Publishing the fractional host VPR here makes legacy X11
     * clients rasterize at 1600x1066 and forces the AnCo layer to enlarge the
     * finished pixels to 2160x1440. Keep the protocol output pixel-native;
     * individual Wayland surfaces receive preferred/fractional scale events,
     * while X11 toolkit sizing is published through Xft.dpi at XWM ready. */
    wlr_output_state_set_scale(&state, 1.0f);
    wlr_output_init(output, NULL, &metadata_impl,
        wl_display_get_event_loop(display), &state);
    wlr_output_state_finish(&state);
    char name[64];
    snprintf(name, sizeof(name), "AnCo-%llu", (unsigned long long)info->display.id);
    wlr_output_set_name(output, name);
    wlr_output_set_description(output, "Original HarmonyOS default display");
    /* Original physical xdpi/ydpi are distinct from UI scale/densityDpi. */
    const double mm_width = info->display.width * 25.4 / info->display.xdpi;
    const double mm_height = info->display.height * 25.4 / info->display.ydpi;
    if (isfinite(mm_width) && mm_width > 0 && mm_width < INT_MAX)
        output->phys_width = (int)lround(mm_width);
    if (isfinite(mm_height) && mm_height > 0 && mm_height < INT_MAX)
        output->phys_height = (int)lround(mm_height);
    if (!wlr_output_layout_add(layout, output, 0, 0) || !output->global) {
        wlr_output_destroy(output); return NULL;
    }
    int width, height;
    wlr_output_effective_resolution(output, &width, &height);
    printf("RUNTIME_WAYLAND_OUTPUT id=%llu mode=%dx%d hz=%u output_scale=%.9g host_vpr=%.9g preferred_buffer_scale=%d logical=%dx%d density_dpi=%d presentation=0\n",
        (unsigned long long)info->display.id, output->width, output->height,
        info->display.refresh_hz, (double)output->scale,info->scale,
        (int)ceil(info->scale),width, height, info->density_dpi);
    return output;
}
