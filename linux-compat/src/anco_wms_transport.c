#include "anco_wms_transport.h"
#include <errno.h>
#include <string.h>

static int message_supported(const AncoWmsMessage *m)
{
    if (!m) return 0;
    switch (m->code) {
    case ANCO_WMS_CREATE_WINDOW: case ANCO_WMS_SHOW_WINDOW:
    case ANCO_WMS_HIDE_WINDOW: case ANCO_WMS_REMOVE_WINDOW:
    case ANCO_WMS_UPDATE_LAYOUT: case ANCO_WMS_CREATE_ACTIVITY:
    case ANCO_WMS_REMOVE_ACTIVITY: case ANCO_WMS_MOVE_ACTIVITY_TO_TOP:
    case ANCO_WMS_FRAME_LAYOUT_FINISH: return 0;
    default: return -ENOTSUP;
    }
}

int anco_wms_append(GBinderLocalRequest *request, const AncoWmsMessage *m)
{
    if (!request) return -EINVAL;
    const int error = message_supported(m);
    if (error) return error;
    GBinderWriter w;
    gbinder_local_request_init_writer(request, &w);
    gbinder_writer_append_int32(&w, m ? 1 : 0);
    if (!m) return 0;
    gbinder_writer_append_int32(&w, m->code);
    switch (m->code) {
    case ANCO_WMS_FRAME_LAYOUT_FINISH:
        gbinder_writer_append_int32(&w, m->data.frame_layout_finish.task);
        break;
    case ANCO_WMS_CREATE_ACTIVITY: case ANCO_WMS_REMOVE_ACTIVITY:
    case ANCO_WMS_MOVE_ACTIVITY_TO_TOP:
        gbinder_writer_append_int32(&w, m->data.activity.activity);
        gbinder_writer_append_int32(&w, m->data.activity.task);
        break;
    case ANCO_WMS_CREATE_WINDOW:
        gbinder_writer_append_int32(&w, m->data.create.window);
        gbinder_writer_append_int32(&w, m->data.create.activity);
        gbinder_writer_append_int32(&w, m->data.create.type);
        gbinder_writer_append_int32(&w, m->data.create.parent);
        gbinder_writer_append_int32(&w, m->data.create.task);
        break;
    case ANCO_WMS_SHOW_WINDOW: case ANCO_WMS_HIDE_WINDOW:
        gbinder_writer_append_int32(&w, m->data.visibility.window);
        gbinder_writer_append_int32(&w, m->data.visibility.activity);
        gbinder_writer_append_int32(&w, m->data.visibility.task);
        gbinder_writer_append_int32(&w, m->data.visibility.reason);
        break;
    case ANCO_WMS_REMOVE_WINDOW:
        gbinder_writer_append_int32(&w, m->data.remove.window);
        gbinder_writer_append_int32(&w, m->data.remove.activity);
        gbinder_writer_append_int32(&w, m->data.remove.task);
        break;
    case ANCO_WMS_UPDATE_LAYOUT: {
        const AncoWmsLayout *p = &m->data.layout.params;
        gint32 float_bits;
        G_STATIC_ASSERT(sizeof(p->screen_brightness) == sizeof(float_bits));
        memcpy(&float_bits, &p->screen_brightness, sizeof(float_bits));
        gbinder_writer_append_int32(&w, m->data.layout.window);
        gbinder_writer_append_int32(&w, m->data.layout.activity);
        gbinder_writer_append_int32(&w, 14); /* LP's original inner marker */
        gbinder_writer_append_int32(&w, p->width);
        gbinder_writer_append_int32(&w, p->height);
        gbinder_writer_append_int32(&w, p->x);
        gbinder_writer_append_int32(&w, p->y);
        gbinder_writer_append_int32(&w, p->type);
        gbinder_writer_append_int32(&w, p->flags);
        gbinder_writer_append_int32(&w, p->layout_in_display_cutout_mode);
        gbinder_writer_append_int32(&w, float_bits);
        gbinder_writer_append_int32(&w, p->screen_orientation);
        gbinder_writer_append_int32(&w, p->system_ui_visibility);
        gbinder_writer_append_int32(&w, p->fit_insets_types);
        gbinder_writer_append_int32(&w, p->fit_insets_sides);
        gbinder_writer_append_bool(&w, p->fit_insets_ignoring_visibility);
        gbinder_writer_append_int32(&w, p->color_mode);
        gbinder_writer_append_int32(&w, p->parent_window);
        gbinder_writer_append_int32(&w, m->data.layout.task);
        break;
    }
    default: g_assert_not_reached();
    }
    return 0;
}

gboolean anco_wms_process_request(GBinderRemoteObject *wms,
                                  const AncoWmsMessage *message, AncoWmsResult *out)
{
    if (!out) return FALSE;
    *out = (AncoWmsResult){0, -1, FALSE, -1};
    out->local_error = message_supported(message);
    if (out->local_error) return FALSE;
    if (!wms) { out->local_error = -ENOTCONN; return FALSE; }
    GBinderClient *client = gbinder_client_new(wms, ANCO_WMS_IFACE);
    GBinderLocalRequest *request = client ? gbinder_client_new_request(client) : NULL;
    if (!request) {
        out->local_error = -ENOMEM;
    } else {
        out->local_error = anco_wms_append(request, message);
        if (!out->local_error) {
            GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(
                client, ANCO_WMS_PROCESS_TRANSACTION, request, &out->transport);
            if (reply && !out->transport) {
                GBinderReader reader;
                gbinder_remote_reply_init_reader(reply, &reader);
                out->decoded = gbinder_reader_read_int32(&reader, &out->service_result);
            }
            if (reply) gbinder_remote_reply_unref(reply);
        }
        gbinder_local_request_unref(request);
    }
    if (client) gbinder_client_unref(client);
    return !out->local_error && !out->transport && out->decoded && !out->service_result;
}
