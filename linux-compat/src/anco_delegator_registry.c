#include "anco_delegator_registry.h"
#include <errno.h>
typedef struct {
    AncoDelegatorSurfaceKey key;
    guint32 count;
    AncoDelegatorProducer *producer;
} Record;
struct AncoDelegatorRegistry {
    GBinderIpc *ipc;
    guint32 features;
    GHashTable *records;
    AncoGraphicVersionController *version;
};
static guint key_hash(gconstpointer value)
{
    const AncoDelegatorSurfaceKey *k=value;
    const guint64 h=k->second^k->first;
    return (guint)(h^(h>>32));
}
static gboolean key_equal(gconstpointer a,gconstpointer b)
{
    const AncoDelegatorSurfaceKey *x=a,*y=b;
    return x->first==y->first&&x->second==y->second;
}
static void record_free(gpointer value)
{
    Record *r=value;
    anco_delegator_producer_unref(r->producer);g_free(r);
}
AncoDelegatorRegistry *anco_delegator_registry_new(GBinderIpc *ipc,guint32 features)
{
    if(!ipc)return NULL;
    AncoDelegatorRegistry *self=g_new0(AncoDelegatorRegistry,1);
    self->ipc=ipc;self->features=features;
    self->records=g_hash_table_new_full(key_hash,key_equal,NULL,record_free);
    return self;
}
AncoDelegatorRegistry *anco_delegator_registry_new_original(GBinderIpc *ipc,
    AncoDelegatorRegistryInit *out)
{
    if(!out)return NULL;
    *out=(AncoDelegatorRegistryInit){0};
    if(!ipc){out->check_status=-EINVAL;return NULL;}
    AncoDelegatorRegistry *self=anco_delegator_registry_new(ipc,0);
    self->version=anco_graphic_version_controller_new(ipc);
    out->check_status=anco_graphic_version_controller_check(self->version,&out->check);
    self->features=out->check.state.features;
    return self;
}
int anco_delegator_registry_add(AncoDelegatorRegistry *self,
    AncoDelegatorSurfaceKey key,gint32 task,AncoDelegatorProducer **out,guint32 *count)
{
    if(!self||!out||!count)return -EINVAL;
    Record *r=g_hash_table_lookup(self->records,&key);
    AncoDelegatorProducer *owned;
    if(r){
        owned=anco_delegator_producer_ref(r->producer);
        if(!owned)return -ESHUTDOWN;
        ++r->count;
    }else{
        AncoDelegatorProducer *p=anco_delegator_producer_new(self->ipc,self->features,task);
        if(!p)return -ENOMEM;
        r=g_new0(Record,1);r->key=key;r->count=1;r->producer=p;
        owned=anco_delegator_producer_ref(p);
        g_hash_table_insert(self->records,&r->key,r);
    }
    AncoDelegatorProducer *old=*out;
    *out=owned;*count=r->count;
    anco_delegator_producer_unref(old);
    return 0;
}
int anco_delegator_registry_remove(AncoDelegatorRegistry *self,
    AncoDelegatorSurfaceKey key,AncoDelegatorRemoveRecord *out)
{
    if(!self||!out)return -EINVAL;
    Record *r=g_hash_table_lookup(self->records,&key);
    *out=(AncoDelegatorRemoveRecord){FALSE,0};
    if(!r)return 0;
    int status=anco_delegator_producer_clear_pending_handle(r->producer);
    if(status)return status;
    out->found=TRUE;out->remaining=--r->count;
    if(!r->count)g_hash_table_remove(self->records,&key);
    return 0;
}
void anco_delegator_registry_free(AncoDelegatorRegistry *self)
{
    if(self){
        anco_graphic_version_controller_free(self->version);
        g_hash_table_destroy(self->records);g_free(self);
    }
}
int anco_delegator_registry_register(AncoDelegatorRegistry *self,
    AncoDelegatorSurfaceKey key,gint32 task,const char *buffershared,
    const char *name,AncoDelegatorBufferReleased listener,void *data,
    AncoDelegatorProducer **binding,AncoDelegatorRegistration *out)
{
    if(!self||!buffershared||!name||!listener||!binding||!out)return -EINVAL;
    *out=(AncoDelegatorRegistration){0};
    out->lookup=(AncoGraphicLookup){0,-1,FALSE};
    out->create=(AncoGraphicResult){0,-1,FALSE,-1};
    out->clear=(AncoDelegatorResult){0,-1,FALSE,-1};
    if(key.first==G_MAXUINT32||(buffershared[0]!='1'&&(guint32)key.second==0)){
        out->skipped=TRUE;return 0;
    }
    AncoDelegatorProducer *p=NULL;
    int status=anco_delegator_registry_add(self,key,task,&p,&out->count);
    if(status)return status;
    /* Original signed32 b.ge, not an unsigned comparison after overflow. */
    out->reused=out->count>=2&&out->count<=G_MAXINT32;
    if(out->reused){
        out->reset_status=anco_delegator_producer_reset(p);
        if(out->reset_status){status=out->reset_status;goto done;}
        out->clear_status=anco_delegator_producer_clear_all(p,&out->clear);
        /* Original caller ignores ClearAll's return. */
    }else{
        GBinderRemoteObject *graphic=anco_graphic_lookup_once(self->ipc,&out->lookup);
        if(!graphic){status=out->lookup.local_error?out->lookup.local_error:-ENOTCONN;goto done;}
        GBinderLocalObject *object=anco_delegator_producer_ref_object(p);
        if(!object){gbinder_remote_object_unref(graphic);status=-ESHUTDOWN;goto done;}
        out->create_attempted=TRUE;
        anco_graphic_create_surface_node_once(graphic,key.second,(gint32)key.first,name,object,&out->create);
        gbinder_local_object_unref(object);gbinder_remote_object_unref(graphic);
        if(out->create.local_error){status=out->create.local_error;goto done;}
        out->original_create_return=out->create.decoded?out->create.service_result:0;
        if(out->original_create_return)goto done;
    }
    status=anco_delegator_producer_set_listener(p,listener,data);
    if(!status){
        AncoDelegatorProducer *old=*binding;
        *binding=anco_delegator_producer_ref(p);
        anco_delegator_producer_unref(old);
        out->binding_changed=TRUE;
    }
done:
    anco_delegator_producer_unref(p);return status;
}
int anco_delegator_registry_unregister(AncoDelegatorRegistry *self,
    AncoDelegatorSurfaceKey key,AncoDelegatorProducer *binding,
    AncoDelegatorUnregistration *out)
{
    if(!self || !out)return -EINVAL;
    *out=(AncoDelegatorUnregistration){0};
    out->lookup=(AncoGraphicLookup){0,-1,FALSE};
    out->remove=(AncoGraphicRemoveResult){{0,-1,FALSE,-1},0};
    int status=anco_delegator_registry_remove(self,key,&out->record);
    if(status)return status;
    AncoDelegatorProducer *p=anco_delegator_producer_ref(binding);
    if(binding && !p)return -ESHUTDOWN;
    if(!out->record.remaining && p){
        out->listener_status=anco_delegator_producer_set_listener(p,NULL,NULL);
        if(out->listener_status){status=out->listener_status;goto finish;}
        out->listener_cleared=TRUE;out->lookup_attempted=TRUE;
        GBinderRemoteObject *graphic=anco_graphic_lookup_once(self->ipc,&out->lookup);
        if(!graphic){status=out->lookup.local_error?out->lookup.local_error:-ENOTCONN;goto finish;}
        out->remove_attempted=TRUE;
        anco_graphic_remove_surface_node_once(graphic,key.second,(gint32)key.first,&out->remove);
        gbinder_remote_object_unref(graphic);
        status=out->remove.wire.local_error;
    }
finish:
    anco_delegator_producer_unref(p);return status;
}
