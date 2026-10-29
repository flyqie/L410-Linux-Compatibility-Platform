#include "anco_graphic_transport.h"
#include "gbinder_ipc.h"
#include "gbinder_local_request_p.h"
#include <errno.h>
#include <string.h>

int anco_graphic_append_alpha(GBinderLocalRequest *request,
                              gfloat alpha, gint32 window_id)
{
    if (!request) return -EINVAL;
    gint32 bits;
    G_STATIC_ASSERT(sizeof(alpha) == sizeof(bits));
    memcpy(&bits, &alpha, sizeof(bits));
    GBinderWriter writer;
    gbinder_local_request_init_writer(request, &writer);
    gbinder_writer_append_int32(&writer, bits);
    gbinder_writer_append_int32(&writer, window_id);
    return 0;
}

gboolean anco_graphic_set_alpha_with_window_id(GBinderRemoteObject *graphic,
    gfloat alpha, gint32 window_id, AncoGraphicResult *out)
{
    if (!out) return FALSE;
    *out = (AncoGraphicResult){0, -1, FALSE, -1};
    if (!graphic) { out->local_error = -ENOTCONN; return FALSE; }
    GBinderClient *client = gbinder_client_new(graphic, ANCO_GRAPHIC_IFACE);
    GBinderLocalRequest *request = client ? gbinder_client_new_request(client) : NULL;
    if (!request) {
        out->local_error = -ENOMEM;
    } else {
        out->local_error = anco_graphic_append_alpha(request, alpha, window_id);
        if (!out->local_error) {
            GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(client,
                ANCO_GRAPHIC_SET_ALPHA_WITH_WINDOW_ID, request, &out->transport);
            if (reply) {
                GBinderReader reader;
                gbinder_remote_reply_init_reader(reply, &reader);
                out->decoded = gbinder_reader_read_int32(&reader, &out->service_result);
            }
            if (reply) gbinder_remote_reply_unref(reply);
        }
        gbinder_local_request_unref(request);
    }
    if (client) gbinder_client_unref(client);
    return !out->local_error && !out->transport && out->decoded;
}

int anco_graphic_exchange_version(GBinderRemoteObject *graphic,
    AncoGraphicVersionState *state,AncoGraphicVersionExchange *out)
{
    if(!state||!out)return -EINVAL;
    *out=(AncoGraphicVersionExchange){0};
    if(!graphic)return -ENOTCONN;
    for(guint i=0;i<2;++i){
        AncoGraphicResult *wire=&out->wire[i];
        anco_graphic_set_alpha_with_window_id(graphic,-999.0f,115,wire);
        if(wire->local_error)return wire->local_error;
        ++out->attempts;
        const gint32 value=wire->decoded?wire->service_result:0;
        out->original_return[i]=value;
        if(value==0||value==5||value==8){
            state->token_supported=0;
            if(i==0&&value==0)continue;
            return 0;
        }
        if((guint32)value&2)state->token_supported=1;
        /* No bit2: original leaves the previous token byte unchanged. */
        state->features=(guint32)value;
        out->original_result=TRUE;
        return 0;
    }
    return 0;
}

gboolean anco_graphic_notify_task_started_once(GBinderRemoteObject *graphic,
    gint32 task, AncoGraphicTaskStartedResult *out)
{
    if (!out) return FALSE;
    *out=(AncoGraphicTaskStartedResult){{0,-1,FALSE,-1},0};
    if (!graphic) {out->wire.local_error=-ENOTCONN;return FALSE;}
    GBinderClient *client=gbinder_client_new(graphic,ANCO_GRAPHIC_IFACE);
    GBinderLocalRequest *request=client?gbinder_client_new_request(client):NULL;
    if (!request) out->wire.local_error=-ENOMEM;
    else {
        GBinderWriter writer;
        gbinder_local_request_init_writer(request,&writer);
        gbinder_writer_append_int32(&writer,task);
        GBinderRemoteReply *reply=gbinder_client_transact_sync_reply(client,
            ANCO_GRAPHIC_NOTIFY_TASK_STARTED,request,&out->wire.transport);
        /* ORIGINAL proxy ignores transact's return, then calls readInt32. */
        if (reply) {
            GBinderReader reader;
            gbinder_remote_reply_init_reader(reply,&reader);
            out->wire.decoded=gbinder_reader_read_int32(&reader,&out->wire.service_result);
            if (out->wire.decoded) out->original_return=out->wire.service_result;
            gbinder_remote_reply_unref(reply);
        }
        gbinder_local_request_unref(request);
    }
    if (client) gbinder_client_unref(client);
    return !out->wire.local_error&&!out->wire.transport&&out->wire.decoded;
}

int anco_graphic_task_started_callback(gint32 task,void *data)
{
    AncoGraphicTaskStartedOperation *operation=data;
    if (!operation) return -EINVAL;
    anco_graphic_notify_task_started_once(operation->actual_graphic,task,&operation->last);
    return operation->last.wire.local_error ? operation->last.wire.local_error :
        operation->last.original_return;
}

