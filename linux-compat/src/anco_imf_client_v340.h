#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The original AnCo manager transaction 5, not evdev or Wayland key codes.
 * Values are produced by VirtKeyEvent -> guest KeyEvent -> AncoIme APK rules.
 */
typedef struct {
    int32_t id;
    int64_t event_time;
    int32_t action;
    int32_t device_id;
    int32_t key_code;
    int32_t display_id;
    int32_t num_lock;
    int32_t caps_lock;
    int32_t scroll_lock;
} AncoImfKeyAttribute;

typedef struct {
    int32_t pressed;
    int64_t down_time;
    int32_t device_id;
    int32_t key_code;
    int64_t unicode;
} AncoImfKeyItem;

typedef struct AncoImfClient AncoImfClient;

/* Per-call Linux diagnostics, never serialized. transport is valid only when
 * attempted; status only when decoded. A decoded status=305 is not the
 * original unsupported-interface transport=305 exception. */
typedef struct {
    bool attempted;
    int32_t transport;
    bool decoded;
    int32_t status;
} AncoImfCallResult;

/* Original ImfPrivateDataValue is not a tagged union: all three fields
 * are serialized. Strings are UTF-8 here and encoded as Parcel string16. */
typedef struct {
    const char *string_value;
    bool bool_value;
    int32_t int_value;
} AncoImfPrivateValue;
typedef struct {
    int32_t type;
    const AncoImfPrivateValue *value;
} AncoImfPrivateData;
typedef struct {
    const char *key;
    const AncoImfPrivateData *data;
} AncoImfPrivateCommand;
typedef struct {
    bool show_keyboard;
    int32_t pattern, enter_key_type, option;
    const AncoImfPrivateCommand *const *commands;
    size_t command_count;
} AncoImfInputAttribute;

/* The process must already have a device-valid AnCo broker/focused-window
 * identity. This library never changes access tokens or host policy.
 */
AncoImfClient *anco_imf_client_open(void);
struct gbinder_remote_object;
/* Retains a supplied actual remote, no SAMgr lookup. Like original asInterface,
 * close only releases this proxy's references, never hides/detaches the editor.
 * Caller must invoke original explicit lifecycle operations as appropriate. */
AncoImfClient *anco_imf_client_from_remote(struct gbinder_remote_object *remote);
void anco_imf_client_close(AncoImfClient *client);

/* Return true only when a Binder reply was decoded. The original service
 * status (0 means success) is returned separately in status.
 */
bool anco_imf_attach(AncoImfClient *client, bool show_keyboard,
                     int32_t input_pattern, int32_t enter_key_type,
                     int32_t input_option, int32_t *status);
bool anco_imf_attach_with_result(AncoImfClient *client, bool show_keyboard,
                     int32_t input_pattern, int32_t enter_key_type,
                     int32_t input_option, AncoImfCallResult *result);
/* Original transaction 9, single send, no fallback. NULL attribute writes
 * presence=0. Original nullable nested objects are not given extra markers.
 * An invalid UTF-8 string or non-representable count is a local failure. */
bool anco_imf_attach_attribute(AncoImfClient *client,
                     const AncoImfInputAttribute *attribute,
                     AncoImfCallResult *result);
typedef struct {
    AncoImfCallResult first, fallback;
    bool used_fallback;
} AncoImfAttachResult;
/* Original manager's dispatch branch only. support_new is manager-lifetime,
 * initialized true by its owner, not reset on every editor/client connection.
 * Attribute is the nonnull object already assembled by the manager. Caller
 * keeps it and support_new valid on its serialized execution path throughout.
 * This does not implement hasAttachFlag, editor admission, or show lifecycle.
 * Return describes final reply decoding, not original manager's void return. */
bool anco_imf_attach_select(AncoImfClient *client, bool *support_new,
                     bool requested_show, const AncoImfInputAttribute *attribute,
                     AncoImfAttachResult *result);
typedef AncoImfAttachResult AncoImfShowResult;
/* Original show dispatch branch after onWindowShown's attach/flag work.
 * support_new_show is independent of attach support and initially true.
 * motion_tool_type is the original guest semantic value, not evdev/Wayland.
 * Always sends, including repeated calls: no legacy attached/shown gating. */
bool anco_imf_show_select(AncoImfClient *client, bool *support_new_show,
                     int32_t motion_tool_type, AncoImfShowResult *result);
/* Original direct proxy calls: no local suppression, retries or implicit hide
 * before detach. Their call sites must supply original lifecycle ordering. */
bool anco_imf_hide_direct(AncoImfClient *client, AncoImfCallResult *result);
bool anco_imf_detach_direct(AncoImfClient *client, AncoImfCallResult *result);
bool anco_imf_show(AncoImfClient *client, int32_t *status);
bool anco_imf_hide(AncoImfClient *client, int32_t *status);
bool anco_imf_detach(AncoImfClient *client, int32_t *status);
bool anco_imf_dispatch_key(AncoImfClient *client,
                           const AncoImfKeyAttribute *attribute,
                           const AncoImfKeyItem *items, size_t item_count,
                           int32_t *status, bool *consumed);

/* Original manager transactions 6 and 7; temporary proxies do not gate
 * on local attached state. Selection text may be NULL as in Parcel. Coordinates must already be in
 * the native window's expected coordinate space; selection indices are
 * UTF-16 code units, not UTF-8 byte offsets. */
bool anco_imf_cursor_update(AncoImfClient *client, double position_x,
                            double position_y, double height,
                            int32_t *status);
bool anco_imf_selection_change(AncoImfClient *client, const char *utf8_text,
                               int32_t old_begin, int32_t old_end,
                               int32_t new_begin, int32_t new_end,
                               int32_t *status);

#ifdef __cplusplus
}
#endif
