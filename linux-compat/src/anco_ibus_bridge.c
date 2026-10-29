#include "anco_ibus_bridge.h"
#include <ibus.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define ANCO_IBUS_ENGINE_NAME "anco-original-imf"

typedef struct {
    IBusEngine parent;
    AncoIbusBridge *bridge;
} AncoIbusEngine;
typedef IBusEngineClass AncoIbusEngineClass;

typedef struct {
    IBusPanelService parent;
    AncoIbusBridge *bridge;
} AncoIbusPanel;
typedef IBusPanelServiceClass AncoIbusPanelClass;

struct AncoIbusBridge {
    AncoLinuxImfRuntime *imf;
    AncoIbusBridgeOps ops;
    void *data;
    IBusBus *bus;
    IBusFactory *factory;
    IBusComponent *component;
    IBusPanelService *panel;
    GPid daemon_pid;
    IBusEngine *active;
    gchar *surrounding;
    guint cursor_chars, anchor_chars;
    gint cursor_x, cursor_y, cursor_width, cursor_height;
    guint64 serial;
    gboolean admitted;
    gboolean panel_requested;
    gboolean cursor_valid;
    gunichar2 query_reply[4001];
};

G_DEFINE_TYPE(AncoIbusEngine, anco_ibus_engine, IBUS_TYPE_ENGINE)
G_DEFINE_TYPE(AncoIbusPanel, anco_ibus_panel, IBUS_TYPE_PANEL_SERVICE)

static gboolean engine_process_key(IBusEngine *engine, guint keyval,
    guint keycode, guint state)
{
    (void)engine;(void)keyval;(void)keycode;(void)state;
    /* Physical keys already traverse the original AnCo IMF consumption gate
     * before Xwayland. Returning FALSE preserves normal X11 delivery and
     * prevents a second host dispatch. */
    return FALSE;
}

static void engine_set_surrounding(IBusEngine *base,IBusText *text,
    guint cursor,guint anchor);

static void engine_enable(IBusEngine *base)
{
    /* libibus requires an engine which consumes surrounding text to make
     * this call from its enable handler.  It advertises that requirement to
     * the real X11 input context, which then supplies set-surrounding-text;
     * the original AnCo IMF query callbacks can consequently read the same
     * editor state that Wayland text-input submits directly. */
    ibus_engine_get_surrounding_text(base,NULL,NULL,NULL);
    puts("RUNTIME_IBUS_ENABLE surrounding_requested=1 standard_engine=1");
}

static void engine_focus_in(IBusEngine *base)
{
    AncoIbusEngine *engine=(AncoIbusEngine *)base;
    AncoIbusBridge *bridge=engine->bridge;
    if(!bridge)return;
    if(bridge->active && bridge->active!=base && bridge->admitted){
        if(bridge->panel_requested && bridge->ops.panel)
            bridge->ops.panel(bridge->data,FALSE);
        bridge->ops.focus(bridge->data,FALSE);
    }
    bridge->active=base;
    bridge->panel_requested=FALSE;
    bridge->admitted=bridge->ops.focus && bridge->ops.focus(bridge->data,TRUE);
    /* IBus initializes every engine with a valid empty surrounding-text
     * cache.  The real editor can replace it later through
     * set-surrounding-text, but an input context without that capability must
     * still answer the original IMF before/after-cursor query successfully.
     * Synchronize the standard cached value at the admitted editor focus;
     * the earlier global-engine Enable may belong to IBus' fake context. */
    IBusText *surrounding=NULL;guint cursor=0,anchor=0;
    if(bridge->admitted) {
        ibus_engine_get_surrounding_text(base,&surrounding,&cursor,&anchor);
        if(surrounding)engine_set_surrounding(base,surrounding,cursor,anchor);
    }
    printf("RUNTIME_IBUS_FOCUS serial=%llu admitted=%d surrounding_cached=%d standard_engine=1\n",
        (unsigned long long)++bridge->serial,bridge->admitted,surrounding!=NULL);
}

