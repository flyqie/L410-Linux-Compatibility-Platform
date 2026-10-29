/* Standard text-input-v3 state storage for the Linux semantic bridge.
 * One instance per ACTUAL text-input resource, owned by the protocol receiver.
 * No host interface, editor identity, focus/seat admission or event emission.
 * Receiver must admit requests for the actual focused resource, enforce seat
 * exclusivity, and invalidate on leave/enter/destroy before invoking consumers.
 * Use version negotiated on that resource (1 or 2), not global maximum. */
#ifndef ANCO_TEXT_V3_STATE_H
#define ANCO_TEXT_V3_STATE_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {int32_t x,y,width,height;bool valid;} AncoTextRect;
typedef struct {
    bool surrounding_valid;
    char surrounding[4001];
    int32_t cursor,anchor;
    uint32_t hint,purpose,cause;
    AncoTextRect rect;
} AncoTextV3Fields;
typedef struct {
    AncoTextV3Fields pending,current;
    AncoTextRect committed_rect;
    bool pending_enabled,enabled,context_reset,rect_pending;
    uint32_t serial,version;
} AncoTextV3State;
bool anco_text_v3_init(AncoTextV3State *,uint32_t version);
/* Focus invalidation preserves commit serial and negotiated version. */
void anco_text_v3_invalidate(AncoTextV3State *);
void anco_text_v3_enable(AncoTextV3State *);
void anco_text_v3_disable(AncoTextV3State *);
bool anco_text_v3_surrounding(AncoTextV3State *,const char *,int32_t cursor,int32_t anchor);
void anco_text_v3_content(AncoTextV3State *,uint32_t hint,uint32_t purpose);
void anco_text_v3_cause(AncoTextV3State *,uint32_t cause);
bool anco_text_v3_rectangle(AncoTextV3State *,int32_t x,int32_t y,int32_t width,int32_t height);
/* Counts every request, even if not admitted by receiver. Returns whether a
 * new enable/disable boundary applied, NOT whether editor is enabled/ready. */
bool anco_text_v3_commit(AncoTextV3State *,bool admitted);
/* Version 2 only: called for actual focused surface commit, after admission.
 * Return true means cursor state applied, not host IME acceptance. */
bool anco_text_v3_surface_commit(AncoTextV3State *);
#endif
