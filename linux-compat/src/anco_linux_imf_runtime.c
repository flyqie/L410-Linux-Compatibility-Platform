#include "anco_linux_imf_runtime.h"
#include "anco_imf_broker.h"
#include "anco_imf_callback_receiver.h"
#include "anco_imf_death.h"
#include "anco_imf_editor_consumer.h"
#include "anco_original_ime_key.h"
#include "gbinder_ipc.h"
#include "gbinder_local_request_p.h"
#include <errno.h>
#include <glib-unix.h>
#include <sys/eventfd.h>
#include <unistd.h>

typedef enum {COMMAND_FOCUS,COMMAND_BLUR,COMMAND_KEY,COMMAND_CURSOR,COMMAND_SELECTION,
              COMMAND_PANEL,COMMAND_STOP} CommandType;
typedef struct {
    CommandType type;guint64 generation;
    union {
        struct {bool show;int32_t pattern,enter,option;} focus;
        AncoNativeInputKey key;
        struct {double x,y,height;} cursor;
        struct {char *text;int32_t old_begin,old_end,new_begin,new_end;} selection;
        bool panel_show;
    } data;
} Command;
typedef enum {RESULT_FOCUS,RESULT_KEY,RESULT_CURSOR,RESULT_SELECTION,RESULT_PANEL} ResultType;
typedef struct {
    ResultType type;guint64 generation;
    AncoNativeInputKey key;
    bool decoded,consumed;int32_t status;
} Result;
typedef struct {int32_t device,key;bool consumed;} Pair;

struct AncoLinuxImfRuntime {
    GAsyncQueue *commands,*results;
    GThread *worker;
    int event_fd;guint event_source;
    guint64 generation;
    bool enabled,stopping;
    bool selection_valid;
    int32_t selection_begin,selection_end;
    Pair pairs[64];size_t pair_count;
    AncoLinuxImfKeyDone key_done;void *data;
    AncoImfBroker *broker;AncoImfDeath *death;
    AncoImfCallbackReceiver *receiver;
    AncoImeStatus status;AncoImfEditorConsumer consumer;
    AncoImeServiceView service;
    bool service_shown,service_window_shown;int32_t service_height;
    AncoLinuxImfExternalConsumer external_consumer;void *external_data;
};

static void command_free(Command *command)
{
    if(!command)return;
    if(command->type==COMMAND_SELECTION)g_free(command->data.selection.text);
    g_free(command);
}

static bool service_is_shown(void *data)
{ return ((AncoLinuxImfRuntime *)data)->service_shown; }
static bool service_hardware_keyboard(void *data)
{ (void)data;return true; }
static bool service_collaboration(void *data)
{ (void)data;return false; }
static void service_request_show(void *data,int32_t flags)
{
    AncoLinuxImfRuntime *self=data;self->service_shown=true;
    printf("RUNTIME_IMF_SERVICE_SHOW flags=%d source=original_status input_view_shown=1\n",flags);
}
static void service_request_hide(void *data,int32_t flags)
{
    AncoLinuxImfRuntime *self=data;self->service_shown=false;
    printf("RUNTIME_IMF_SERVICE_HIDE flags=%d source=original_status input_view_shown=0\n",flags);
}
static void service_height(void *data,int32_t height,bool by_self)
{
    AncoLinuxImfRuntime *self=data;self->service_height=height;
    printf("RUNTIME_IMF_SERVICE_HEIGHT height=%d by_self=%d\n",height,by_self);
}
static void service_window_shown(void *data)
{
    AncoLinuxImfRuntime *self=data;self->service_window_shown=true;
    printf("RUNTIME_IMF_SERVICE_WINDOW shown=1 original_callback=1004 input_view_shown=%d\n",
        self->service_shown);
}
static void service_window_hidden(void *data)
{
    AncoLinuxImfRuntime *self=data;self->service_window_shown=false;
    printf("RUNTIME_IMF_SERVICE_WINDOW shown=0 original_callback=1005 input_view_shown=%d\n",
        self->service_shown);
}

static void emit(AncoLinuxImfRuntime *self,Result *result)
{
    g_async_queue_push(self->results,result);
    eventfd_write(self->event_fd,1);
}

