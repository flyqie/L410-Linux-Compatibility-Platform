#define _GNU_SOURCE
#include "anco_delegator_pending_frame.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

struct AncoDelegatorPendingFrame {
    gboolean dequeue, queue;
    gint32 slot;
    int fence;
    guint8 *handle;
    gsize size;
};
static void close_handle(guint8 *handle)
{
    if (handle) {
        gint32 count;
        memcpy(&count, handle+4, 4);
        for (gint32 i=0; i<count; ++i) {
            gint32 fd;
            memcpy(&fd,handle+12+4u*i,4);
            if (fd>=0) close(fd); /* Linux owns actual copies, including0 */
        }
        g_free(handle);
    }
}
static int clone_handle(const void *input,gsize size,guint8 **out)
{
    *out=NULL;
    if (!input) return size ? -EINVAL : 0;
    gint32 header[3];
    if (size<sizeof(header)) return -EMSGSIZE;
    memcpy(header,input,sizeof(header));
    if ((guint32)header[1]>1024 || (guint32)header[2]>1024) return -EINVAL;
    if (size!=12u+4u*(header[1]+header[2])) return -EMSGSIZE;
    guint8 *copy=g_try_malloc(size);
    if (!copy) return -ENOMEM;
    memcpy(copy,input,size);
    memset(copy+12,0xff,4u*header[1]);
    for (gint32 i=0;i<header[1];++i) {
        gint32 fd;
        memcpy(&fd,(const guint8 *)input+12+4u*i,4);
        const int duplicate=fcntl(fd,F_DUPFD_CLOEXEC,0);
        memcpy(copy+12+4u*i,&duplicate,4);
        /* Original CloneHandle fails only for a positive input dup failure. */
        if (fd>=1 && duplicate<0) {
            const int error=-errno;
            close_handle(copy);
            return error;
        }
    }
    *out=copy;
    return 0;
}
AncoDelegatorPendingFrame *anco_delegator_pending_frame_new(void)
{
    AncoDelegatorPendingFrame *self=g_new0(AncoDelegatorPendingFrame,1);
    self->slot=-1;self->fence=-1;
    return self;
}
void anco_delegator_pending_frame_clear_handle(AncoDelegatorPendingFrame *self)
{
    if (self) {
        close_handle(self->handle);
        self->handle=NULL;
        self->size=0;
    }
}
int anco_delegator_pending_frame_cache_dequeue(AncoDelegatorPendingFrame *self,
    gint32 slot,const void *handle,gsize size)
{
    if (!self) return -EINVAL;
    self->dequeue=TRUE;self->queue=FALSE;self->slot=slot;
    close_handle(self->handle);self->handle=NULL;self->size=0;
    int status=clone_handle(handle,size,&self->handle);
    if (self->handle) self->size=size;
    return status;
}
int anco_delegator_pending_frame_cache_queue_owned(AncoDelegatorPendingFrame *self,
    int fence)
{
    if (!self) return -EINVAL; /* caller still owns FD */
    if (self->fence>=0 && self->fence!=fence) close(self->fence);
    self->queue=TRUE;self->fence=fence;
    return 0;
}
int anco_delegator_normal_dequeue(AncoDelegatorPendingFrame *pending,
    AncoDelegatorMetadataState *metadata,AncoDelegatorReleaseState *records,
    gboolean shared,gboolean *active,GBinderRemoteObject *peer,guint32 features,
    const AncoDelegatorDequeueCall *input,AncoDelegatorDequeueReport *out)
{
    if (!pending||!metadata||!records||!active||!input||!out) return -EINVAL;
    const AncoDelegatorDequeueCall call=*input;
    memset(out,0,sizeof(*out));out->wire=(AncoDelegatorResult){0,-1,FALSE,-1};
    if(shared){out->shared_skipped=TRUE;return 0;}
    if(!peer){
        out->pending_cached=TRUE;
        out->cache_error=anco_delegator_pending_frame_cache_dequeue(pending,
            call.slot,call.handle,call.handle_bytes);
        return 0;
    }
    *active=TRUE;
    anco_delegator_metadata_state_replay(metadata,peer,features,&out->metadata);
    out->request_flag=anco_delegator_release_state_registered(records,call.slot)?call.request_flag:1;
    gboolean sent=anco_delegator_dequeue_flag_once(peer,features,out->request_flag,
        call.slot,call.handle,call.handle_bytes,&out->wire);
    if(sent)anco_delegator_release_state_dequeued_reply(records,call.slot,
        out->request_flag&&out->wire.reply_decoded&&out->wire.reply_value==10);
    return out->wire.local_error?out->wire.local_error:out->wire.transport;
}
int anco_delegator_normal_queue(AncoDelegatorPendingFrame *pending,
    AncoDelegatorReleaseState *records,gboolean shared,GBinderRemoteObject *peer,
    guint32 features,const AncoDelegatorQueueCall *input,gint32 task,
    gboolean *notify_pending,int (*notify)(gint32,void *),void *notify_data,
    AncoDelegatorQueueReport *out)
{
    if(!pending||!records||!input||!notify_pending||!out)return -EINVAL;
    const AncoDelegatorQueueCall call=*input;
    memset(out,0,sizeof(*out));out->wire=(AncoDelegatorResult){0,-1,FALSE,-1};
    if(shared||!peer){
        if(!peer){
            anco_delegator_pending_frame_cache_queue_owned(pending,call.acquire_fence_fd);
            out->pending_cached=TRUE;
        }
        AncoDelegatorRelease release=ANCO_DELEGATOR_RELEASE_INIT;
        release.slot=call.slot;out->release_called=TRUE;
        out->release_status=anco_delegator_release_state_consume(records,&release);
        return out->release_status;
    }
    AncoDelegatorTaskNotify notice={task,notify_pending,notify,notify_data,FALSE,0};
    anco_delegator_queue_notify_once(peer,features,call.slot,call.rect,
        call.acquire_fence_fd,FALSE,&notice,&out->fence_dup_error,&out->wire);
    out->notify_attempted=notice.attempted;out->notify_status=notice.status;
    return out->wire.local_error?out->wire.local_error:out->wire.transport;
}

