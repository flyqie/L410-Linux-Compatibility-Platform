#include "anco_text_v2_state.h"
#include "anco_text_utf8.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
enum {SURROUND=1,CONTENT=2,RECT=4,LANGUAGE=8};
static char *copy_language(const char *s)
{size_t n=strlen(s)+1;char *p=malloc(n);if(p)memcpy(p,s,n);return p;}
void anco_text_v2_init(AncoTextV2State *s){*s=(AncoTextV2State){0};}
void anco_text_v2_clear(AncoTextV2State *s)
{free(s->pending.language);free(s->current.language);anco_text_v2_init(s);}
bool anco_text_v2_surrounding(AncoTextV2State *s,const char *text,int32_t c,int32_t a)
{
    if(!text)return false;
    size_t n=0;while(n<=4000 && text[n])++n;
    if(n>4000 || !anco_text_utf8_valid(text,n,c,a))return false;
    memcpy(s->pending.surrounding,text,n+1);s->pending.cursor=c;s->pending.anchor=a;
    s->pending.surrounding_valid=true;s->dirty|=SURROUND;return true;
}
void anco_text_v2_content(AncoTextV2State *s,uint32_t hint,uint32_t purpose)
{s->pending.hint=hint;s->pending.purpose=purpose;s->dirty|=CONTENT;}
bool anco_text_v2_rectangle(AncoTextV2State *s,int32_t x,int32_t y,int32_t w,int32_t h)
{
    if(w<0 || h<0)return false;
    s->pending.x=x;s->pending.y=y;s->pending.width=w;s->pending.height=h;
    s->pending.rect_valid=true;s->dirty|=RECT;return true;
}
int anco_text_v2_language(AncoTextV2State *s,const char *language)
{
    if(!language)return -EINVAL;
    char *copy=copy_language(language);if(!copy)return -ENOMEM;
    free(s->pending.language);s->pending.language=copy;s->dirty|=LANGUAGE;return 0;
}
int anco_text_v2_update(AncoTextV2State *s,uint32_t serial,uint32_t reason)
{
    if(reason>3)return -EINVAL;
    AncoTextV2Fields next=reason?(AncoTextV2Fields){0}:s->current;
    const char *language=(s->dirty&LANGUAGE)?s->pending.language:next.language;
    next.language=NULL;
    if(language){next.language=copy_language(language);if(!next.language)return -ENOMEM;}
    if(s->dirty&SURROUND) {
        memcpy(next.surrounding,s->pending.surrounding,sizeof(next.surrounding));
        next.surrounding_valid=s->pending.surrounding_valid;
        next.cursor=s->pending.cursor;next.anchor=s->pending.anchor;
    }
    if(s->dirty&CONTENT){next.hint=s->pending.hint;next.purpose=s->pending.purpose;}
    if(s->dirty&RECT) {
        next.x=s->pending.x;next.y=s->pending.y;next.width=s->pending.width;
        next.height=s->pending.height;next.rect_valid=s->pending.rect_valid;
    }
    free(s->current.language);s->current=next;s->serial=serial;s->reason=reason;
    free(s->pending.language);s->pending=(AncoTextV2Fields){0};s->dirty=0;
    return 0;
}
