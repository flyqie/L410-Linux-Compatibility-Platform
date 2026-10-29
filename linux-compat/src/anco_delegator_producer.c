#include "anco_delegator_producer.h"
#include <errno.h>

struct AncoDelegatorProducer {
    guint refs;
    gboolean closed;
    AncoDelegatorProducerSnapshot state;
    AncoDelegatorPendingFrame *pending;
    AncoDelegatorReleaseState *records;
    AncoDelegatorReceiver *receiver;
    AncoGraphicTaskStartOperation task_start;
    AncoDelegatorPendingReplay last_replay;
    void (*client_changed)(void *);
    void *client_data;
};
static gboolean enter(AncoDelegatorProducer *self)
{if(!self||self->closed)return FALSE;++self->refs;return TRUE;}
static void leave(AncoDelegatorProducer *self)
{
    if(!--self->refs){
        self->closed=TRUE;
        anco_delegator_receiver_free(self->receiver);
        anco_delegator_release_state_free(self->records);
        anco_delegator_pending_frame_free(self->pending);g_free(self);
    }
}
AncoDelegatorProducer *anco_delegator_producer_ref(AncoDelegatorProducer *self)
{return enter(self)?self:NULL;}
void anco_delegator_producer_unref(AncoDelegatorProducer *self)
{if(self)leave(self);}
int anco_delegator_producer_clear_pending_handle(AncoDelegatorProducer *self)
{
    if(!enter(self))return -ESHUTDOWN;
    anco_delegator_pending_frame_clear_handle(self->pending);
    leave(self);return 0;
}
static int release(AncoDelegatorRelease *r,void *data)
{
    AncoDelegatorProducer *self=data;if(!enter(self))return -ESHUTDOWN;
    int status=anco_delegator_release_state_consume(self->records,r);leave(self);return status;
}
static void set_client(GBinderRemoteObject *peer,void *data)
{
    AncoDelegatorProducer *self=data;if(!enter(self))return;
    anco_delegator_pending_frame_replay(self->pending,&self->state.metadata,self->records,
        peer,self->state.features,self->state.task,&self->state.notify_pending,
        anco_graphic_task_start_callback,&self->task_start,&self->last_replay);
    if (!self->closed && self->client_changed) self->client_changed(self->client_data);
    leave(self);
}
static void mark_destroyed(void *data)
{AncoDelegatorProducer *self=data;if(self&&!self->closed)self->state.destroyed=TRUE;}
static void destroyed(gboolean clear_listener,void *data)
{
    AncoDelegatorProducer *self=data;if(!enter(self))return;
    anco_delegator_release_state_clear_pending(self->records,clear_listener);leave(self);
}
static void clear_client(void *data)
{AncoDelegatorProducer *self=data;anco_delegator_receiver_clear_client(self->receiver);}
AncoDelegatorProducer *anco_delegator_producer_new(GBinderIpc *ipc,guint32 features,gint32 task)
{
    if(!ipc)return NULL;
    AncoDelegatorProducer *self=g_new0(AncoDelegatorProducer,1);self->refs=1;
    self->state=(AncoDelegatorProducerSnapshot){.task=task,.features=features,.notify_pending=TRUE,
        .metadata=ANCO_DELEGATOR_METADATA_STATE_INIT};
    self->task_start.actual_ipc=ipc;self->pending=anco_delegator_pending_frame_new();
    self->records=anco_delegator_release_state_new(NULL,NULL);
    const AncoDelegatorReceiverOps ops={release,set_client,destroyed,mark_destroyed};
    self->receiver=anco_delegator_receiver_new(ipc,features,&ops,self);
    if(!self->receiver){leave(self);return NULL;}return self;
}
GBinderLocalObject *anco_delegator_producer_ref_object(AncoDelegatorProducer *self)
{return self&&!self->closed?anco_delegator_receiver_ref_object(self->receiver):NULL;}
GBinderRemoteObject *anco_delegator_producer_ref_client(AncoDelegatorProducer *self)
{return self&&!self->closed?anco_delegator_receiver_ref_client(self->receiver):NULL;}
int anco_delegator_producer_observe_client(AncoDelegatorProducer *self,
    void (*changed)(void *),void *data)
{
    if(!self)return -EINVAL;
    if(self->closed)return -ESHUTDOWN;
    self->client_changed=changed;self->client_data=data;return 0;
}
int anco_delegator_producer_snapshot(AncoDelegatorProducer *self,AncoDelegatorProducerSnapshot *out)
{if(!self||!out)return -EINVAL;if(self->closed)return -ESHUTDOWN;*out=self->state;return 0;}
int anco_delegator_producer_set_listener(AncoDelegatorProducer *self,
    AncoDelegatorBufferReleased listener,void *data)
{
    if(!enter(self))return -ESHUTDOWN;
    int status=anco_delegator_release_state_set_listener(self->records,listener,data,clear_client,self);
    leave(self);return status;
}
int anco_delegator_producer_set_shared(AncoDelegatorProducer *self,gboolean shared)
{if(!self)return -EINVAL;if(self->closed)return -ESHUTDOWN;self->state.shared=shared;return 0;}
static gboolean metadata_enter(AncoDelegatorProducer *self,AncoDelegatorMetadataUpdate *out)
{
    if(!out)return FALSE;
    if(enter(self))return TRUE;
    int status=self?-ESHUTDOWN:-EINVAL;
    *out=(AncoDelegatorMetadataUpdate){FALSE,status,{status,-1,FALSE,-1}};
    return FALSE;
}
void anco_delegator_producer_set_transform(AncoDelegatorProducer *self,
    guint32 value,AncoDelegatorMetadataUpdate *out)
{
    if(!metadata_enter(self,out))return;
    GBinderRemoteObject *peer=anco_delegator_receiver_ref_client(self->receiver);
    anco_delegator_metadata_state_transform(&self->state.metadata,peer,self->state.features,value,out);
    gbinder_remote_object_unref(peer);leave(self);
}
int anco_delegator_producer_set_queue_size(AncoDelegatorProducer *self,
    gint32 value,AncoDelegatorMetadataUpdate *out)
{
    if(!out)return -EINVAL;
    if(!metadata_enter(self,out))return out->status;
    GBinderRemoteObject *peer=anco_delegator_receiver_ref_client(self->receiver);
    int status=anco_delegator_metadata_state_queue_size(&self->state.metadata,peer,
        self->state.features,value,out);
    gbinder_remote_object_unref(peer);leave(self);return status;
}
void anco_delegator_producer_set_dataspace(AncoDelegatorProducer *self,
    guint32 value,AncoDelegatorMetadataUpdate *out)
{
    if(!metadata_enter(self,out))return;
    GBinderRemoteObject *peer=anco_delegator_receiver_ref_client(self->receiver);
    anco_delegator_metadata_state_dataspace(&self->state.metadata,peer,self->state.features,value,out);
    gbinder_remote_object_unref(peer);leave(self);
}
int anco_delegator_producer_dequeue(AncoDelegatorProducer *self,
    const AncoDelegatorDequeueCall *call,AncoDelegatorDequeueReport *out)
{
    if(!enter(self))return -ESHUTDOWN;
    GBinderRemoteObject *peer=anco_delegator_receiver_ref_client(self->receiver);
    int status=anco_delegator_normal_dequeue(self->pending,&self->state.metadata,self->records,
        self->state.shared,&self->state.active,peer,self->state.features,call,out);
    gbinder_remote_object_unref(peer);leave(self);return status;
}
int anco_delegator_producer_queue(AncoDelegatorProducer *self,
    const AncoDelegatorQueueCall *call,AncoDelegatorQueueReport *out)
{
    if(!enter(self))return -ESHUTDOWN;
    GBinderRemoteObject *peer=anco_delegator_receiver_ref_client(self->receiver);
    int status=anco_delegator_normal_queue(self->pending,self->records,self->state.shared,
        peer,self->state.features,call,self->state.task,&self->state.notify_pending,
        anco_graphic_task_start_callback,&self->task_start,out);
    gbinder_remote_object_unref(peer);leave(self);return status;
}
gboolean anco_delegator_producer_add_pending(AncoDelegatorProducer *self,
    const AncoDelegatorReleaseId *id,gint32 slot)
{return self&&!self->closed&&anco_delegator_release_state_add_pending(self->records,
    self->state.active,self->state.destroyed,id,slot);}
