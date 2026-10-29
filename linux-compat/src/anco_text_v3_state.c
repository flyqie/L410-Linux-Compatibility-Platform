#include "anco_text_v3_state.h"
#include <string.h>
#include "anco_text_utf8.h"
bool anco_text_v3_init(AncoTextV3State *s,uint32_t version)
{
    if(!s || version<1 || version>2)return false;
    *s=(AncoTextV3State){.version=version};return true;
}
void anco_text_v3_invalidate(AncoTextV3State *s)
{uint32_t serial=s->serial,version=s->version;*s=(AncoTextV3State){.serial=serial,.version=version};}
static void reset_pending(AncoTextV3State *s,bool enabled)
{
    s->pending=(AncoTextV3Fields){0};s->pending_enabled=enabled;
    s->context_reset=true;s->rect_pending=false;
}
void anco_text_v3_enable(AncoTextV3State *s){reset_pending(s,true);}
void anco_text_v3_disable(AncoTextV3State *s){reset_pending(s,false);}
bool anco_text_v3_surrounding(AncoTextV3State *s,const char *text,int32_t cursor,int32_t anchor)
{
    if(!text)return false;
    size_t n=0;while(n<=4000 && text[n])++n;
    if(n>4000 || !anco_text_utf8_valid(text,n,cursor,anchor))return false;
    memcpy(s->pending.surrounding,text,n+1);s->pending.cursor=cursor;s->pending.anchor=anchor;
    s->pending.surrounding_valid=true;return true;
}
void anco_text_v3_content(AncoTextV3State *s,uint32_t hint,uint32_t purpose)
{s->pending.hint=hint;s->pending.purpose=purpose;}
void anco_text_v3_cause(AncoTextV3State *s,uint32_t cause){s->pending.cause=cause;}
bool anco_text_v3_rectangle(AncoTextV3State *s,int32_t x,int32_t y,int32_t w,int32_t h)
{
    if(w<0 || h<0)return false;
    s->pending.rect=(AncoTextRect){x,y,w,h,true};s->rect_pending=true;return true;
}
bool anco_text_v3_commit(AncoTextV3State *s,bool admitted)
{
    ++s->serial;if(!admitted)return false;
    bool reset=s->context_reset;
    AncoTextRect applied=s->current.rect;
    if(reset){applied=(AncoTextRect){0};s->committed_rect=(AncoTextRect){0};}
    s->enabled=s->pending_enabled;s->current=s->pending;
    if(s->version>=2) {
        s->current.rect=applied;
        if(s->rect_pending)s->committed_rect=s->pending.rect;
    }
    if(!s->enabled){s->current=(AncoTextV3Fields){0};s->committed_rect=(AncoTextRect){0};}
    s->pending.cause=0;s->context_reset=false;s->rect_pending=false;
    return reset;
}
bool anco_text_v3_surface_commit(AncoTextV3State *s)
{
    if(s->version<2 || !s->enabled || !s->committed_rect.valid)return false;
    s->current.rect=s->committed_rect;s->committed_rect=(AncoTextRect){0};return true;
}
