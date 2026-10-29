/* Linux-only native/musl-to-glibc process boundary. NOT a host AnCo API.
 * One inherited SOCK_SEQPACKET pair, owned by the actual session launcher.
 * Every packet is an actual original callback state or Open() result; no
 * host-window -> Linux-window ID mapping, synthetic input or ready watchdog.
 */
#ifndef ANCO_NATIVE_INPUT_RELAY_H
#define ANCO_NATIVE_INPUT_RELAY_H
#include <stdint.h>
#define ANCO_INPUT_RELAY_SESSION UINT32_MAX
typedef struct {
    uint32_t code, bytes;
    int32_t status, host_focus;
    uint32_t container, key_excluded, retained_windows, knuckle;
} AncoNativeInputRelay;
/* Raw original VirtKeyEvent values; fixed-width representation only for the
 * Linux/musl process boundary. Conversion/target selection belongs to the
 * actual Wayland owner, not this header or a host Window ID. */
typedef struct {
    AncoNativeInputRelay state;
    int64_t action_time_us, action_start_us;
    int32_t event_id, device_id, android_key, android_action;
    uint32_t android_meta, reserved;
} AncoNativeInputKey;
/* Complete original Pointer fields across the same Linux-only boundary.
 * No host ID -> guest ID guess, coordinate conversion or input receipt. */
typedef struct {
    int64_t field_00;
    int32_t field_08,field_0c,field_10,field_14,field_18,field_1c;
} AncoNativeInputPointerItem;
typedef struct {
    AncoNativeInputRelay state;
    int64_t field_00,field_08;
    int32_t fields_10_to_28[7];
    uint32_t field_2c;
    float field_30,field_34;
    int32_t fields_38_to_48[5];
    uint32_t item_count;
    AncoNativeInputPointerItem items[32];
} AncoNativeInputPointer;
/* Full retained original host-window snapshot. Same private process boundary;
 * records are followed by transform floats, hot rects and excluded rects in
 * that order. Scalar bit patterns (including float values) are preserved. */
typedef struct {
    AncoNativeInputRelay state;
    uint32_t record_count, reserved;
} AncoNativeInputWindows;
typedef struct {
    int32_t display_id, window_id;
    uint32_t action, flags;
    float z_order;
    uint32_t transform_count, hot_count, excluded_count;
} AncoNativeHostWindowFields;
typedef struct { int32_t values[4]; } AncoNativeHostRect;
#endif
