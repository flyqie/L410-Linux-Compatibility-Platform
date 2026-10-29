/* Linux editor side of the stock AnCo IMF broker ABI (SA 66827).
 * The stock guest HwInputMethodManagerService ultimately obtains the same
 * broker SA. A separate callback endpoint must hold SA 66875 and bind every
 * reverse text operation to the same focused Wayland editor.
 */
#include "anco_imf_client_v340.h"

#include <gbinder.h>
#include "gbinder_ipc.h"
#include "gbinder_local_request_p.h"
#include <limits.h>

#define IMF_BROKER_SA 66827
#define IMF_BROKER_IFACE "ohos.anco.system.imf.IAncoInputMethodManager"

struct AncoImfClient {
    GBinderIpc *ipc;
    GBinderRemoteObject *object;
    GBinderClient *binder;
    bool attached;
    bool shown;
    bool legacy_cleanup;
};

static GBinderRemoteObject *lookup_broker(GBinderIpc *ipc)
{
    GBinderLocalRequest *req = gbinder_local_request_new(
        gbinder_ipc_io(ipc), gbinder_ipc_protocol(ipc), NULL);
    if (!req) return NULL;
    gbinder_local_request_append_int32(req, 0x100);
    gbinder_local_request_append_int32(req, 0);
    gbinder_local_request_append_string16(req, "ohos.samgr.accessToken");
    gbinder_local_request_append_int32(req, IMF_BROKER_SA);
    int transport = -1;
    GBinderRemoteReply *reply = gbinder_ipc_transact_sync_reply_main(
        ipc, 0, 2, req, &transport);
    GBinderRemoteObject *object = reply && !transport ?
        gbinder_remote_reply_read_object(reply) : NULL;
    if (reply) gbinder_remote_reply_unref(reply);
    gbinder_local_request_unref(req);
    return object;
}

static bool transact(AncoImfClient *client, guint code,
                     GBinderLocalRequest *req, int32_t *status,
                     bool *consumed, AncoImfCallResult *details)
{
    if (details) *details = (AncoImfCallResult){false, 0, false, -1};
    if (status) *status = -1;
    if (consumed) *consumed = false;
    if (!req) return false;
    int transport = -1;
    GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(
        client->binder, code, req, &transport);
    if (details) {
        details->attempted = true;
        details->transport = transport;
    }
    bool decoded = false;
    if (reply && !transport) {
        GBinderReader reader;
        gint32 result = -1;
        gint32 used = 0;
        gbinder_remote_reply_init_reader(reply, &reader);
        decoded = gbinder_reader_read_int32(&reader, &result);
        /* The guest Parcel.readBoolean() reads a second int32. The
         * convenience read_int32(reply) restarts at offset zero. */
        if (decoded && code == 5)
            decoded = gbinder_reader_read_int32(&reader, &used);
        if (decoded) {
            if (details) { details->decoded = true; details->status = result; }
            if (status) *status = result;
            if (consumed) *consumed = used != 0;
        }
    }
    if (reply) gbinder_remote_reply_unref(reply);
    gbinder_local_request_unref(req);
    return decoded;
}

static bool simple_call(AncoImfClient *client, guint code, int32_t *status)
{
    if (status) *status = -1;
    if (!client) return false;
    return transact(client, code, gbinder_client_new_request(client->binder),
                    status, NULL, NULL);
}

AncoImfClient *anco_imf_client_open(void)
{
    AncoImfClient *client = g_new0(AncoImfClient, 1);
    client->legacy_cleanup = true;
    client->ipc = gbinder_ipc_new("/dev/binder", "aidl2");
    if (client->ipc) client->object = lookup_broker(client->ipc);
    if (client->object)
        client->binder = gbinder_client_new(client->object, IMF_BROKER_IFACE);
    if (!client->binder) {
        anco_imf_client_close(client);
        return NULL;
    }
    return client;
}

AncoImfClient *anco_imf_client_from_remote(GBinderRemoteObject *remote)
{
    if (!remote) return NULL;
    AncoImfClient *client = g_new0(AncoImfClient, 1);
    client->object = gbinder_remote_object_ref(remote);
    client->binder = gbinder_client_new(remote, IMF_BROKER_IFACE);
    if (!client->binder) {
        anco_imf_client_close(client);
        return NULL;
    }
    return client;
}

