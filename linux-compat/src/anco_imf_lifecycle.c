#include "anco_imf_lifecycle.h"
#include <string.h>
void anco_ime_status_init(AncoImeStatus *s)
{
    if (!s) return;
    atomic_init(&s->show,false); atomic_init(&s->hide,false);
    atomic_init(&s->window_hide,false); atomic_init(&s->has_attach,false);
}
static bool lifecycle_owner(AncoImfLifecycle *s)
{ return s && s->broker && s->status && s->owner==g_thread_self(); }
bool anco_imf_lifecycle_init(AncoImfLifecycle *s,AncoImfBroker *b,AncoImeStatus *flags)
{
    if (!s || !b || !flags) return false;
    *s=(AncoImfLifecycle){.broker=b,.status=flags,.support_new_attach=true,
                         .support_new_show=true,.owner=g_thread_self()};
    return true;
}
bool anco_imf_lifecycle_update_editor(AncoImfLifecycle *s,const AncoImeEditor *e)
{
    if (!lifecycle_owner(s)) return false;
    s->has_editor=e!=NULL;
    if (e) s->last_editor=*e;
    return true;
}
static void lifecycle_attach(AncoImfLifecycle *s,const AncoImeEditor *e,
    const AncoImfPrivateCommand *const *commands,size_t count,AncoImfAttachResult *r)
{
    AncoImfClient *c=anco_imf_broker_client(s->broker);
    if (!c) return;
    AncoImeEditorFields fields={0};
    anco_ime_editor_update_fields(&fields,false,e);
    AncoImfInputAttribute a={.show_keyboard=fields.show_keyboard,
        .pattern=fields.pattern,.enter_key_type=fields.enter_key_type,
        .option=fields.option,.commands=e?commands:NULL,.command_count=e?count:0};
    atomic_store(&s->status->has_attach,true);
    anco_imf_attach_select(c,&s->support_new_attach,false,&a,r);
    anco_imf_client_close(c);
}
static void lifecycle_result(AncoImfLifecycleResult *r)
{
    memset(r,0,sizeof(*r));
    for(unsigned i=0;i<2;++i) {
        r->attach[i].first.status=-1; r->attach[i].fallback.status=-1;
    }
    r->show.first.status=r->show.fallback.status=r->direct.status=-1;
}
bool anco_imf_lifecycle_start(AncoImfLifecycle *s,const AncoImeEditor *e,
    const AncoImfPrivateCommand *const *commands,size_t count,AncoImfLifecycleResult *out)
{
    AncoImfLifecycleResult local; if(!out)out=&local; lifecycle_result(out);
    if(!anco_imf_lifecycle_update_editor(s,e))return false;
    out->attach_count=1;
    lifecycle_attach(s,e,commands,count,&out->attach[0]);
    return true;
}
bool anco_imf_lifecycle_shown(AncoImfLifecycle *s,
    AncoImeCollaborationQuery collaboration,void *data,
    int32_t tool,AncoImfLifecycleResult *out)
{
    AncoImfLifecycleResult local; if(!out)out=&local; lifecycle_result(out);
    if(!lifecycle_owner(s) || !collaboration)return false;
    AncoImfClient *c=anco_imf_broker_client(s->broker);
    if(!c)return true;
    atomic_store(&s->status->show,true);
    if(collaboration(data)) {
        lifecycle_attach(s,s->has_editor?&s->last_editor:NULL,NULL,0,
                         &out->attach[out->attach_count++]);
    }
    if(!atomic_load(&s->status->has_attach)) {
        lifecycle_attach(s,s->has_editor?&s->last_editor:NULL,NULL,0,
                         &out->attach[out->attach_count++]);
    }
    anco_imf_show_select(c,&s->support_new_show,tool,&out->show);
    anco_imf_client_close(c);
    return true;
}
bool anco_imf_lifecycle_hidden(AncoImfLifecycle *s,AncoImfLifecycleResult *out)
{
    AncoImfLifecycleResult local; if(!out)out=&local; lifecycle_result(out);
    if(!lifecycle_owner(s))return false;
    AncoImfClient *c=anco_imf_broker_client(s->broker);
    if(!c)return true;
    atomic_store(&s->status->hide,true);
    anco_imf_hide_direct(c,&out->direct);
    anco_imf_client_close(c);
    return true;
}
bool anco_imf_lifecycle_unbind(AncoImfLifecycle *s,bool keyboard_shown,
    AncoImfLifecycleResult *out)
{
    AncoImfLifecycleResult local; if(!out)out=&local; lifecycle_result(out);
    if(!lifecycle_owner(s))return false;
    AncoImfClient *c=anco_imf_broker_client(s->broker);
    if(!c)return true;
    if(keyboard_shown)atomic_store(&s->status->hide,true);
    anco_imf_detach_direct(c,&out->direct);
    anco_imf_client_close(c);
    return true;
}