static void engine_focus_out(IBusEngine *base)
{
    AncoIbusEngine *engine=(AncoIbusEngine *)base;
    AncoIbusBridge *bridge=engine->bridge;
    if(!bridge || bridge->active!=base)return;
    if(bridge->admitted && bridge->panel_requested && bridge->ops.panel)
        bridge->ops.panel(bridge->data,FALSE);
    if(bridge->admitted && bridge->ops.focus)
        bridge->ops.focus(bridge->data,FALSE);
    bridge->active=NULL;bridge->admitted=FALSE;bridge->panel_requested=FALSE;
    g_clear_pointer(&bridge->surrounding,g_free);
    bridge->cursor_chars=bridge->anchor_chars=0;
    printf("RUNTIME_IBUS_BLUR serial=%llu standard_engine=1\n",
        (unsigned long long)++bridge->serial);
}

static void engine_set_cursor(IBusEngine *base,gint x,gint y,gint w,gint h)
{
    AncoIbusEngine *engine=(AncoIbusEngine *)base;
    AncoIbusBridge *bridge=engine->bridge;
    if(!bridge || bridge->active!=base || h<=0)return;
    bridge->cursor_x=x;bridge->cursor_y=y;bridge->cursor_width=w;
    bridge->cursor_height=h;bridge->cursor_valid=TRUE;
    if(!bridge->admitted)return;
    if(bridge->ops.cursor)bridge->ops.cursor(bridge->data,x,y,w,h);
    /* IBus FocusIn is followed by Enable and the editor's first valid cursor
     * location.  The global engine can already be enabled by IBus' fake
     * context before any Linux window exists, so that one-shot Enable cannot
     * represent the real editor becoming visible.  Complete the original
     * AnCo attach/show lifecycle only after this admitted editor supplies its
     * cursor, matching Wayland text-input's focus/cursor/show ordering. */
    if(!bridge->panel_requested && bridge->ops.panel){
        bridge->ops.panel(bridge->data,TRUE);
        bridge->panel_requested=TRUE;
    }
    printf("RUNTIME_IBUS_CURSOR local=%d,%d,%d,%d standard_engine=1\n",x,y,w,h);
}

static void panel_set_cursor_relative(IBusPanelService *base,gint x,gint y,
    gint w,gint h)
{
    AncoIbusPanel *panel=(AncoIbusPanel *)base;
    AncoIbusBridge *bridge=panel->bridge;
    if(!bridge || !bridge->active || h<=0)return;
    bridge->cursor_x=x;bridge->cursor_y=y;bridge->cursor_width=w;
    bridge->cursor_height=h;bridge->cursor_valid=TRUE;
    if(!bridge->admitted)return;
    if(bridge->ops.cursor)bridge->ops.cursor(bridge->data,x,y,w,h);
    if(!bridge->panel_requested && bridge->ops.panel){
        bridge->ops.panel(bridge->data,TRUE);
        bridge->panel_requested=TRUE;
    }
    printf("RUNTIME_IBUS_CURSOR_RELATIVE local=%d,%d,%d,%d standard_panel=1\n",
        x,y,w,h);
}

static void panel_destroy(IBusObject *object)
{
    AncoIbusPanel *panel=(AncoIbusPanel *)object;
    panel->bridge=NULL;
    IBUS_OBJECT_CLASS(anco_ibus_panel_parent_class)->destroy(object);
}

static void anco_ibus_panel_class_init(AncoIbusPanelClass *klass)
{
    IBusPanelServiceClass *panel=IBUS_PANEL_SERVICE_CLASS(klass);
    IBusObjectClass *object=IBUS_OBJECT_CLASS(klass);
    panel->set_cursor_location_relative=panel_set_cursor_relative;
    object->destroy=panel_destroy;
}

static void anco_ibus_panel_init(AncoIbusPanel *panel)
{ panel->bridge=NULL; }