static gpointer worker_main(gpointer data)
{
    AncoLinuxImfRuntime *self=data;
    AncoImfClient *client=NULL;guint64 generation=0;bool ready=false;
    AncoOriginalImeKeyState keys;anco_original_ime_key_reset(&keys);
    for(;;){
        Command *command=g_async_queue_pop(self->commands);
        if(command->type==COMMAND_STOP){command_free(command);break;}
        Result *result=g_new0(Result,1);result->generation=command->generation;
        result->status=-1;
        switch(command->type){
        case COMMAND_FOCUS:
            anco_imf_client_close(client);client=anco_imf_client_open();
            generation=command->generation;ready=false;
            anco_original_ime_key_reset(&keys);result->type=RESULT_FOCUS;
            if(client && anco_imf_attach(client,command->data.focus.show,
                command->data.focus.pattern,command->data.focus.enter,
                command->data.focus.option,&result->status) && !result->status){
                int32_t shown=-1;result->decoded=anco_imf_show(client,&shown);
                result->status=shown;ready=result->decoded&&!shown;
            }
            break;
        case COMMAND_BLUR:
            anco_imf_client_close(client);client=NULL;ready=false;
            generation=command->generation;anco_original_ime_key_reset(&keys);
            g_free(result);result=NULL;break;
        case COMMAND_KEY: {
            result->type=RESULT_KEY;result->key=command->data.key;
            if(ready&&client&&command->generation==generation){
                AncoImfKeyAttribute attribute;const AncoImfKeyItem *items=NULL;
                size_t count=0;
                if(!anco_original_ime_key_convert(&keys,&command->data.key,
                    &attribute,&items,&count))
                    result->decoded=anco_imf_dispatch_key(client,&attribute,items,
                        count,&result->status,&result->consumed);
            }
            break;
        }
        case COMMAND_CURSOR:
            result->type=RESULT_CURSOR;
            if(ready&&client&&command->generation==generation)
                result->decoded=anco_imf_cursor_update(client,command->data.cursor.x,
                    command->data.cursor.y,command->data.cursor.height,&result->status);
            break;
        case COMMAND_SELECTION:
            result->type=RESULT_SELECTION;
            if(ready&&client&&command->generation==generation)
                result->decoded=anco_imf_selection_change(client,
                    command->data.selection.text,
                    command->data.selection.old_begin,
                    command->data.selection.old_end,
                    command->data.selection.new_begin,
                    command->data.selection.new_end,&result->status);
            break;
        case COMMAND_PANEL:
            result->type=RESULT_PANEL;result->consumed=command->data.panel_show;
            if(client&&command->generation==generation)
                result->decoded=command->data.panel_show?anco_imf_show(client,&result->status):
                    anco_imf_hide(client,&result->status);
            ready=command->data.panel_show&&result->decoded&&!result->status;
            break;
        case COMMAND_STOP:break;
        }
        command_free(command);if(result)emit(self,result);
    }
    anco_imf_client_close(client);return NULL;
}

static bool normalized_consumed(AncoLinuxImfRuntime *self,
    const AncoNativeInputKey *key,bool consumed)
{
    size_t found=self->pair_count;
    for(size_t n=0;n<self->pair_count;++n)
        if(self->pairs[n].device==key->device_id&&self->pairs[n].key==key->android_key){found=n;break;}
    if(key->android_action==0){
        if(found==self->pair_count&&self->pair_count<G_N_ELEMENTS(self->pairs))
            self->pairs[self->pair_count++]=(Pair){key->device_id,key->android_key,consumed};
        else if(found<self->pair_count)self->pairs[found].consumed=consumed;
    }else if(found<self->pair_count){
        consumed=self->pairs[found].consumed;
        self->pairs[found]=self->pairs[--self->pair_count];
    }
    return consumed;
}