int anco_delegator_producer_take_fence(AncoDelegatorProducer *self,gint32 slot)
{return self&&!self->closed?anco_delegator_release_state_take_fence(self->records,slot):-1;}
gboolean anco_delegator_producer_registered(AncoDelegatorProducer *self,gint32 slot)
{return self&&!self->closed&&anco_delegator_release_state_registered(self->records,slot);}
gboolean anco_delegator_producer_conversion_failed(AncoDelegatorProducer *self,gint32 slot)
{return self&&!self->closed&&anco_delegator_release_state_conversion_failed(self->records,slot);}
static int clear(AncoDelegatorProducer *self,gint32 slot,gboolean all,AncoDelegatorResult *out)
{
    if(!out)return -EINVAL;
    if(!enter(self))return -ESHUTDOWN;
    *out=(AncoDelegatorResult){0,-1,FALSE,-1};
    if(self->state.shared){leave(self);return 0;}
    if(!all)anco_delegator_release_state_clear_conversion(self->records,slot);
    GBinderRemoteObject *peer=anco_delegator_receiver_ref_client(self->receiver);
    if(all)anco_delegator_clear_all_once(peer,self->state.features,out);
    else anco_delegator_clear_slot_once(peer,self->state.features,slot,out);
    int status=!peer?-1:out->local_error?out->local_error:out->transport;
    gbinder_remote_object_unref(peer);leave(self);return status;
}
int anco_delegator_producer_clear_slot(AncoDelegatorProducer *self,gint32 slot,AncoDelegatorResult *out)
{return clear(self,slot,FALSE,out);}
int anco_delegator_producer_clear_all(AncoDelegatorProducer *self,AncoDelegatorResult *out)
{return clear(self,0,TRUE,out);}
int anco_delegator_producer_reset(AncoDelegatorProducer *self)
{
    if(!enter(self))return -ESHUTDOWN;
    anco_delegator_release_state_reset_records(self->records);
    anco_delegator_release_state_reset_conversion(self->records);
    self->state.metadata=(AncoDelegatorMetadataState)ANCO_DELEGATOR_METADATA_STATE_INIT;
    self->state.active=FALSE;self->state.task=-1;
    self->state.notify_pending=TRUE;self->state.destroyed=FALSE;leave(self);return 0;
}
void anco_delegator_producer_free(AncoDelegatorProducer *self)
{
    if(self&&!self->closed){self->closed=TRUE;
        anco_delegator_receiver_free(self->receiver);self->receiver=NULL;leave(self);}
}
