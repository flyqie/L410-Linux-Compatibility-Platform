#include "anco_imf_editor_consumer.h"
#include <errno.h>
#include <string.h>

static gsize utf16_prefix(const char *text,int32_t bytes)
{
    gsize units=0;
    for(const char *p=text;p<text+bytes;p=g_utf8_next_char(p))
        units+=g_utf8_get_char(p)>0xffff?2:1;
    return units;
}
static void query_text(AncoImfEditorConsumer *c,const AncoImfCallbackMessage *m,
                       AncoImfCallbackResult *result)
{
    if(m->data.query.flags || m->data.query.length<0 || !c->context ||
       !anco_wayland_context_active(c->context))return;
    const char *text;int32_t cursor,anchor;
    if(anco_text_seat_surrounding(c->seat,&text,&cursor,&anchor))return;
    glong total=0;
    gunichar2 *utf16=g_utf8_to_utf16(text,-1,NULL,&total,NULL);
    if(!utf16)return;
    gsize start=utf16_prefix(text,MIN(cursor,anchor));
    gsize end=utf16_prefix(text,MAX(cursor,anchor));
    gsize count=(gsize)m->data.query.length;
    if(m->code==4){count=MIN(count,start);start-=count;}
    else {start=end;count=MIN(count,(gsize)total-end);}
    /* Preserve original String substring units, including a split surrogate.
     * No UTF8 roundtrip that would replace a valid raw UTF16 reply fragment. */
    memcpy(c->query_reply,utf16+start,count*sizeof(*utf16));
    c->query_reply[count]=0;g_free(utf16);
    result->text=c->query_reply;result->units=count;result->status=0;
}

static char *insertion_utf8(const guint8 *s,gsize units)
{
    if(!s || units>4000)return NULL;
    /* Read little-endian bytes explicitly: input may be unaligned. At most
     * 4000 UTF16 units can fit the existing 4000-byte Wayland commit limit. */
    gunichar2 text[4001];
    for(gsize n=0;n<units;++n){
        text[n]=(gunichar2)(s[2*n]|((guint16)s[2*n+1]<<8));
        if(!text[n])return NULL;
    }
    text[units]=0;
    /* NULL items_read requires complete valid surrogate pairs. */
    glong bytes=0;
    char *utf8=g_utf16_to_utf8(text,(glong)units,NULL,&bytes,NULL);
    if(bytes>4000){g_free(utf8);return NULL;}
    return utf8;
}

static gboolean selected_delete(AncoTextSeat *seat,guint32 *before,guint32 *after)
{
    const char *text;int32_t cursor,anchor;
    if(!seat || !before || !after ||
       anco_text_seat_surrounding(seat,&text,&cursor,&anchor) ||
       cursor==anchor)return FALSE;
    const gsize bytes=strlen(text);
    if(cursor<0 || anchor<0 || (gsize)cursor>bytes || (gsize)anchor>bytes ||
       !g_utf8_validate(text,-1,NULL) ||
       (cursor<(int32_t)bytes && (((guchar)text[cursor]&0xc0)==0x80)) ||
       (anchor<(int32_t)bytes && (((guchar)text[anchor]&0xc0)==0x80)))return FALSE;
    *before=cursor>anchor?(guint32)(cursor-anchor):0;
    *after=anchor>cursor?(guint32)(anchor-cursor):0;
    return TRUE;
}

void anco_imf_editor_consume(const AncoImfCallbackMessage *m,
    const AncoCallbackHeader *header,AncoImfCallbackResult *result,void *data)
{
    (void)header; /* Metadata, never caller authority or editor identity. */
    if(!result)return;
    *result=(AncoImfCallbackResult){.status=-1};
    AncoImfEditorConsumer *c=data;
    if(!c || !m)return;
    switch(m->code){
    case 1004: case 1005: {
        /* AncoImeProxy's Optional current service and instanceof gate.
         * No invented status reply, visibility test, attach or direct host
         * show/hide. These service callbacks have their own state ordering. */
        const AncoImeServiceView *service=c->service;
        if(service && service->is_anco_service){
            void (*notify)(void *)=m->code==1004 ?
                service->on_window_shown : service->hide_ime_window;
            if(notify)notify(service->data);
        }
        break;
    }
    case 4: case 5: query_text(c,m,result);break;
    case 1: {
        if(m->data.insert.cursor!=1 || !c->seat || !c->context ||
           !anco_wayland_context_active(c->context))return;
        char *text=insertion_utf8(m->data.insert.utf16,m->data.insert.units);
        if(!text)return;
        int queued=anco_text_seat_commit_end(c->seat,text);
        g_free(text);
        if(!queued && !anco_wayland_context_wake(c->context))result->status=0;
        break;
    }
    case 2: case 3: {
        const gint32 before=m->data.deletion.before,after=m->data.deletion.after;
        /* Original Optional service branch returns true even when absent.
         * This is NOT a delivered-event or GUI acknowledgement. */
        if(!c->service){result->status=0;return;}
        if(!c->context || !anco_wayland_context_active(c->context))return;
        if(before<0 || after<0)return;
        guint32 delete_before=(guint32)before,delete_after=(guint32)after;
        /* Wayland publishes the active selection as UTF-8 byte offsets in the
         * committed surrounding text.  Original IMF DeleteBackward/Forward
         * only supplies the key deletion length, so replace it with the exact
         * selected interval when the client has an active selection. */
        selected_delete(c->seat,&delete_before,&delete_after);
        if(!anco_text_seat_delete_surrounding(c->seat,delete_before,
                delete_after) &&
           !anco_wayland_context_wake(c->context))result->status=0;
        break;
    }
    case 7: {
        /* Original AncoImeProxy ignores actionCode and calls
         * InputMethodService.sendKeyChar('\n').  For text-input-v2 the
         * protocol's standard keysym pair is the direct current-connection
         * equivalent.  text-input-v3 has no keysym event, so use the common
         * runtime's already-focused wl_keyboard path rather than inventing a
         * protocol request or committing a newline as text. */
        if(!c->seat || !c->context || !anco_wayland_context_active(c->context))return;
        const guint32 down=(guint32)(g_get_monotonic_time()/1000);
        int queued=anco_text_seat_ime_keysym(c->seat,0xff0dU,down,down);
        if(queued==-ENOTSUP && c->function_key)
            queued=c->function_key(c->function_key_data,m->data.action);
        if(!queued && !anco_wayland_context_wake(c->context))result->status=0;
        break;
    }
    case 8: result->status=anco_ime_receive_status(c->status,c->service,m->data.status);break;
    case 9: result->status=anco_ime_receive_height(c->status,c->service,m->data.height);break;
    default: break;
    }
}
