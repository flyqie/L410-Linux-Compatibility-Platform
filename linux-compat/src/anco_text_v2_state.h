/* Per-actual-resource text-input-v2 attribute batching; Linux internal only.
 * No focus, enabled-surface identity, panel requests or host ABI here.
 * Receiver owns lifetime/admission, validates enums and supplies original
 * update_state serial. Do not use v3 commit counts as v2 serials. */
#ifndef ANCO_TEXT_V2_STATE_H
#define ANCO_TEXT_V2_STATE_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    bool surrounding_valid,rect_valid;
    char surrounding[4001];
    int32_t cursor,anchor,x,y,width,height;
    uint32_t hint,purpose;
    char *language; /* owned; NULL means unspecified, not an empty tag */
} AncoTextV2Fields;
typedef struct {
    AncoTextV2Fields pending,current;
    unsigned dirty;
    uint32_t serial,reason;
} AncoTextV2State;
void anco_text_v2_init(AncoTextV2State *);
/* Releases language ownership; clears fields, does not destroy wl_resource. */
void anco_text_v2_clear(AncoTextV2State *);
bool anco_text_v2_surrounding(AncoTextV2State *,const char *,int32_t cursor,int32_t anchor);
void anco_text_v2_content(AncoTextV2State *,uint32_t hint,uint32_t purpose);
bool anco_text_v2_rectangle(AncoTextV2State *,int32_t x,int32_t y,int32_t width,int32_t height);
int anco_text_v2_language(AncoTextV2State *,const char *language);
/* 0 success, negative errno on invalid reason/allocation failure; current,
 * pending and serial stay unchanged on failure. Reasons 0..3 are the original
 * change/full/reset/enter values. Not readiness or permission to attach IME. */
int anco_text_v2_update(AncoTextV2State *,uint32_t serial,uint32_t reason);
#endif