void anco_ibus_bridge_host_imf_reset(AncoIbusBridge *bridge)
{
    if(!bridge || !bridge->active || !bridge->ops.focus)return;
    const gboolean restore_panel=bridge->panel_requested;
    if(bridge->admitted && restore_panel && bridge->ops.panel)
        bridge->ops.panel(bridge->data,FALSE);
    if(bridge->admitted)bridge->ops.focus(bridge->data,FALSE);
    bridge->admitted=bridge->ops.focus(bridge->data,TRUE);
    bridge->panel_requested=FALSE;
    if(bridge->admitted && bridge->surrounding && bridge->ops.surrounding)
        bridge->ops.surrounding(bridge->data,bridge->surrounding,
            bridge->cursor_chars,bridge->anchor_chars);
    if(bridge->admitted && bridge->cursor_valid && bridge->ops.cursor)
        bridge->ops.cursor(bridge->data,bridge->cursor_x,bridge->cursor_y,
            bridge->cursor_width,bridge->cursor_height);
    if(bridge->admitted && restore_panel && bridge->ops.panel){
        bridge->ops.panel(bridge->data,TRUE);
        bridge->panel_requested=TRUE;
    }
    printf("RUNTIME_IBUS_HOST_IMF_RESET serial=%llu admitted=%d surrounding_cached=%d cursor_cached=%d panel_restored=%d original_remove_window_reset=1\n",
        (unsigned long long)++bridge->serial,bridge->admitted,
        bridge->surrounding!=NULL,bridge->cursor_valid,bridge->panel_requested);
}

static guint clamp_chars(const char *text,guint value)
{
    const glong length=g_utf8_strlen(text,-1);
    return (guint)MIN((gulong)value,(gulong)MAX(length,0));
}

static void engine_set_surrounding(IBusEngine *base,IBusText *text,
    guint cursor,guint anchor)
{
    AncoIbusEngine *engine=(AncoIbusEngine *)base;
    AncoIbusBridge *bridge=engine->bridge;
    if(!bridge || bridge->active!=base || !bridge->admitted || !text)return;
    const char *utf8=ibus_text_get_text(text);
    if(!utf8 || !g_utf8_validate(utf8,-1,NULL) || strlen(utf8)>16000)return;
    g_free(bridge->surrounding);bridge->surrounding=g_strdup(utf8);
    bridge->cursor_chars=clamp_chars(utf8,cursor);
    bridge->anchor_chars=clamp_chars(utf8,anchor);
    if(bridge->ops.surrounding)bridge->ops.surrounding(bridge->data,utf8,
        bridge->cursor_chars,bridge->anchor_chars);
    printf("RUNTIME_IBUS_SURROUNDING bytes=%zu cursor_chars=%u anchor_chars=%u standard_engine=1\n",
        strlen(utf8),bridge->cursor_chars,bridge->anchor_chars);
}

static void engine_destroy(IBusObject *object)
{
    AncoIbusEngine *engine=(AncoIbusEngine *)object;
    if(engine->bridge && engine->bridge->active==IBUS_ENGINE(engine))
        engine_focus_out(IBUS_ENGINE(engine));
    engine->bridge=NULL;
    IBUS_OBJECT_CLASS(anco_ibus_engine_parent_class)->destroy(object);
}

static void anco_ibus_engine_class_init(AncoIbusEngineClass *klass)
{
    IBusEngineClass *engine=IBUS_ENGINE_CLASS(klass);
    IBusObjectClass *object=IBUS_OBJECT_CLASS(klass);
    engine->process_key_event=engine_process_key;
    engine->enable=engine_enable;
    engine->focus_in=engine_focus_in;
    engine->focus_out=engine_focus_out;
    engine->set_cursor_location=engine_set_cursor;
    engine->set_surrounding_text=engine_set_surrounding;
    object->destroy=engine_destroy;
}

static void anco_ibus_engine_init(AncoIbusEngine *engine)
{ engine->bridge=NULL; }

static IBusEngine *create_engine(IBusFactory *factory,const gchar *name,
    gpointer data)
{
    (void)factory;
    AncoIbusBridge *bridge=data;
    if(!bridge || strcmp(name,ANCO_IBUS_ENGINE_NAME))return NULL;
    g_autofree char *path=g_strdup_printf(
        "/org/freedesktop/IBus/engine/anco_original_imf/%llu",
        (unsigned long long)++bridge->serial);
    IBusEngine *base=ibus_engine_new_with_type(anco_ibus_engine_get_type(),name,
        path,ibus_bus_get_connection(bridge->bus));
    if(base)((AncoIbusEngine *)base)->bridge=bridge;
    printf("RUNTIME_IBUS_ENGINE_CREATE serial=%llu created=%d\n",
        (unsigned long long)bridge->serial,base!=NULL);
    return base;
}