static gboolean results_ready(gint fd,GIOCondition condition,gpointer data)
{
    AncoLinuxImfRuntime *self=data;eventfd_t value;
    while(eventfd_read(fd,&value)==0){}
    Result *result;
    while((result=g_async_queue_try_pop(self->results))){
        const bool stale=self->stopping||result->generation!=self->generation;
        if(result->type==RESULT_FOCUS){
            printf("RUNTIME_IMF_FOCUS generation=%llu decoded=%d status=%d stale=%d input_view_shown=%d window_notification_shown=%d\n",
                (unsigned long long)result->generation,result->decoded,result->status,stale,
                self->service_shown,self->service_window_shown);
        }else if(result->type==RESULT_KEY){
            bool consumed=result->decoded&&!result->status&&result->consumed;
            if(!stale)consumed=normalized_consumed(self,&result->key,consumed);
            printf("RUNTIME_IMF_KEY generation=%llu event=%d decoded=%d status=%d consumed=%d stale=%d\n",
                (unsigned long long)result->generation,result->key.event_id,
                result->decoded,result->status,consumed,stale);
            if(self->key_done)self->key_done(self->data,&result->key,consumed,stale);
        }else{
            printf("RUNTIME_IMF_RESULT type=%d generation=%llu decoded=%d status=%d stale=%d input_view_shown=%d requested_panel=%d\n",
                result->type,(unsigned long long)result->generation,result->decoded,result->status,
                stale,self->service_shown,result->type==RESULT_PANEL?result->consumed:-1);
        }
        g_free(result);
    }
    return condition&(G_IO_HUP|G_IO_ERR|G_IO_NVAL)?G_SOURCE_REMOVE:G_SOURCE_CONTINUE;
}

static void broker_died(AncoImfBroker *broker,void *data)
{
    (void)broker;AncoLinuxImfRuntime *self=data;
    self->enabled=false;++self->generation;self->pair_count=0;
    puts("RUNTIME_IMF_BROKER_DIED editor_retired=1");
}
static void consume(const AncoImfCallbackMessage *message,
    const AncoCallbackHeader *header,AncoImfCallbackResult *result,void *data)
{
    AncoLinuxImfRuntime *self=data;
    if(message) {
        printf("RUNTIME_IMF_CALLBACK code=%u insert_units=%zu insert_cursor=%d keyboard_status=%d input_view_shown=%d window_notification_shown=%d\n",
            message->code,message->code==1?message->data.insert.units:0,
            message->code==1?message->data.insert.cursor:0,
            message->code==8?message->data.status:-1,
            self->service_shown,self->service_window_shown);
        fflush(stdout);
    }
    if(!self->external_consumer ||
       !self->external_consumer(self->external_data,message,result))
        anco_imf_editor_consume(message,header,result,&self->consumer);
}

void anco_linux_imf_runtime_set_external_consumer(AncoLinuxImfRuntime *self,
    AncoLinuxImfExternalConsumer consumer,void *data)
{
    if(!self||self->stopping)return;
    self->external_consumer=consumer;self->external_data=data;
    printf("RUNTIME_IMF_EXTERNAL_CONSUMER active=%d standard_linux_protocol=ibus\n",
        consumer!=NULL);
}

int anco_linux_imf_runtime_editor_key(AncoLinuxImfRuntime *self,
    int32_t android_key)
{
    if(!self || self->stopping || !self->enabled || !self->key_done)return -ENOENT;
    if(android_key!=66 && android_key!=67 && android_key!=112)return -EINVAL;
    AncoNativeInputKey key={
        .event_id=-1,.device_id=-1,.action_time_us=g_get_monotonic_time(),
        .android_key=android_key,.android_meta=0,.android_action=0};
    self->key_done(self->data,&key,false,false);
    key.event_id=-2;key.android_action=1;key.action_time_us=g_get_monotonic_time();
    self->key_done(self->data,&key,false,false);
    printf("RUNTIME_IMF_EDITOR_KEY android=%d raw_wayland_pair=1 original_current_connection=1\n",
        android_key);
    return 0;
}

static int function_key(void *data,int32_t action)
{
    (void)action;
    return anco_linux_imf_runtime_editor_key(data,66);
}

static int callback_absent(GBinderIpc *ipc)
{
    GBinderLocalRequest *request=gbinder_local_request_new(gbinder_ipc_io(ipc),
        gbinder_ipc_protocol(ipc),NULL);if(!request)return -ENOMEM;
    gbinder_local_request_append_int32(request,0x100);gbinder_local_request_append_int32(request,0);
    gbinder_local_request_append_string16(request,"ohos.samgr.accessToken");
    gbinder_local_request_append_int32(request,66875);int status=-1;
    GBinderRemoteReply *reply=gbinder_ipc_transact_sync_reply_main(ipc,0,2,request,&status);
    if(reply)gbinder_remote_reply_unref(reply);
    gbinder_local_request_unref(request);
    return status==7?0:status?status:-EADDRINUSE;
}

