/* Part of the SAME receiver and logical-seat owner, not a parallel runtime.
 * v2 stores one current enabled surface per resource (Qt-style Linux mapping).
 * Effective selection follows an explicit enable or committed update of that
 * focused enabled resource. Replacement invalidates old before notifying new.
 * This chooses a Linux current connection; original IMF policy is in consumer. */
static void v2_select(Input *i,bool boundary)
{
    AncoLinuxWindowOwner window;AncoTextSeat *s=i->seat;
    if(!i->v2_ready || i->enabled_surface!=s->focus || !admitted(i,&window))return;
    if(s->active!=i){
        if(s->active)notify(s->active,NULL,true);
        s->active=i;boundary=true;
    }
    notify(i,&window,boundary);
}
static void v2_enable(struct wl_client *client,struct wl_resource *r,struct wl_resource *surface)
{
    Input *i=wl_resource_get_user_data(r);
    if(wl_resource_get_client(surface)!=client){
        wl_resource_post_error(r,WL_DISPLAY_ERROR_INVALID_OBJECT,"foreign enabled surface");return;
    }
    if(i->enabled_surface==surface)return;
    if(i->seat->active==i){notify(i,NULL,true);i->seat->active=NULL;}
    forget_enabled(i);i->enabled_surface=surface;i->enabled_destroy.notify=enabled_destroyed;
    wl_resource_add_destroy_listener(surface,&i->enabled_destroy);
    v2_select(i,true);
}
static void v2_disable(struct wl_client *client,struct wl_resource *r,struct wl_resource *surface)
{
    (void)client;Input *i=wl_resource_get_user_data(r);
    if(i->enabled_surface!=surface)return;
    if(i->seat->active==i){notify(i,NULL,true);i->seat->active=NULL;}
    forget_enabled(i);
}
static void v2_surrounding(struct wl_client *client,struct wl_resource *r,const char *text,int32_t cursor,int32_t anchor)
{
    (void)client;Input *i=wl_resource_get_user_data(r);
    if(request_owner(r) && !anco_text_v2_surrounding(&i->state_v2,text,cursor,anchor))
        wl_resource_post_error(r,WL_DISPLAY_ERROR_IMPLEMENTATION,"invalid surrounding UTF-8/range");
}
static void v2_content(struct wl_client *client,struct wl_resource *r,uint32_t hint,uint32_t purpose)
{
    (void)client;Input *i=wl_resource_get_user_data(r);if(!request_owner(r))return;
    if((hint&~0x3ffu)||purpose>12){wl_resource_post_error(r,WL_DISPLAY_ERROR_IMPLEMENTATION,"invalid v2 content type");return;}
    anco_text_v2_content(&i->state_v2,hint,purpose);
}
static void v2_rectangle(struct wl_client *client,struct wl_resource *r,int32_t x,int32_t y,int32_t w,int32_t h)
{
    (void)client;Input *i=wl_resource_get_user_data(r);
    if(request_owner(r) && !anco_text_v2_rectangle(&i->state_v2,x,y,w,h))
        wl_resource_post_error(r,WL_DISPLAY_ERROR_IMPLEMENTATION,"invalid cursor rectangle");
}
static void v2_language(struct wl_client *client,struct wl_resource *r,const char *language)
{
    Input *i=wl_resource_get_user_data(r);
    if(request_owner(r) && anco_text_v2_language(&i->state_v2,language)==-ENOMEM)wl_client_post_no_memory(client);
}
static void v2_update(struct wl_client *client,struct wl_resource *r,uint32_t serial,uint32_t reason)
{
    Input *i=wl_resource_get_user_data(r);if(!request_owner(r))return;
    int error=anco_text_v2_update(&i->state_v2,serial,reason);
    if(error){
        if(error==-ENOMEM)wl_client_post_no_memory(client);
        else wl_resource_post_error(r,WL_DISPLAY_ERROR_IMPLEMENTATION,"invalid v2 update reason");
        return;
    }
    i->v2_ready=true;v2_select(i,reason==2||reason==3);
}
static void v2_panel(struct wl_resource *r,bool show)
{
    Input *i=wl_resource_get_user_data(r);AncoTextReceiver *receiver=i->seat->receiver;
    AncoLinuxWindowOwner window;
    if(!receiver->panel || i->seat->active!=i || !admitted(i,&window))return;
    receiver->notifying=true;
    receiver->panel(receiver->data,i->seat,r,&window,show);
    receiver->notifying=false;
}
static void v2_show(struct wl_client *c,struct wl_resource *r){(void)c;v2_panel(r,true);}
static void v2_hide(struct wl_client *c,struct wl_resource *r){(void)c;v2_panel(r,false);}
static const struct zwp_text_input_v2_interface v2_impl={
    .destroy=destroy_request,.enable=v2_enable,.disable=v2_disable,
    .show_input_panel=v2_show,.hide_input_panel=v2_hide,.set_surrounding_text=v2_surrounding,
    .set_content_type=v2_content,.set_cursor_rectangle=v2_rectangle,
    .set_preferred_language=v2_language,.update_state=v2_update
};
