#include "anco_text_receiver.h"
#include "text-input-unstable-v3-server.h"
#include "text-input-unstable-v2-server.h"
#include "anco_text_utf8.h"
#include <wayland-server-protocol.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct Input Input;
typedef struct Binding Binding;
typedef struct Manager Manager;
struct AncoTextReceiver {
    struct wl_global *global, *global_v2;
    struct wl_display *display;
    AncoWaylandOwnership *ownership;
    struct wl_list seats, managers;
    AncoTextChanged changed;
    AncoTextPanelRequest panel;
    void *data;
    bool notifying;
};
struct AncoTextSeat {
    AncoTextReceiver *receiver;
    struct wl_list link, bindings, inputs;
    struct wl_resource *focus;
    struct wl_listener focus_destroy;
    Input *active;
};
struct Binding {AncoTextSeat *seat; struct wl_resource *resource;
    struct wl_listener destroy; struct wl_list link;};
struct Manager {AncoTextReceiver *receiver;struct wl_resource *resource;struct wl_list link;unsigned protocol;};
struct Input {AncoTextSeat *seat;struct wl_resource *resource;
    AncoTextV3State state;AncoTextV2State state_v2;
    unsigned protocol;bool v2_ready;
    struct wl_resource *enabled_surface;struct wl_listener enabled_destroy;
    struct wl_list link;};

