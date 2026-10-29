/* Linux Wayland protocol receiver, not a host ABI. Serialized owner context.
 * Supported globals are text-input-v2 and text-input-v3, version 1 each. */
#ifndef ANCO_TEXT_RECEIVER_H
#define ANCO_TEXT_RECEIVER_H
#include "anco_wayland_ownership.h"
#include "anco_text_v3_state.h"
#include "anco_text_v2_state.h"
typedef struct AncoTextReceiver AncoTextReceiver;
typedef struct AncoTextSeat AncoTextSeat;
/* Borrowed protocol-specific committed state. Protocol is 2 or 3, independent
 * of the negotiated interface version. Never cast purpose/serial/reason between
 * these protocols or into original AnCo EditorInfo fields. */
typedef struct {
    unsigned protocol;
    union {
        const AncoTextV2State *v2;
        const AncoTextV3State *v3;
    } state;
} AncoTextEditor;
/* Callback is synchronous, borrowed state only. No resource destruction,
 * receiver mutation, recursive dispatch or deferred pointer dereference here.
 * NULL state/window means the current editor was invalidated. A nonnull state
 * is committed and enabled; boundary distinguishes a repeated enable from an
 * ordinary attribute update. This is NOT host attach/show acceptance. */
typedef void (*AncoTextChanged)(void *data, AncoTextSeat *seat,
    struct wl_resource *input, const AncoTextEditor *state,
    const AncoLinuxWindowOwner *window, bool boundary);
typedef void (*AncoTextPanelRequest)(void *data, AncoTextSeat *seat,
    struct wl_resource *input, const AncoLinuxWindowOwner *window, bool show);
/* Same borrowed/nonreentrant contract as changed. Actual original service
 * admission/visibility belongs to consumer; a request is not shown evidence. */
int anco_text_receiver_panel_handler(AncoTextReceiver *, AncoTextPanelRequest);
AncoTextReceiver *anco_text_receiver_new(struct wl_display *,
    AncoWaylandOwnership *, AncoTextChanged, void *data);
/* Destroy globals/resources before display/ownership. Does not destroy seats'
 * wl_seat resources, client processes, surfaces or windows. */
void anco_text_receiver_free(AncoTextReceiver *);
/* One object per actual compositor logical seat. Runtime registers each real
 * wl_seat binding from its seat bind callback. Client release removes only
 * that binding; text-input children retain their logical seat association. */
AncoTextSeat *anco_text_receiver_seat(AncoTextReceiver *);
int anco_text_seat_bind(AncoTextSeat *, struct wl_resource *seat_resource);
/* Called from the actual keyboard focus path. NULL clears focus. Nonnull
 * surface must have an admitted actual Window; errors preserve old focus. */
int anco_text_seat_focus(AncoTextSeat *, struct wl_resource *surface);
/* Standard commit to the actual current connection, placing cursor at the
 * inserted text end (original InputConnection newCursorPosition == 1 ONLY).
 * Caller must not use this for another cursor position. No preedit creation or
 * optimistic surrounding-text update. UTF-8 <=4000 bytes, empty is permitted.
 * Returns 0 when events are queued, negative errno otherwise; not an app/UI
 * acknowledgement. Same serialized context, including a synchronous reverse
 * callback during changed; queues events without reentering Wayland dispatch.
 * Runtime must wake its AncoWaylandContext after non-Wayland callbacks. */
int anco_text_seat_commit_end(AncoTextSeat *, const char *text);
/* Dedicated standard v2 IME keysym pair, zero modifiers, to actual current
 * enabled resource. Does not touch physical held keys/XKB/focus. Caller supplies
 * actual monotonic event times and XKB keysym after original key translation.
 * v3 returns -ENOTSUP: no invented wire extension or physical-key fallback.
 * Queue acceptance only; client may discard while its reset callback is pending.
 * Runtime must retain original admission/order and wake its Wayland context. */
int anco_text_seat_ime_keysym(AncoTextSeat *,uint32_t keysym,
                            uint32_t down_time,uint32_t up_time);
/* Deliver the original IMF deletion through the active standard Wayland
 * text-input resource.  This never synthesizes a physical keyboard event. */
int anco_text_seat_delete_surrounding(AncoTextSeat *,uint32_t before,
                                      uint32_t after);
/* Current committed selection only, not pending setters or a second cache.
 * Exact live Window/resource revalidation; ENODATA means no surrounding state,
 * ENOENT no current admitted editor. Output untouched on failure. */
int anco_text_seat_has_selection(AncoTextSeat *,bool *selected);
/* Borrowed committed surrounding fragment and UTF8 byte offsets. Same exact
 * current-resource admission as selection; never exposes pending setters.
 * Valid only through the synchronous call, no dispatch/mutation meanwhile.
 * Fragment offsets are NOT absolute document positions. Outputs unchanged
 * on failure. ENODATA is unknown surrounding text, not an empty document. */
int anco_text_seat_surrounding(AncoTextSeat *,const char **text,
                             int32_t *cursor,int32_t *anchor);
/* Call from existing Window-retirement hook before role removal. Does not
 * dereference the retiring role; sends leave and invalidates matching focus. */
void anco_text_receiver_retire(AncoTextReceiver *, struct wl_resource *surface);
#endif