static void global_engine_selected(GObject *object,GAsyncResult *result,
    gpointer data)
{
    AncoIbusBridge *bridge=data;GError *error=NULL;
    const gboolean selected=ibus_bus_set_global_engine_async_finish(
        IBUS_BUS(object),result,&error);
    if(selected)
        printf("RUNTIME_IBUS_BRIDGE_READY standard_bus=1 standard_engine=1 original_imf=1 key_injection=0 daemon_pid=%ld\n",
            (long)bridge->daemon_pid);
    else
        fprintf(stderr,"RUNTIME_IBUS_ENGINE_SELECT_FAILED %s\n",
            error?error->message:"unknown error");
    g_clear_error(&error);
}

AncoIbusBridge *anco_ibus_bridge_new(AncoLinuxImfRuntime *imf,
    const AncoIbusBridgeOps *ops,void *data)
{
    if(!imf||!ops||!ops->focus)return NULL;
    AncoIbusBridge *bridge=g_new0(AncoIbusBridge,1);
    bridge->imf=imf;bridge->ops=*ops;bridge->data=data;
    const char *address="unix:path=/run/l410-anco/ibus";
    g_setenv("IBUS_ADDRESS",address,TRUE);
    g_setenv("QT_IM_MODULE","ibus",TRUE);
    g_setenv("GTK_IM_MODULE","ibus",TRUE);
    g_setenv("XMODIFIERS","@im=ibus",TRUE);
    unlink("/run/l410-anco/ibus");
    ibus_init();
    gchar *daemon_argv[]={"/usr/bin/ibus-daemon","--single",
        "--panel=disable","--config=disable","--emoji-extension=disable",
        "--cache=none","--address=unix:path=/run/l410-anco/ibus",NULL};
    GError *error=NULL;
    if(!g_spawn_async(NULL,daemon_argv,NULL,G_SPAWN_DO_NOT_REAP_CHILD,
        NULL,NULL,&bridge->daemon_pid,&error)){
        fprintf(stderr,"RUNTIME_IBUS_DAEMON_ERROR %s\n",
            error?error->message:"spawn failed");
        g_clear_error(&error);goto fail;
    }
    for(guint attempt=0;attempt<100;++attempt){
        bridge->bus=ibus_bus_new();
        if(bridge->bus&&ibus_bus_is_connected(bridge->bus))break;
        g_clear_object(&bridge->bus);g_usleep(20000);
    }
    if(!bridge->bus||!ibus_bus_is_connected(bridge->bus))goto fail;
    bridge->panel=IBUS_PANEL_SERVICE(g_object_new(anco_ibus_panel_get_type(),
        "object-path",IBUS_PATH_PANEL,
        "connection",ibus_bus_get_connection(bridge->bus),NULL));
    if(!bridge->panel)goto fail;
    ((AncoIbusPanel *)bridge->panel)->bridge=bridge;
    const guint32 panel_name=ibus_bus_request_name(bridge->bus,
        IBUS_SERVICE_PANEL,0);
    if(!panel_name)goto fail;
    printf("RUNTIME_IBUS_PANEL_READY standard_panel=1 relative_cursor=1 request_name=%u\n",
        panel_name);
    bridge->factory=ibus_factory_new(ibus_bus_get_connection(bridge->bus));
    if(!bridge->factory)goto fail;
    g_signal_connect(bridge->factory,"create-engine",G_CALLBACK(create_engine),bridge);
    bridge->component=ibus_component_new("org.l410.AncoOriginalImf",
        "AnCo original IMF bridge","1.0","LGPL","L410 Linux Compatibility",
        "","","ibus");
    IBusEngineDesc *desc=ibus_engine_desc_new(ANCO_IBUS_ENGINE_NAME,
        "AnCo Original Input Method","HarmonyOS AnCo IMF","zh_CN","LGPL",
        "L410 Linux Compatibility","","us");
    if(!bridge->component||!desc)goto fail;
    ibus_component_add_engine(bridge->component,desc);
    if(!ibus_bus_register_component(bridge->bus,bridge->component))goto fail;
    anco_linux_imf_runtime_set_external_consumer(imf,
        (AncoLinuxImfExternalConsumer)anco_ibus_bridge_consume,bridge);
    ibus_bus_set_global_engine_async(bridge->bus,ANCO_IBUS_ENGINE_NAME,-1,NULL,
        global_engine_selected,bridge);
    printf("RUNTIME_IBUS_BRIDGE_REGISTERED standard_bus=1 standard_engine=1 original_imf=1 daemon_pid=%ld async_select=1\n",
        (long)bridge->daemon_pid);
    return bridge;
fail:
    puts("RUNTIME_IBUS_BRIDGE_FAILED standard_bus=1 original_imf_unchanged=1");
    anco_ibus_bridge_free(bridge);return NULL;
}

