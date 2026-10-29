#ifndef ANCO_IMF_EDITOR_CONSUMER_H
#define ANCO_IMF_EDITOR_CONSUMER_H
#include "anco_imf_callback_receiver.h"
#include "anco_imf_lifecycle.h"
#include "anco_text_receiver.h"
#include "anco_wayland_context.h"
typedef int (*AncoImfFunctionKey)(void *data, int32_t action);
/* Borrowed actual common-runtime objects, all on their serialized owner.
 * seat is the actual IME/current-input seat, not a new focus cache. The runtime
 * updates service from actual input-service ownership before invocation.
 * Every pointer must survive the synchronous callback. No SA publication here. */
typedef struct {
    AncoTextSeat *seat;
    AncoWaylandContext *context;
    AncoImeStatus *status;
    const AncoImeServiceView *service;
    /* Original transaction7 sendKeyChar('\n') adapter.  text-input-v2 has a
     * standard IME keysym event; v3 intentionally does not, so the common
     * runtime supplies the same current wl_keyboard connection used by the
     * original AnCo key callback.  This is not a host API or a second focus. */
    AncoImfFunctionKey function_key;
    void *function_key_data;
    /* Current synchronous query reply. At most 4000 UTF8 bytes were committed,
     * hence at most 4000 UTF16 units. Retain through receiver serialization. */
    gunichar2 query_reply[4001];
} AncoImfEditorConsumer;
/* Direct receiver consume function. Routes 1004/1005 to the actual current
 * AnCo service entrypoints if supplied, with no status reply or direct broker
 * fallback. This does not implement the service's window/height state machine.
 * Implements 1(cursor=1), original transaction7 sendKeyChar newline, 8, 9 and
 * the original single-key deletion branches of 2/3. An active v2/v3 Wayland selection supplies its exact UTF-8 byte
 * interval to those unchanged original deletion callbacks.
 * Queries4/5 support flags0 and nonnegative UTF16 lengths from actual committed
 * surrounding fragments. No absolute document index or style inference.
 * General non-selection surrounding deletion and v3 IME key output remain
 * unsupported.
 * Other operations explicitly fail until their original semantics are wired.
 * Commit success means accepted into actual Wayland queue, not UI receipt.
 * Strings unrepresentable in Wayland (NUL, lone surrogate) fail, never silently
 * change/truncate text. Original input method has no invented replacement API. */
void anco_imf_editor_consume(const AncoImfCallbackMessage *,
    const AncoCallbackHeader *, AncoImfCallbackResult *, void *);
#endif