static void notify(Input *i, const AncoLinuxWindowOwner *window, bool boundary)
{
    AncoTextReceiver *r=i->seat->receiver;
    if (!r->changed) return;
    AncoTextEditor editor={.protocol=i->protocol};
    if(i->protocol==2)editor.state.v2=&i->state_v2;else editor.state.v3=&i->state;
    r->notifying=true;
    r->changed(r->data,i->seat,i->resource,window?&editor:NULL,window,boundary);
    r->notifying=false;
}
static bool matches(Input *i)
{
    return i->seat->focus && wl_resource_get_client(i->resource)==
        wl_resource_get_client(i->seat->focus);
}
static bool admitted(Input *i, AncoLinuxWindowOwner *window)
{
    return matches(i) && !anco_wayland_ownership_surface_window(
        i->seat->receiver->ownership,i->seat->focus,window);
}
static void send_enter(Input *i)
{
    if(i->protocol==2)zwp_text_input_v2_send_enter(i->resource,
        wl_display_next_serial(i->seat->receiver->display),i->seat->focus);
    else zwp_text_input_v3_send_enter(i->resource,i->seat->focus);
}
static void forget_enabled(Input *i)
{
    if(i->enabled_surface){wl_list_remove(&i->enabled_destroy.link);i->enabled_surface=NULL;}
}
static void enabled_destroyed(struct wl_listener *l,void *data)
{
    (void)data;Input *i=wl_container_of(l,i,enabled_destroy);
    if(i->seat->active==i){notify(i,NULL,true);i->seat->active=NULL;}
    forget_enabled(i);anco_text_v2_clear(&i->state_v2);i->v2_ready=false;
}
static void clear_focus(AncoTextSeat *s)
{
    if (!s->focus) return;
    Input *i;
    if (s->active) {notify(s->active,NULL,true);s->active=NULL;}
    wl_list_for_each(i,&s->inputs,link) {
        if(matches(i)) {
            if(i->protocol==2)zwp_text_input_v2_send_leave(i->resource,
                wl_display_next_serial(s->receiver->display),s->focus);
            else zwp_text_input_v3_send_leave(i->resource,s->focus);
        }
        if(i->protocol==2){anco_text_v2_clear(&i->state_v2);i->v2_ready=false;}
        else anco_text_v3_invalidate(&i->state);
    }
    wl_list_remove(&s->focus_destroy.link);
    s->focus=NULL;
}
static void focus_destroyed(struct wl_listener *l, void *data)
{(void)data;AncoTextSeat *s=wl_container_of(l,s,focus_destroy);clear_focus(s);}
static void binding_destroyed(struct wl_listener *l, void *data)
{
    (void)data;Binding *b=wl_container_of(l,b,destroy);
    wl_list_remove(&b->destroy.link);wl_list_remove(&b->link);free(b);
}
static void input_destroyed(struct wl_resource *resource)
{
    Input *i=wl_resource_get_user_data(resource);
    if(i->seat->active==i){notify(i,NULL,true);i->seat->active=NULL;}
    forget_enabled(i);anco_text_v2_clear(&i->state_v2);
    wl_list_remove(&i->link);free(i);
}
static void destroy_request(struct wl_client *client,struct wl_resource *resource)
{(void)client;wl_resource_destroy(resource);}
static bool request_owner(struct wl_resource *resource)
{
    Input *i=wl_resource_get_user_data(resource);AncoLinuxWindowOwner window;
    return admitted(i,&window);
}
static void enable(struct wl_client *client,struct wl_resource *resource)
{
    (void)client;Input *i=wl_resource_get_user_data(resource);
    if(request_owner(resource) && (!i->seat->active || i->seat->active==i))
        anco_text_v3_enable(&i->state);
}
static void disable(struct wl_client *client,struct wl_resource *resource)
{
    (void)client;Input *i=wl_resource_get_user_data(resource);
    if(request_owner(resource))anco_text_v3_disable(&i->state);
}
static void surrounding(struct wl_client *client,struct wl_resource *resource,
    const char *text,int32_t cursor,int32_t anchor)
{
    (void)client;Input *i=wl_resource_get_user_data(resource);
    if(request_owner(resource) && !anco_text_v3_surrounding(&i->state,text,cursor,anchor))
        wl_resource_post_error(resource,WL_DISPLAY_ERROR_IMPLEMENTATION,"invalid surrounding UTF-8/range");
}
static void cause(struct wl_client *client,struct wl_resource *resource,uint32_t value)
{
    (void)client;Input *i=wl_resource_get_user_data(resource);
    if(!request_owner(resource))return;
    if(value>1){wl_resource_post_error(resource,WL_DISPLAY_ERROR_IMPLEMENTATION,"invalid change cause");return;}
    anco_text_v3_cause(&i->state,value);
}
static void content(struct wl_client *client,struct wl_resource *resource,uint32_t hint,uint32_t purpose)
{
    (void)client;Input *i=wl_resource_get_user_data(resource);
    if(!request_owner(resource))return;
    if((hint&~0x3ffu)||purpose>13){wl_resource_post_error(resource,WL_DISPLAY_ERROR_IMPLEMENTATION,"invalid content type");return;}
    anco_text_v3_content(&i->state,hint,purpose);
}
static void rectangle(struct wl_client *client,struct wl_resource *resource,
    int32_t x,int32_t y,int32_t width,int32_t height)
{
    (void)client;Input *i=wl_resource_get_user_data(resource);
    if(request_owner(resource) && !anco_text_v3_rectangle(&i->state,x,y,width,height))
        wl_resource_post_error(resource,WL_DISPLAY_ERROR_IMPLEMENTATION,"invalid cursor rectangle");
}
static void commit(struct wl_client *client,struct wl_resource *resource)
{
    (void)client;Input *i=wl_resource_get_user_data(resource);
    AncoTextSeat *s=i->seat;AncoLinuxWindowOwner window;
    bool ok=admitted(i,&window);
    /* Two pending enables may precede either commit. Never replace another
     * resource's committed current connection at the second commit. */
    if(ok && i->state.pending_enabled && s->active && s->active!=i) {
        anco_text_v3_invalidate(&i->state);ok=false;
    }
    bool boundary=anco_text_v3_commit(&i->state,ok);
    if(!ok) {
        if(s->active==i){notify(i,NULL,true);s->active=NULL;}
        return;
    }
    if(i->state.enabled){s->active=i;notify(i,&window,boundary);}
    else if(s->active==i){notify(i,NULL,true);s->active=NULL;}
}
static const struct zwp_text_input_v3_interface input_impl={
    .destroy=destroy_request,.enable=enable,.disable=disable,.set_surrounding_text=surrounding,
    .set_text_change_cause=cause,.set_content_type=content,.set_cursor_rectangle=rectangle,.commit=commit
};
static AncoTextSeat *find_seat(AncoTextReceiver *r,struct wl_resource *resource)
{
    AncoTextSeat *s;Binding *b;
    wl_list_for_each(s,&r->seats,link)
        wl_list_for_each(b,&s->bindings,link)if(b->resource==resource)return s;
    return NULL;
}
#include "anco_text_receiver_v2.inc"
static void get_input(struct wl_client *client,struct wl_resource *resource,
    uint32_t id,struct wl_resource *seat)
{
    Manager *m=wl_resource_get_user_data(resource);
    AncoTextSeat *s=find_seat(m->receiver,seat);
    if(!s || wl_resource_get_client(seat)!=client){wl_resource_post_error(resource,
        WL_DISPLAY_ERROR_IMPLEMENTATION,"unregistered logical seat");return;}
    Input *i=calloc(1,sizeof(*i));
    if(!i){wl_client_post_no_memory(client);return;}
    i->protocol=m->protocol;
    i->resource=wl_resource_create(client,i->protocol==2?&zwp_text_input_v2_interface:&zwp_text_input_v3_interface,1,id);
    if(!i->resource){free(i);wl_client_post_no_memory(client);return;}
    i->seat=s;anco_text_v3_init(&i->state,1);
    wl_list_insert(s->inputs.prev,&i->link);
    wl_resource_set_implementation(i->resource,i->protocol==2?(const void *)&v2_impl:(const void *)&input_impl,i,input_destroyed);
    if(matches(i))send_enter(i);
}
static void manager_destroyed(struct wl_resource *resource)
{Manager *m=wl_resource_get_user_data(resource);wl_list_remove(&m->link);free(m);}
static const struct zwp_text_input_manager_v3_interface manager_impl={
    .destroy=destroy_request,.get_text_input=get_input};
