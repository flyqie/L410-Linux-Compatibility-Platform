/* Original AncoWmsLayoutParams.fromLp calculation, Linux semantic inputs. */
#ifndef ANCO_WMS_LAYOUT_H
#define ANCO_WMS_LAYOUT_H
#include "anco_wms_transport.h"

typedef struct { gint32 left, top, right, bottom; } AncoWmsRect;
typedef struct {
    gboolean exiting, client_visible, visible_requested;
    gboolean has_visible_secure_window, can_show_when_locked, can_turn_screen_on;
} AncoWmsActivityLayoutState;
typedef struct {
    /* Actual WindowState-equivalent getters, not client assertions. Incoming
     * LP type and window.getAttrs().type are separate original values.
     */
    gboolean anco_enabled, has_frame;
    gint32 window_type;
    AncoWmsRect frame, surface_insets;
    gboolean has_parent;
    gint32 parent_window_id;
    gfloat parent_global_scale;
    gboolean has_task;
    AncoWmsRect task_bounds;
    /* Snapshot of the actual Task's Activity getter results. The caller must
     * supply real evaluated visibility/security/keyguard/power semantics;
     * this module does not implement or guess those getters/policies.
     */
    const AncoWmsActivityLayoutState *activities;
    gsize activity_count;
} AncoWmsLayoutState;

/* attrs supplies ORIGINAL incoming LP scalar values. Its width/height/x/y/
 * parent_window are intentionally NOT copied: original constructor derives
 * them from WindowState frame/insets/parent/Task. Explicit NULL-frame branch
 * keeps original Java-zero geometry/parent fields. App/task flags aggregation
 * still runs. No arbitrary clamp, guessed defaults, DPI rule or host call.
 * Requires non-NULL real state equivalent as in valid WMS window callers.
 * Errors leave output untouched; aliasing attrs==out is supported.
 * Caller must perform actual role/type/token/admission separately.
 */
int anco_wms_layout_project(const AncoWmsLayout *attrs,
                            const AncoWmsLayoutState *state, AncoWmsLayout *out);
#endif