void anco_imf_client_close(AncoImfClient *client)
{
    if (!client) return;
    int32_t status;
    if (client->legacy_cleanup && client->shown) anco_imf_hide(client, &status);
    if (client->legacy_cleanup && client->attached) anco_imf_detach(client, &status);
    if (client->binder) gbinder_client_unref(client->binder);
    if (client->object) gbinder_remote_object_unref(client->object);
    if (client->ipc) gbinder_ipc_unref(client->ipc);
    g_free(client);
}

bool anco_imf_attach(AncoImfClient *client, bool show_keyboard,
                     int32_t input_pattern, int32_t enter_key_type,
                     int32_t input_option, int32_t *status)
{
    AncoImfCallResult result;
    bool decoded = anco_imf_attach_with_result(client, show_keyboard,
        input_pattern, enter_key_type, input_option, &result);
    if (status) *status = result.status;
    return decoded;
}

bool anco_imf_attach_with_result(AncoImfClient *client, bool show_keyboard,
                     int32_t input_pattern, int32_t enter_key_type,
                     int32_t input_option, AncoImfCallResult *details)
{
    if (details) *details = (AncoImfCallResult){false, 0, false, -1};
    /* The original onStartInput may attach again for a new editor, and its
     * integer attributes may carry sentinels. Do not narrow that ABI. */
    if (!client) return false;
    GBinderLocalRequest *req = gbinder_client_new_request(client->binder);
    if (!req) return false;
    /* Guest Parcel.writeBoolean writes an int32. The current 6.0 stub
     * implements original attachInput as transaction 1. */
    gbinder_local_request_append_int32(req, show_keyboard ? 1 : 0);
    gbinder_local_request_append_int32(req, (guint32)input_pattern);
    gbinder_local_request_append_int32(req, (guint32)enter_key_type);
    gbinder_local_request_append_int32(req, (guint32)input_option);
    int32_t result = -1;
    bool decoded = transact(client, 1, req, &result, NULL, details);
    if (decoded && result == 0) client->attached = true;
    return decoded;
}

bool anco_imf_attach_attribute(AncoImfClient *client,
                     const AncoImfInputAttribute *attribute,
                     AncoImfCallResult *details)
{
    if (details) *details = (AncoImfCallResult){false, 0, false, -1};
    if (!client) return false;
    if (attribute) {
        if (attribute->command_count > INT_MAX ||
            (attribute->command_count && !attribute->commands)) return false;
        for (size_t i = 0; i < attribute->command_count; ++i) {
            const AncoImfPrivateCommand *command = attribute->commands[i];
            if (!command) continue;
            if (command->key && !g_utf8_validate(command->key, -1, NULL)) return false;
            if (command->data && command->data->value &&
                command->data->value->string_value &&
                !g_utf8_validate(command->data->value->string_value, -1, NULL)) return false;
        }
    }
    GBinderLocalRequest *req = gbinder_client_new_request(client->binder);
    if (!req) return false;
    gbinder_local_request_append_int32(req, attribute ? 1 : 0);
    if (attribute) {
        gbinder_local_request_append_int32(req, attribute->show_keyboard ? 1 : 0);
        gbinder_local_request_append_int32(req, (guint32)attribute->pattern);
        gbinder_local_request_append_int32(req, (guint32)attribute->enter_key_type);
        gbinder_local_request_append_int32(req, (guint32)attribute->option);
        gbinder_local_request_append_int32(req, (guint32)attribute->command_count);
        for (size_t i = 0; i < attribute->command_count; ++i) {
            const AncoImfPrivateCommand *command = attribute->commands[i];
            /* Original attribute writer skips null entries without a marker,
             * while retaining the list size. Do not substitute code10's format. */
            if (!command) continue;
            gbinder_local_request_append_string16(req, command->key);
            if (!command->data) continue;
            gbinder_local_request_append_int32(req, (guint32)command->data->type);
            const AncoImfPrivateValue *value = command->data->value;
            if (!value) continue;
            gbinder_local_request_append_string16(req, value->string_value);
            gbinder_local_request_append_int32(req, value->bool_value ? 1 : 0);
            gbinder_local_request_append_int32(req, (guint32)value->int_value);
        }
    }
    int32_t result = -1;
    bool decoded = transact(client, 9, req, &result, NULL, details);
    if (decoded && result == 0) client->attached = true;
    return decoded;
}

