#include "anco_imf_broker.h"
#include "gbinder_ipc.h"
#include "gbinder_local_request_p.h"
#include <errno.h>

struct AncoImfBroker {
    GBinderIpc *ipc;
    GBinderRemoteObject *remote;
    gulong death;
    GThread *thread;
    unsigned busy;
    AncoImfBrokerDied listener;
    void *data;
};
static void clear_remote(AncoImfBroker *b)
{
    GBinderRemoteObject *old=b->remote;
    gulong death=b->death;
    b->remote=NULL;b->death=0;
    if(old) {
        if(death)gbinder_remote_object_remove_handler(old,death);
        gbinder_remote_object_unref(old);
    }
}
static void remote_died(GBinderRemoteObject *object,void *data)
{
    AncoImfBroker *b=data;
    if(object!=b->remote)return;
    ++b->busy;
    clear_remote(b);
    if(b->listener)b->listener(b,b->data);
    --b->busy;
}
GBinderRemoteObject *anco_imf_broker_get(AncoImfBroker *b)
{
    if(!b || b->thread!=g_thread_self())return NULL;
    ++b->busy;
    if(!b->remote) {
        GBinderLocalRequest *req=gbinder_local_request_new(
            gbinder_ipc_io(b->ipc),gbinder_ipc_protocol(b->ipc),NULL);
        if(req) {
            gbinder_local_request_append_int32(req,0x100);
            gbinder_local_request_append_int32(req,0);
            gbinder_local_request_append_string16(req,"ohos.samgr.accessToken");
            gbinder_local_request_append_int32(req,66827);
            int status=-1;
            GBinderRemoteReply *reply=gbinder_ipc_transact_sync_reply_main(
                b->ipc,0,2,req,&status);
            GBinderRemoteObject *next=reply && !status?
                gbinder_remote_reply_read_object(reply):NULL;
            if(reply)gbinder_remote_reply_unref(reply);
            gbinder_local_request_unref(req);
            /* Original retains the acquired object even if linkToDeath fails.
             * It has no WMS-style isBinderAlive rejection here. */
            b->remote=next;
            if(next)b->death=gbinder_remote_object_add_death_handler(next,remote_died,b);
        }
    }
    GBinderRemoteObject *result=gbinder_remote_object_ref(b->remote);
    --b->busy;
    return result;
}
AncoImfBroker *anco_imf_broker_new(GBinderIpc *ipc)
{
    if(!ipc)return NULL;
    AncoImfBroker *b=g_new0(AncoImfBroker,1);
    b->ipc=gbinder_ipc_ref(ipc);b->thread=g_thread_self();
    GBinderRemoteObject *initial=anco_imf_broker_get(b);
    gbinder_remote_object_unref(initial);
    return b;
}
AncoImfClient *anco_imf_broker_client(AncoImfBroker *b)
{
    GBinderRemoteObject *remote=anco_imf_broker_get(b);
    AncoImfClient *client=anco_imf_client_from_remote(remote);
    gbinder_remote_object_unref(remote);
    return client;
}
int anco_imf_broker_set_listener(AncoImfBroker *b,AncoImfBrokerDied fn,void *data)
{
    if(!b)return -EINVAL;
    if(b->thread!=g_thread_self())return -EPERM;
    b->listener=fn;b->data=data;
    return 0;
}
int anco_imf_broker_free(AncoImfBroker *b)
{
    if(!b)return 0;
    if(b->thread!=g_thread_self())return -EPERM;
    if(b->busy)return -EBUSY;
    clear_remote(b);gbinder_ipc_unref(b->ipc);g_free(b);
    return 0;
}