AncoLinuxImfRuntime *anco_linux_imf_runtime_new(GBinderIpc *ipc,AncoTextSeat *seat,
    AncoWaylandContext *context,AncoLinuxImfKeyDone done,void *data)
{
    if(!ipc||!seat||!context||!done||callback_absent(ipc))return NULL;
    AncoLinuxImfRuntime *self=g_new0(AncoLinuxImfRuntime,1);
    self->event_fd=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);self->key_done=done;self->data=data;
    self->commands=g_async_queue_new();self->results=g_async_queue_new();
    anco_ime_status_init(&self->status);
    self->service=(AncoImeServiceView){.data=self,.is_anco_service=true,
        .is_shown=service_is_shown,.hardware_keyboard=service_hardware_keyboard,
        .collaboration=service_collaboration,.request_show=service_request_show,
        .request_hide=service_request_hide,.notify_height=service_height,
        .on_window_shown=service_window_shown,.hide_ime_window=service_window_hidden};
    self->consumer=(AncoImfEditorConsumer){.seat=seat,.context=context,
        .status=&self->status,.service=&self->service,
        .function_key=function_key,.function_key_data=self};
    self->broker=anco_imf_broker_new(ipc);
    if(self->event_fd<0||!self->broker||anco_imf_broker_set_listener(self->broker,broker_died,self))goto fail;
    const AncoImfCallbackReceiverOps ops={.consume=consume};
    self->receiver=anco_imf_callback_receiver_new(ipc,&ops,self);if(!self->receiver)goto fail;
    AncoImfPublicationResult published;
    if(anco_imf_callback_receiver_publish(self->receiver,&published))goto fail;
    self->event_source=g_unix_fd_add(self->event_fd,G_IO_IN|G_IO_HUP|G_IO_ERR|G_IO_NVAL,
        results_ready,self);if(!self->event_source)goto fail;
    self->worker=g_thread_new("anco-original-imf",worker_main,self);if(!self->worker)goto fail;
    printf("RUNTIME_IMF_CALLBACK_PUBLISH attempted=%d transport=%d decoded=%d service=%d text_v2_v3=1\n",
        published.attempted,published.transport_status,published.reply_decoded,published.service_status);
    return self;
fail:
    anco_linux_imf_runtime_free(self);return NULL;
}

