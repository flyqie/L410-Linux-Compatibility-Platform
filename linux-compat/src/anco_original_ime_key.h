/* Exact Linux transcription of the original AnCo guest's
 * VirtKeyEvent -> SA66827 transaction-5 key conversion. Linux-private only;
 * this does not define a host protocol or replace the original IMF service. */
#ifndef ANCO_ORIGINAL_IME_KEY_H
#define ANCO_ORIGINAL_IME_KEY_H
#include "anco_imf_client_v340.h"
#include "anco_native_input_relay.h"

typedef struct {
    AncoImfKeyItem held[64];
    size_t count;
    int32_t previous_action, previous_key;
} AncoOriginalImeKeyState;

void anco_original_ime_key_reset(AncoOriginalImeKeyState *);
/* Returns 0 on exact conversion. The output item pointer remains owned by the
 * state until the next conversion/reset. */
int anco_original_ime_key_convert(AncoOriginalImeKeyState *,
    const AncoNativeInputKey *, AncoImfKeyAttribute *,
    const AncoImfKeyItem **items, size_t *count);
#endif