GBinderRemoteObject *anco_graphic_lookup_once(GBinderIpc *ipc,AncoGraphicLookup *out)
{
    if (!out) return NULL;
    *out=(AncoGraphicLookup){0,-1,FALSE};
    if (!ipc) {out->local_error=-ENOTCONN;return NULL;}
    GBinderLocalRequest *request=gbinder_local_request_new(
        gbinder_ipc_io(ipc),gbinder_ipc_protocol(ipc),NULL);
    if (!request) {out->local_error=-ENOMEM;return NULL;}
    /* Guest get-SA and host CheckSystemAbility use code2/flags0. Linux talks
     * to original HOST SAMgr; do not substitute guest ISystemAbilityManager. */
    gbinder_local_request_append_int32(request,0x100);
    gbinder_local_request_append_int32(request,0);
    gbinder_local_request_append_string16(request,"ohos.samgr.accessToken");
    gbinder_local_request_append_int32(request,ANCO_GRAPHIC_SA);
    GBinderRemoteReply *reply=gbinder_ipc_transact_sync_reply_main(
        ipc,0,2,request,&out->transport);
    GBinderRemoteObject *graphic=reply&&!out->transport?
        gbinder_remote_reply_read_object(reply):NULL;
    if (reply) gbinder_remote_reply_unref(reply);
    gbinder_local_request_unref(request);
    out->resolved=graphic!=NULL;
    return graphic;
}
gboolean anco_graphic_task_start_once(GBinderIpc *ipc,gint32 task,
    AncoGraphicTaskStartResult *out)
{
    if (!out) return FALSE;
    *out=(AncoGraphicTaskStartResult){0,-1,FALSE,FALSE,{{0,-1,FALSE,-1},0}};
    AncoGraphicLookup lookup;
    GBinderRemoteObject *graphic=anco_graphic_lookup_once(ipc,&lookup);
    out->lookup_local_error=lookup.local_error;
    out->lookup_transport=lookup.transport;out->resolved=lookup.resolved;
    if (!graphic) return FALSE;
    out->notification_attempted=TRUE;
    const gboolean delivered=anco_graphic_notify_task_started_once(graphic,
        task,&out->notification);
    /* Original local sp/proxy lifetime, not WMS's cache/aliveness policy. */
    gbinder_remote_object_unref(graphic);
    return delivered;
}

struct AncoGraphicVersionController {
    GBinderIpc *ipc;
    AncoGraphicVersionState state;
    GBinderRemoteObject *watched;
    gulong handler;
};
static void version_drop_watch(AncoGraphicVersionController *self)
{
    GBinderRemoteObject *object=self->watched;
    gulong handler=self->handler;
    self->watched=NULL;self->handler=0;
    if(handler)gbinder_remote_object_remove_handler(object,handler);
    gbinder_remote_object_unref(object);
}
static void version_broker_died(GBinderRemoteObject *object,void *data)
{
    AncoGraphicVersionController *self=data;
    if(self->watched==object)version_drop_watch(self);
    /* Original callback does not reset the token byte or feature bits. */
}
AncoGraphicVersionController *anco_graphic_version_controller_new(GBinderIpc *ipc)
{
    if(!ipc)return NULL;
    AncoGraphicVersionController *self=g_new0(AncoGraphicVersionController,1);
    self->ipc=ipc;
    self->state=(AncoGraphicVersionState)ANCO_GRAPHIC_VERSION_STATE_INIT;
    return self;
}
int anco_graphic_version_controller_check(AncoGraphicVersionController *self,
    AncoGraphicVersionCheck *out)
{
    if(!self||!out)return -EINVAL;
    *out=(AncoGraphicVersionCheck){0};out->lookup.transport=-1;
    int status=0;
    if(self->state.token_supported!=255){
        out->cached=TRUE;out->original_result=self->state.token_supported!=0;
    }else{
        GBinderRemoteObject *object=anco_graphic_lookup_once(self->ipc,&out->lookup);
        if(!object){status=out->lookup.local_error?out->lookup.local_error:-ENOTCONN;goto done;}
        status=anco_graphic_exchange_version(object,&self->state,&out->exchange);
        out->original_result=out->exchange.original_result;
        if(!status&&self->state.token_supported!=255&&!self->watched){
            self->handler=gbinder_remote_object_add_death_handler(object,version_broker_died,self);
            if(self->handler)self->watched=gbinder_remote_object_ref(object);
            else status=-ENOMEM;
        }
        gbinder_remote_object_unref(object);
    }
done:
    out->state=self->state;out->watch_installed=self->handler!=0;
    return status;
}
void anco_graphic_version_controller_free(AncoGraphicVersionController *self)
{
    if(self){version_drop_watch(self);g_free(self);}
}

int anco_graphic_task_start_callback(gint32 task,void *data)
{
    AncoGraphicTaskStartOperation *operation=data;
    if (!operation) return -EINVAL;
    anco_graphic_task_start_once(operation->actual_ipc,task,&operation->last);
    if (operation->last.lookup_local_error)
        return operation->last.lookup_local_error;
    if (!operation->last.resolved) return -ENOTCONN;
    return operation->last.notification.wire.local_error ?
        operation->last.notification.wire.local_error :
        operation->last.notification.original_return;
}