static void submit(AncoLinuxImfRuntime *self,Command *command)
{ if(self&&!self->stopping&&self->worker)g_async_queue_push(self->commands,command);else command_free(command); }
int anco_linux_imf_focus(AncoLinuxImfRuntime *self,bool show,int32_t pattern,int32_t enter,int32_t option)
{
    if(!self||self->stopping)return -EINVAL;
    /* The original AnCo coordinator keeps one IMF connection while the same
     * focused editor remains enabled. Qt text-input-v2 can commit RESET and
     * ENTER boundaries back-to-back for that editor; neither is a new focus.
     * Closing and reattaching here races the host hard-input session and clears
     * its composition state. A real editor/focus replacement reaches blur()
     * first through the shared text-seat focus path. */
    if(self->enabled){
        printf("RUNTIME_IMF_FOCUS_REUSED generation=%llu show=%d pattern=%d enter=%d option=%d\n",
            (unsigned long long)self->generation,show,pattern,enter,option);
        return 0;
    }
    self->enabled=true;self->selection_valid=false;
    ++self->generation;self->pair_count=0;
    Command *c=g_new0(Command,1);c->type=COMMAND_FOCUS;c->generation=self->generation;
    c->data.focus=(typeof(c->data.focus)){show,pattern,enter,option};submit(self,c);return 0;
}
void anco_linux_imf_blur(AncoLinuxImfRuntime *self)
{
    if(!self||self->stopping)return;
    self->enabled=false;self->selection_valid=false;
    self->service_shown=false;self->service_window_shown=false;
    ++self->generation;self->pair_count=0;
    Command *c=g_new0(Command,1);c->type=COMMAND_BLUR;c->generation=self->generation;submit(self,c);
}
bool anco_linux_imf_enabled(const AncoLinuxImfRuntime *self)
{return self&&self->enabled&&!self->stopping;}
bool anco_linux_imf_dispatch_key(AncoLinuxImfRuntime *self,const AncoNativeInputKey *key)
{
    if(!anco_linux_imf_enabled(self)||!key)return false;
    printf("RUNTIME_IMF_KEY_QUEUED generation=%llu event=%d action=%d key=%d device=%d\n",
        (unsigned long long)self->generation,key->event_id,key->android_action,
        key->android_key,key->device_id);
    Command *c=g_new0(Command,1);c->type=COMMAND_KEY;c->generation=self->generation;c->data.key=*key;submit(self,c);return true;
}
void anco_linux_imf_cursor(AncoLinuxImfRuntime *self,double x,double y,double height)
{
    if(!anco_linux_imf_enabled(self))return;
    Command *c=g_new0(Command,1);
    c->type=COMMAND_CURSOR;c->generation=self->generation;c->data.cursor=(typeof(c->data.cursor)){x,y,height};submit(self,c);
}
static int utf16_prefix_units(const char *text,int32_t bytes,int32_t *units)
{
    if(!text||!units||bytes<0)return -EINVAL;
    const size_t length=strlen(text);
    if(length>4000||(size_t)bytes>length||!g_utf8_validate(text,-1,NULL))return -EINVAL;
    const char *end=text+bytes,*p=text;gint64 count=0;
    while(p<end){
        const gunichar codepoint=g_utf8_get_char(p);
        const char *next=g_utf8_next_char(p);
        if(next>end)return -EINVAL;
        count+=codepoint>0xffff?2:1;p=next;
    }
    if(p!=end||count>G_MAXINT32)return -EINVAL;
    *units=(int32_t)count;return 0;
}
int anco_linux_imf_selection(AncoLinuxImfRuntime *self,const char *text,
    int32_t cursor,int32_t anchor)
{
    if(!anco_linux_imf_enabled(self)||!text)return -EINVAL;
    int32_t begin=0,end=0;
    /* Wayland's anchor is Android's selection start; its cursor is the active
     * selection end. Preserve direction instead of sorting the interval. */
    int error=utf16_prefix_units(text,anchor,&begin);
    if(!error)error=utf16_prefix_units(text,cursor,&end);
    if(error)return error;
    Command *c=g_new0(Command,1);c->type=COMMAND_SELECTION;
    c->generation=self->generation;c->data.selection.text=g_strdup(text);
    c->data.selection.old_begin=self->selection_valid?self->selection_begin:-1;
    c->data.selection.old_end=self->selection_valid?self->selection_end:-1;
    c->data.selection.new_begin=begin;c->data.selection.new_end=end;
    self->selection_valid=true;self->selection_begin=begin;self->selection_end=end;
    printf("RUNTIME_IMF_SELECTION_QUEUED generation=%llu old=%d,%d new=%d,%d utf8_bytes=%zu\n",
        (unsigned long long)self->generation,c->data.selection.old_begin,
        c->data.selection.old_end,begin,end,strlen(text));
    submit(self,c);return 0;
}
void anco_linux_imf_panel(AncoLinuxImfRuntime *self,bool show)
{
    if(!anco_linux_imf_enabled(self))return;
    Command *c=g_new0(Command,1);
    c->type=COMMAND_PANEL;c->generation=self->generation;c->data.panel_show=show;submit(self,c);
}
int anco_linux_imf_runtime_free(AncoLinuxImfRuntime *self)
{
    if(!self)return 0;
    self->stopping=true;self->enabled=false;++self->generation;
    if(self->event_source){g_source_remove(self->event_source);self->event_source=0;}
    if(self->worker){Command *c=g_new0(Command,1);c->type=COMMAND_STOP;
        g_async_queue_push(self->commands,c);g_thread_join(self->worker);self->worker=NULL;}
    Result *result;while(self->results&&(result=g_async_queue_try_pop(self->results)))g_free(result);
    Command *command;while(self->commands&&(command=g_async_queue_try_pop(self->commands)))command_free(command);
    int error=0;
    if(self->receiver){AncoImfPublicationResult withdrawn;
        error=anco_imf_callback_receiver_withdraw(self->receiver,&withdrawn);
        printf("RUNTIME_IMF_CALLBACK_WITHDRAW local=%d attempted=%d transport=%d decoded=%d service=%d\n",
            error,withdrawn.attempted,withdrawn.transport_status,withdrawn.reply_decoded,withdrawn.service_status);
        if(!error)error=anco_imf_callback_receiver_free(self->receiver);}
    if(self->broker){int e=anco_imf_broker_set_listener(self->broker,NULL,NULL);if(!error)error=e;
        e=anco_imf_broker_free(self->broker);if(!error)error=e;}
    if(error)return error;
    if(self->event_fd>=0)close(self->event_fd);
    if(self->commands)g_async_queue_unref(self->commands);
    if(self->results)g_async_queue_unref(self->results);
    g_free(self);return 0;
}