static bool service_window_hidden(AncoImfLifecycle *s,AncoImeWindowState *window,
    bool from_window_request,AncoImfLifecycleResult *out)
{
    AncoImfLifecycleResult local;if(!out)out=&local;lifecycle_result(out);
    if(!lifecycle_owner(s) || !window)return false;
    window->notification_shown=false;
    if(from_window_request && window->height!=0)
        atomic_store(&s->status->window_hide,true);
    /* Manager may synchronously receive callbacks. Never restore service
     * fields/flags after it returns, even when host transport fails. */
    return anco_imf_lifecycle_hidden(s,out);
}
bool anco_imf_service_window_hidden(AncoImfLifecycle *s,AncoImeWindowState *window,
    AncoImfLifecycleResult *out)
{ return service_window_hidden(s,window,false,out); }
bool anco_imf_service_hide_window(AncoImfLifecycle *s,AncoImeWindowState *window,
    AncoImfLifecycleResult *out)
{ return service_window_hidden(s,window,true,out); }

bool anco_imf_service_notify_height(AncoImfLifecycle *s,AncoImeWindowState *window,
    int32_t height,bool by_self,AncoImeHeightUpdate update,void *data)
{
    if(!lifecycle_owner(s) || !window || !update)return false;
    if(window->height==height)return true;
    if(window->last_display_mode_height==height && height>0){
        window->last_display_mode_height=window->height;
        return true;
    }
    window->last_display_mode_height=height;
    update(data,height,by_self);
    return true;
}

static bool ime_service_valid(const AncoImeServiceView *v)
{
    return v && v->is_shown && v->hardware_keyboard && v->collaboration &&
        v->request_show && v->request_hide && v->notify_height;
}
int32_t anco_ime_receive_status(AncoImeStatus *s,const AncoImeServiceView *v,
                               int32_t status)
{
    if(!s || !ime_service_valid(v))return -1;
    const bool shown=v->is_shown(v->data);
    switch(status) {
    case 0:
        if(shown)v->request_hide(v->data,0);
        atomic_store(&s->has_attach,false);
        return 0;
    case 1: {
        const bool requested=atomic_load(&s->hide);
        atomic_store(&s->hide,false);
        /* Preserve original hardware-first short circuit and special handling
         * even when already hidden or the outgoing hide flag was set. */
        if(v->hardware_keyboard(v->data) || v->collaboration(v->data)) {
            if(v->is_anco_service)v->notify_height(v->data,0,true);
        } else if(shown && !requested) {
            v->request_hide(v->data,0);
        }
        return 0;
    }
    case 2: {
        const bool requested=atomic_load(&s->show);
        atomic_store(&s->show,false);
        if(!shown && !requested)v->request_show(v->data,0);
        return 0;
    }
    default:return -1;
    }
}
int32_t anco_ime_receive_height(AncoImeStatus *s,const AncoImeServiceView *v,
                               int32_t height)
{
    if(!s || (v && !ime_service_valid(v)))return -1;
    if(!atomic_load(&s->window_hide) && v && v->is_anco_service)
        v->notify_height(v->data,height,false);
    /* Original clears after consumer, even if no current service. */
    atomic_store(&s->window_hide,false);
    return 0;
}
