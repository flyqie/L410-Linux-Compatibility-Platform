#include "anco_wms_layout.h"
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

static gint32 bits_signed(guint32 bits)
{
    gint32 result;
    G_STATIC_ASSERT(sizeof(result)==sizeof(bits));
    memcpy(&result,&bits,sizeof(result));
    return result;
}
static gint32 java_add(gint32 a, gint32 b)
{ return bits_signed((guint32)a+(guint32)b); }
static gint32 java_sub(gint32 a, gint32 b)
{ return bits_signed((guint32)a-(guint32)b); }
static gint32 java_scale(gint32 value, gfloat scale)
{
    /* int-to-float, float multiply, float-to-int are three original DEX ops.
     * Explicit single precision avoids double promotion changing truncation;
     * Java's NaN0/saturation avoids C's out-of-range float-cast undefinedness.
     */
    volatile gfloat product=(gfloat)value*scale;
    if (isnan(product)) return 0;
    if (product>=2147483648.0f) return INT32_MAX;
    if (product<=-2147483648.0f) return INT32_MIN;
    return (gint32)product;
}

int anco_wms_layout_project(const AncoWmsLayout *attrs,
                            const AncoWmsLayoutState *s, AncoWmsLayout *out)
{
    if (!attrs || !s || !out) return -EINVAL;
    const gboolean task_flags=attrs->type<2000 && s->has_task;
    if (task_flags && s->activity_count && !s->activities) return -EINVAL;
    AncoWmsLayout p=*attrs;
    /* Original Java constructor does not initialize these from raw LP. */
    p.width=p.height=p.x=p.y=p.parent_window=0;
    guint32 flags=((guint32)attrs->flags & ~2048u) | 1024u;
    if (s->has_frame) {
        p.width=java_add(java_add(java_sub(s->frame.right,s->frame.left),
            s->surface_insets.left),s->surface_insets.right);
        p.height=java_add(java_add(java_sub(s->frame.bottom,s->frame.top),
            s->surface_insets.top),s->surface_insets.bottom);
        p.x=java_sub(s->frame.left,s->surface_insets.left);
        p.y=java_sub(s->frame.top,s->surface_insets.top);
        p.parent_window=s->anco_enabled && s->has_parent &&
            s->window_type>=1000 && s->window_type<=1999 ? s->parent_window_id : -1;
        if ((s->window_type==1000 || s->window_type==1003) && s->has_parent) {
            p.width=java_scale(p.width,s->parent_global_scale);
            p.height=java_scale(p.height,s->parent_global_scale);
            p.x=java_scale(p.x,s->parent_global_scale);
            p.y=java_scale(p.y,s->parent_global_scale);
        }
        if (s->has_task) {
            p.x=java_sub(p.x,s->task_bounds.left);
            p.y=java_sub(p.y,s->task_bounds.top);
        }
    }
    if (task_flags) {
        flags &= ~8u;
        flags &= ~16u;
        for (gsize i=0;i<s->activity_count;++i) {
            const AncoWmsActivityLayoutState *a=&s->activities[i];
            if (a->exiting || !a->client_visible || !a->visible_requested) continue;
            if (a->has_visible_secure_window) flags |= 8192u;
            if (a->can_show_when_locked) flags |= 524288u;
            if (a->can_turn_screen_on) flags |= 2097152u;
        }
    }
    p.flags=bits_signed(flags);
    *out=p;
    return 0;
}