bool anco_imf_attach_select(AncoImfClient *client, bool *support_new,
                     bool requested_show, const AncoImfInputAttribute *attribute,
                     AncoImfAttachResult *result)
{
    AncoImfAttachResult local = {
        {false, 0, false, -1}, {false, 0, false, -1}, false
    };
    bool decoded = false;
    if (client && support_new && attribute) {
        if (*support_new) {
            decoded = anco_imf_attach_attribute(client, attribute, &local.first);
        } else {
            decoded = anco_imf_attach_with_result(client, requested_show,
                attribute->pattern, attribute->enter_key_type, attribute->option,
                &local.first);
        }
        /* Match the original catch-time state check and its exact exception,
         * not UNKNOWN_TRANSACTION(-74), a business result, or generic failure. */
        if (*support_new && local.first.attempted && local.first.transport == 305) {
            *support_new = false;
            local.used_fallback = true;
            decoded = anco_imf_attach_with_result(client, attribute->show_keyboard,
                attribute->pattern, attribute->enter_key_type, attribute->option,
                &local.fallback);
        }
    }
    if (result) *result = local;
    return decoded;
}

bool anco_imf_show_select(AncoImfClient *client, bool *support_new_show,
                     int32_t motion_tool_type, AncoImfShowResult *result)
{
    AncoImfShowResult local = {
        {false, 0, false, -1}, {false, 0, false, -1}, false
    };
    bool decoded = false;
    int32_t status = -1;
    if (client && support_new_show) {
        GBinderLocalRequest *req = gbinder_client_new_request(client->binder);
        if (req && *support_new_show) {
            int32_t reason;
            switch (motion_tool_type) {
            case 1: reason = 2; break;
            case 2: case 4: reason = 20; break;
            case 3: reason = 1; break;
            default: reason = 0; break;
            }
            gbinder_local_request_append_int32(req, 1);
            gbinder_local_request_append_int32(req, (guint32)reason);
        }
        decoded = transact(client, *support_new_show ? 8 : 3, req,
                           &status, NULL, &local.first);
        if (*support_new_show && local.first.attempted && local.first.transport == 305) {
            *support_new_show = false;
            local.used_fallback = true;
            decoded = transact(client, 3, gbinder_client_new_request(client->binder),
                               &status, NULL, &local.fallback);
        }
        if (decoded && status == 0) client->shown = true;
    }
    if (result) *result = local;
    return decoded;
}

bool anco_imf_hide_direct(AncoImfClient *client, AncoImfCallResult *details)
{
    if (details) *details = (AncoImfCallResult){false, 0, false, -1};
    if (!client) return false;
    int32_t status = -1;
    bool decoded = transact(client, 4, gbinder_client_new_request(client->binder),
                            &status, NULL, details);
    if (decoded && status == 0) client->shown = false;
    return decoded;
}

bool anco_imf_detach_direct(AncoImfClient *client, AncoImfCallResult *details)
{
    if (details) *details = (AncoImfCallResult){false, 0, false, -1};
    if (!client) return false;
    int32_t status = -1;
    bool decoded = transact(client, 2, gbinder_client_new_request(client->binder),
                            &status, NULL, details);
    if (decoded && status == 0) client->attached = false;
    return decoded;
}

bool anco_imf_show(AncoImfClient *client, int32_t *status)
{
    if (status) *status = -1;
    if (!client || !client->attached) return false;
    if (client->shown) {
        if (status) *status = 0;
        return true;
    }
    int32_t result = -1;
    bool decoded = simple_call(client, 3, &result);
    if (status) *status = result;
    if (decoded && result == 0) client->shown = true;
    return decoded;
}

bool anco_imf_hide(AncoImfClient *client, int32_t *status)
{
    if (status) *status = -1;
    if (!client || !client->attached) return false;
    if (!client->shown) {
        if (status) *status = 0;
        return true;
    }
    int32_t result = -1;
    bool decoded = simple_call(client, 4, &result);
    if (status) *status = result;
    if (decoded && result == 0) client->shown = false;
    return decoded;
}