int anco_delegator_pending_frame_replay(AncoDelegatorPendingFrame *self,
    AncoDelegatorMetadataState *metadata,AncoDelegatorReleaseState *records,
    GBinderRemoteObject *peer,guint32 features,gint32 task,gboolean *notify_pending,
    int (*notify)(gint32,void *),void *notify_data,AncoDelegatorPendingReplay *out)
{
    if (!self || !metadata || !records || !notify_pending || !out) return -EINVAL;
    if (peer && self->dequeue && self->queue && self->handle &&
        (guint32)self->slot<64 && *notify_pending && task!=-1 && !notify)
        return -EINVAL; /* no partial send with a missing required real operation */
    memset(out,0,sizeof(*out));
    out->dequeue=out->queue=(AncoDelegatorResult){0,-1,FALSE,-1};
    anco_delegator_metadata_state_replay(metadata,peer,features,&out->metadata);
    /* Original UpdateLastBuffer moves/clears fields before every send. */
    const AncoDelegatorPendingFrame frame=*self;
    *self=(AncoDelegatorPendingFrame){.slot=-1,.fence=-1};
    if (frame.dequeue && frame.handle && (guint32)frame.slot<64) {
        out->dequeue_attempted=TRUE;
        gboolean sent=anco_delegator_dequeue_once(peer,features,frame.slot,
            frame.handle,frame.size,&out->dequeue);
        /* Clone is retired regardless transport success, before queue. */
        close_handle(frame.handle);
        if (sent) {
            anco_delegator_release_state_dequeued_reply(records,frame.slot,
                out->dequeue.reply_decoded&&out->dequeue.reply_value==10);
            if (frame.queue) {
                AncoDelegatorTaskNotify notice={task,notify_pending,notify,notify_data,FALSE,0};
                out->queue_attempted=TRUE;
                anco_delegator_queue_notify_once(peer,features,frame.slot,NULL,
                    frame.fence,TRUE,&notice,&out->fence_dup_error,&out->queue);
                out->notify_attempted=notice.attempted;out->notify_status=notice.status;
            }
        }
    } else close_handle(frame.handle);
    if (frame.fence>=0) close(frame.fence);
    return 0; /* original OnSetClient does not propagate replay send status */
}
void anco_delegator_pending_frame_free(AncoDelegatorPendingFrame *self)
{
    if (self) {
        close_handle(self->handle);
        if (self->fence>=0) close(self->fence);
        g_free(self);
    }
}
