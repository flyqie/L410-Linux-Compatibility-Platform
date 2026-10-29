#include "anco_imf_callback_codec.h"
#include <errno.h>
int anco_imf_callback_read(GBinderReader *r, guint code, AncoImfCallbackMessage *out)
{
    if (!r || !out) return -EINVAL;
    if (code < 1 || code > 14) return -ENOTSUP;
    AncoImfCallbackMessage m = {.code=code};
#define I(field) m.data.field = anco_callback_read_int32(r)
    switch (code) {
    case 1:
        m.data.insert.utf16=anco_callback_read_string16(r,&m.data.insert.units);
        I(insert.cursor); break;
    case 2: case 3: I(deletion.before); I(deletion.after); break;
    case 4: case 5: I(query.length); I(query.flags); break;
    case 6: break;
    case 7: case 13: I(action); break;
    case 8: I(status); break;
    case 9: I(height); break;
    case 10: I(direction); break;
    case 11: I(selection.start); I(selection.end); break;
    case 12: I(select.key); I(select.skip); break;
    case 14: {
        gint32 count=anco_callback_read_int32(r);
        /* Original for(i=0;i<count;i++) projects negative counts to empty. */
        if(count<=0)break;
        m.data.commands.items=g_try_malloc0_n((gsize)count,sizeof(AncoImfReceivedPrivateCommand));
        if(!m.data.commands.items)return -ENOMEM;
        m.data.commands.count=(gsize)count;
        for(gsize n=0;n<m.data.commands.count;++n){
            AncoImfReceivedPrivateCommand *item=&m.data.commands.items[n];
            item->present=anco_callback_read_int32(r)!=0;
            if(!item->present)continue;
            item->key=anco_callback_read_string16(r,&item->key_units);
            item->type=anco_callback_read_int32(r);
            item->text=anco_callback_read_string16(r,&item->text_units);
            item->boolean=anco_callback_read_int32(r)!=0;
            item->integer=anco_callback_read_int32(r);
        }
        break;
    }
    }
#undef I
    *out=m;
    return 0;
}
void anco_imf_callback_message_clear(AncoImfCallbackMessage *m)
{
    if(!m)return;
    if(m->code==14)g_free(m->data.commands.items);
    *m=(AncoImfCallbackMessage){0};
}