bool anco_imf_detach(AncoImfClient *client, int32_t *status)
{
    if (status) *status = -1;
    if (!client || !client->attached) return false;
    if (client->shown) {
        int32_t hidden = -1;
        if (!anco_imf_hide(client, &hidden) || hidden != 0) return false;
    }
    int32_t result = -1;
    bool decoded = simple_call(client, 2, &result);
    if (status) *status = result;
    if (decoded && result == 0) client->attached = false;
    return decoded;
}

bool anco_imf_dispatch_key(AncoImfClient *client,
                           const AncoImfKeyAttribute *attribute,
                           const AncoImfKeyItem *items, size_t item_count,
                           int32_t *status, bool *consumed)
{
    if (status) *status = -1;
    if (consumed) *consumed = false;
    if (!client || !attribute ||
        item_count > INT_MAX || (item_count && !items) ||
        (attribute->action != 2 && attribute->action != 3) ||
        attribute->num_lock < 0 || attribute->num_lock > 1 ||
        attribute->caps_lock < 0 || attribute->caps_lock > 1 ||
        attribute->scroll_lock < 0 || attribute->scroll_lock > 1)
        return false;
    for (size_t i = 0; i < item_count; ++i)
        if (items[i].pressed < 0 || items[i].pressed > 1) return false;
    GBinderLocalRequest *req = gbinder_client_new_request(client->binder);
    if (!req) return false;
    gbinder_local_request_append_int32(req, 1); /* attribute present */
    gbinder_local_request_append_int32(req, (guint32)attribute->id);
    gbinder_local_request_append_int64(req, (guint64)attribute->event_time);
    gbinder_local_request_append_int32(req, (guint32)attribute->action);
    gbinder_local_request_append_int32(req, (guint32)attribute->device_id);
    gbinder_local_request_append_int32(req, (guint32)attribute->key_code);
    gbinder_local_request_append_int32(req, (guint32)attribute->display_id);
    gbinder_local_request_append_int32(req, (guint32)attribute->num_lock);
    gbinder_local_request_append_int32(req, (guint32)attribute->caps_lock);
    gbinder_local_request_append_int32(req, (guint32)attribute->scroll_lock);
    gbinder_local_request_append_int32(req, (guint32)item_count);
    for (size_t i = 0; i < item_count; ++i) {
        gbinder_local_request_append_int32(req, 1); /* item present */
        gbinder_local_request_append_int32(req, (guint32)items[i].pressed);
        gbinder_local_request_append_int64(req, (guint64)items[i].down_time);
        gbinder_local_request_append_int32(req, (guint32)items[i].device_id);
        gbinder_local_request_append_int32(req, (guint32)items[i].key_code);
        gbinder_local_request_append_int64(req, (guint64)items[i].unicode);
    }
    return transact(client, 5, req, status, consumed, NULL);
}

bool anco_imf_cursor_update(AncoImfClient *client, double position_x,
                            double position_y, double height,
                            int32_t *status)
{
    if (status) *status = -1;
    if (!client) return false;
    GBinderLocalRequest *req = gbinder_client_new_request(client->binder);
    if (!req) return false;
    gbinder_local_request_append_double(req, position_x);
    gbinder_local_request_append_double(req, position_y);
    gbinder_local_request_append_double(req, height);
    return transact(client, 6, req, status, NULL, NULL);
}

bool anco_imf_selection_change(AncoImfClient *client, const char *utf8_text,
                               int32_t old_begin, int32_t old_end,
                               int32_t new_begin, int32_t new_end,
                               int32_t *status)
{
    if (status) *status = -1;
    if (!client || (utf8_text &&
        !g_utf8_validate(utf8_text, -1, NULL))) return false;
    GBinderLocalRequest *req = gbinder_client_new_request(client->binder);
    if (!req) return false;
    /* gbinder converts UTF-8 to the guest Parcel's UTF-16 string. The
     * four indices must already have been converted from Wayland bytes. */
    gbinder_local_request_append_string16(req, utf8_text);
    gbinder_local_request_append_int32(req, (guint32)old_begin);
    gbinder_local_request_append_int32(req, (guint32)old_end);
    gbinder_local_request_append_int32(req, (guint32)new_begin);
    gbinder_local_request_append_int32(req, (guint32)new_end);
    return transact(client, 7, req, status, NULL, NULL);
}