void anco_ibus_bridge_free(AncoIbusBridge *bridge)
{
    if(!bridge)return;
    anco_linux_imf_runtime_set_external_consumer(bridge->imf,NULL,NULL);
    if(bridge->admitted && bridge->ops.focus)bridge->ops.focus(bridge->data,FALSE);
    bridge->active=NULL;bridge->admitted=FALSE;
    if(bridge->bus && bridge->panel)
        ibus_bus_release_name(bridge->bus,IBUS_SERVICE_PANEL);
    if(bridge->panel)ibus_object_destroy(IBUS_OBJECT(bridge->panel));
    if(bridge->factory)ibus_object_destroy(IBUS_OBJECT(bridge->factory));
    if(bridge->bus)g_object_unref(bridge->bus);
    if(bridge->component)g_object_unref(bridge->component);
    if(bridge->daemon_pid){
        kill(bridge->daemon_pid,SIGTERM);
        int status=0;
        while(waitpid(bridge->daemon_pid,&status,0)<0&&errno==EINTR){}
        g_spawn_close_pid(bridge->daemon_pid);bridge->daemon_pid=0;
    }
    g_free(bridge->surrounding);g_free(bridge);
    puts("RUNTIME_IBUS_BRIDGE_RETIRE standard_engine=1");
}

static char *insert_utf8(const guint8 *bytes,gsize units)
{
    if(!bytes||units>4000)return NULL;
    gunichar2 text[4001];
    for(gsize n=0;n<units;++n){
        text[n]=(gunichar2)(bytes[2*n]|((guint16)bytes[2*n+1]<<8));
        if(!text[n])return NULL;
    }
    text[units]=0;glong written=0;
    char *utf8=g_utf16_to_utf8(text,(glong)units,NULL,&written,NULL);
    if(!utf8||written>16000){g_free(utf8);return NULL;}
    return utf8;
}

static gsize utf16_units_before_chars(const char *text,guint chars)
{
    gsize units=0;const char *p=text;
    for(guint n=0;n<chars && *p;++n,p=g_utf8_next_char(p))
        units+=g_utf8_get_char(p)>0xffff?2:1;
    return units;
}

static guint chars_for_utf16(const char *start,const char *limit,guint units,
    gboolean backward)
{
    guint chars=0,used=0;const char *p=start;
    while(units>used){
        const char *next=backward?g_utf8_prev_char(p):g_utf8_next_char(p);
        if((backward&&next<limit)||(!backward&&next>limit))break;
        guint amount=g_utf8_get_char(backward?next:p)>0xffff?2:1;
        if(used+amount>units)break;
        used+=amount;++chars;p=next;
    }
    return chars;
}