static const struct zwp_text_input_manager_v2_interface manager_v2_impl={
    .destroy=destroy_request,.get_text_input=get_input};
static void bind_manager_protocol(struct wl_client *client,void *data,uint32_t id,unsigned protocol)
{
    AncoTextReceiver *r=data;Manager *m=calloc(1,sizeof(*m));
    if(!m){wl_client_post_no_memory(client);return;}
    m->resource=wl_resource_create(client,protocol==2?&zwp_text_input_manager_v2_interface:&zwp_text_input_manager_v3_interface,1,id);
    if(!m->resource){free(m);wl_client_post_no_memory(client);return;}
    m->receiver=r;m->protocol=protocol;wl_list_insert(r->managers.prev,&m->link);
    wl_resource_set_implementation(m->resource,protocol==2?(const void *)&manager_v2_impl:(const void *)&manager_impl,m,manager_destroyed);
}
static void bind_manager(struct wl_client *c,void *d,uint32_t v,uint32_t id)
{(void)v;bind_manager_protocol(c,d,id,3);}
static void bind_manager_v2(struct wl_client *c,void *d,uint32_t v,uint32_t id)
{(void)v;bind_manager_protocol(c,d,id,2);}
AncoTextReceiver *anco_text_receiver_new(struct wl_display *display,
    AncoWaylandOwnership *ownership,AncoTextChanged changed,void *data)
{
    if(!display || !ownership){errno=EINVAL;return NULL;}
    AncoTextReceiver *r=calloc(1,sizeof(*r));if(!r)return NULL;
    r->ownership=ownership;r->changed=changed;r->data=data;r->display=display;
    wl_list_init(&r->seats);wl_list_init(&r->managers);
    r->global=wl_global_create(display,&zwp_text_input_manager_v3_interface,1,r,bind_manager);
    if(!r->global){free(r);return NULL;}
    r->global_v2=wl_global_create(display,&zwp_text_input_manager_v2_interface,1,r,bind_manager_v2);
    if(!r->global_v2){wl_global_destroy(r->global);free(r);return NULL;}return r;
}
int anco_text_receiver_panel_handler(AncoTextReceiver *r,AncoTextPanelRequest callback)
{if(!r)return -EINVAL;if(r->notifying)return -EBUSY;r->panel=callback;return 0;}
AncoTextSeat *anco_text_receiver_seat(AncoTextReceiver *r)
{
    if(!r || r->notifying){errno=EBUSY;return NULL;}
    AncoTextSeat *s=calloc(1,sizeof(*s));if(!s)return NULL;
    s->receiver=r;wl_list_init(&s->inputs);wl_list_init(&s->bindings);
    wl_list_insert(r->seats.prev,&s->link);return s;
}
int anco_text_seat_bind(AncoTextSeat *s,struct wl_resource *resource)
{
    if(!s || !resource)return -EINVAL;
    if(s->receiver->notifying)return -EBUSY;
    if(strcmp(wl_resource_get_class(resource),"wl_seat"))return -EPROTOTYPE;
    AncoTextSeat *old=find_seat(s->receiver,resource);
    if(old)return old==s?0:-EEXIST;
    Binding *b=calloc(1,sizeof(*b));if(!b)return -ENOMEM;
    b->seat=s;b->resource=resource;b->destroy.notify=binding_destroyed;
    wl_resource_add_destroy_listener(resource,&b->destroy);
    wl_list_insert(s->bindings.prev,&b->link);return 0;
}
int anco_text_seat_focus(AncoTextSeat *s,struct wl_resource *surface)
{
    if(!s)return -EINVAL;
    if(s->receiver->notifying)return -EBUSY;
    if(surface){AncoLinuxWindowOwner window;int e=anco_wayland_ownership_surface_window(
        s->receiver->ownership,surface,&window);if(e)return e;}
    if(surface==s->focus)return 0;
    clear_focus(s);s->focus=surface;
    if(surface){
        s->focus_destroy.notify=focus_destroyed;
        wl_resource_add_destroy_listener(surface,&s->focus_destroy);
        Input *i;wl_list_for_each(i,&s->inputs,link)if(matches(i))
            send_enter(i);
    }
    return 0;
}
void anco_text_receiver_retire(AncoTextReceiver *r,struct wl_resource *surface)
{
    if(!r || r->notifying)return;
    AncoTextSeat *s;wl_list_for_each(s,&r->seats,link){
        if(s->focus==surface)clear_focus(s);
        Input *i;wl_list_for_each(i,&s->inputs,link)if(i->enabled_surface==surface){
            forget_enabled(i);anco_text_v2_clear(&i->state_v2);i->v2_ready=false;
        }
    }
}
int anco_text_seat_commit_end(AncoTextSeat *s,const char *text)
{
    if(!s || !text)return -EINVAL;
    size_t length=0;while(length<=4000 && text[length])++length;
    if(length>4000)return -EMSGSIZE;
    if(!anco_text_utf8_valid(text,length,(int32_t)length,(int32_t)length))return -EINVAL;
    Input *i=s->active;AncoLinuxWindowOwner window;
    if(!i || !admitted(i,&window))return -ENOENT;
    if(i->protocol==2){
        if(!i->v2_ready || i->enabled_surface!=s->focus)return -ENOENT;
        zwp_text_input_v2_send_commit_string(i->resource,text);
    }else{
        if(!i->state.enabled)return -ENOENT;
        zwp_text_input_v3_send_commit_string(i->resource,text);
        zwp_text_input_v3_send_done(i->resource,i->state.serial);
    }
    return 0;
}
int anco_text_seat_surrounding(AncoTextSeat *s,const char **text,
                             int32_t *cursor,int32_t *anchor)
{
    if(!s || !text || !cursor || !anchor)return -EINVAL;
    Input *i=s->active;AncoLinuxWindowOwner window;
    if(!i || !admitted(i,&window))return -ENOENT;
    if(i->protocol==2){
        if(!i->v2_ready || i->enabled_surface!=s->focus)return -ENOENT;
        if(!i->state_v2.current.surrounding_valid)return -ENODATA;
        *text=i->state_v2.current.surrounding;
        *cursor=i->state_v2.current.cursor;*anchor=i->state_v2.current.anchor;
    }else{
        if(!i->state.enabled)return -ENOENT;
        if(!i->state.current.surrounding_valid)return -ENODATA;
        *text=i->state.current.surrounding;
        *cursor=i->state.current.cursor;*anchor=i->state.current.anchor;
    }
    return 0;
}
int anco_text_seat_has_selection(AncoTextSeat *s,bool *selected)
{
    if(!selected)return -EINVAL;
    const char *text;int32_t cursor,anchor;
    int error=anco_text_seat_surrounding(s,&text,&cursor,&anchor);
    if(!error)*selected=cursor!=anchor;
    return error;
}
int anco_text_seat_ime_keysym(AncoTextSeat *s,uint32_t keysym,
                            uint32_t down_time,uint32_t up_time)
{
    if(!s)return -EINVAL;
    Input *i=s->active;AncoLinuxWindowOwner window;
    if(!i || !admitted(i,&window))return -ENOENT;
    if(i->protocol!=2)return -ENOTSUP;
    if(!i->v2_ready || i->enabled_surface!=s->focus)return -ENOENT;
    zwp_text_input_v2_send_keysym(i->resource,down_time,keysym,
                                WL_KEYBOARD_KEY_STATE_PRESSED,0);
    zwp_text_input_v2_send_keysym(i->resource,up_time,keysym,
                                WL_KEYBOARD_KEY_STATE_RELEASED,0);
    return 0;
}
int anco_text_seat_delete_surrounding(AncoTextSeat *s,uint32_t before,
                                      uint32_t after)
{
    if(!s)return -EINVAL;
    Input *i=s->active;AncoLinuxWindowOwner window;
    if(!i || !admitted(i,&window))return -ENOENT;
    if(i->protocol==2){
        if(!i->v2_ready || i->enabled_surface!=s->focus)return -ENOENT;
        zwp_text_input_v2_send_delete_surrounding_text(i->resource,before,after);
        zwp_text_input_v2_send_commit_string(i->resource,"");
    }else{
        if(!i->state.enabled)return -ENOENT;
        zwp_text_input_v3_send_delete_surrounding_text(i->resource,before,after);
        zwp_text_input_v3_send_done(i->resource,i->state.serial);
    }
    return 0;
}
void anco_text_receiver_free(AncoTextReceiver *r)
{
    if(!r || r->notifying)return;
    wl_global_destroy(r->global);
    wl_global_destroy(r->global_v2);
    Manager *m,*mn;wl_list_for_each_safe(m,mn,&r->managers,link)wl_resource_destroy(m->resource);
    AncoTextSeat *s,*sn;wl_list_for_each_safe(s,sn,&r->seats,link){
        clear_focus(s);
        Input *i,*in;wl_list_for_each_safe(i,in,&s->inputs,link)wl_resource_destroy(i->resource);
        Binding *b,*bn;wl_list_for_each_safe(b,bn,&s->bindings,link)binding_destroyed(&b->destroy,NULL);
        wl_list_remove(&s->link);free(s);
    }
    free(r);
}