static int forward_editor_key(AncoIbusBridge *bridge,int32_t android_key)
{
    /* The physical event was consumed by the original AnCo IMF gate.  Its
     * reverse callback is the current-connection result, so deliver the
     * corresponding Android editor key through the runtime's existing
     * focused wl_keyboard route.  It then reaches this X11 input context as a
     * normal key and the engine returns FALSE from process_key_event, exactly
     * once; no global injection or second focus authority is introduced. */
    return anco_linux_imf_runtime_editor_key(bridge->imf,android_key);
}

static void query_surrounding(AncoIbusBridge *bridge,
    const AncoImfCallbackMessage *message,AncoImfCallbackResult *result)
{
    if(message->data.query.flags||message->data.query.length<0||
       !bridge->surrounding)return;
    glong total=0;gunichar2 *utf16=g_utf8_to_utf16(bridge->surrounding,-1,
        NULL,&total,NULL);if(!utf16)return;
    gsize cursor=utf16_units_before_chars(bridge->surrounding,bridge->cursor_chars);
    gsize anchor=utf16_units_before_chars(bridge->surrounding,bridge->anchor_chars);
    gsize start=MIN(cursor,anchor),end=MAX(cursor,anchor);
    gsize count=(gsize)message->data.query.length;
    if(message->code==4){count=MIN(count,start);start-=count;}
    else {start=end;count=MIN(count,(gsize)total-end);}
    memcpy(bridge->query_reply,utf16+start,count*sizeof(gunichar2));
    bridge->query_reply[count]=0;g_free(utf16);
    result->text=bridge->query_reply;result->units=count;result->status=0;
}

gboolean anco_ibus_bridge_consume(AncoIbusBridge *bridge,
    const AncoImfCallbackMessage *message,AncoImfCallbackResult *result)
{
    if(!bridge||!message||!result||!bridge->active||!bridge->admitted)return FALSE;
    *result=(AncoImfCallbackResult){.status=-1};
    switch(message->code){
    case 1: {
        if(message->data.insert.cursor!=1)return TRUE;
        g_autofree char *text=insert_utf8(message->data.insert.utf16,
            message->data.insert.units);
        if(!text)return TRUE;
        ibus_engine_commit_text(bridge->active,ibus_text_new_from_string(text));
        result->status=0;break;
    }
    case 2: case 3: {
        if(message->data.deletion.before<0||message->data.deletion.after<0)return TRUE;
        const gboolean selected=bridge->surrounding &&
            bridge->cursor_chars!=bridge->anchor_chars;
        const gint32 requested_before=message->data.deletion.before;
        const gint32 requested_after=message->data.deletion.after;
        /* Original AncoImeProxy gives a selection priority, then maps the
         * one-character backward/forward cases to key67/key112 on the current
         * InputConnection.  Preserve that distinction through the standard
         * IBus current context; only general ranges use surrounding deletion. */
        if(selected || (requested_before==1 && requested_after==0)){
            result->status=forward_editor_key(bridge,67);break;
        }
        if(requested_before==0 && requested_after==1){
            result->status=forward_editor_key(bridge,112);break;
        }
        gint offset=0;guint count=0;
        if(bridge->surrounding){
            const char *cursor=g_utf8_offset_to_pointer(bridge->surrounding,
                bridge->cursor_chars);
            const guint before=chars_for_utf16(cursor,bridge->surrounding,
                (guint)message->data.deletion.before,TRUE);
            const guint total_chars=(guint)g_utf8_strlen(bridge->surrounding,-1);
            const char *end=g_utf8_offset_to_pointer(bridge->surrounding,total_chars);
            const guint after=chars_for_utf16(cursor,end,
                (guint)message->data.deletion.after,FALSE);
            offset=-(gint)before;count=before+after;
        }else{
            offset=-message->data.deletion.before;
            count=(guint)(message->data.deletion.before+message->data.deletion.after);
        }
        if(count)ibus_engine_delete_surrounding_text(bridge->active,offset,count);
        result->status=0;break;
    }
    case 4: case 5: query_surrounding(bridge,message,result);break;
    case 7:
        result->status=forward_editor_key(bridge,66);break;
    default:return FALSE;
    }
    printf("RUNTIME_IBUS_CALLBACK code=%u status=%d standard_engine=1\n",
        message->code,result->status);
    return TRUE;
}
