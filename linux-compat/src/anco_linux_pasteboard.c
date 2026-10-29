#include "anco_linux_pasteboard.h"
#include "anco_paste_encode_v362.h"
#include "anco_paste_text_v361.h"
#include "gbinder_ipc.h"
#include "gbinder_local_request_p.h"
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/types/wlr_data_device.h>

#define PROXY_SA 66871
#define HOST_SA 66823
#define PROXY_IFACE "com.android.server.pasteboard.ancoproxy.IAncoProxyPasteboardInterface"
#define HOST_IFACE "OHOS.Anco.Service.IBrokerPasteboardService"
#define PASTEBOARD_E_OK 0x01a80000
#define MAX_PASTE_BYTES (4u * 1024u * 1024u)

typedef struct IncomingRead IncomingRead;
typedef struct HostTextSource HostTextSource;

struct AncoLinuxPasteboard {
    GBinderIpc *ipc;
    struct wl_display *display;
    struct wlr_seat *seat;
    GBinderLocalObject *object;
    struct wl_listener request_selection;
    IncomingRead *incoming;
    HostTextSource *host_source;
    GBytes *last_forwarded;
    GThread *owner;
    gboolean publication_retained;
    guint busy;
};

struct IncomingRead {
    AncoLinuxPasteboard *bridge;
    int fd;
    guint watch;
    GByteArray *bytes;
    gboolean overflow;
};

struct HostTextSource {
    struct wlr_data_source source;
    AncoLinuxPasteboard *bridge;
    GBytes *bytes;
};

typedef struct {
    GBytes *bytes;
    int fd;
    size_t offset;
} PendingWrite;

static GBinderLocalRequest *samgr_request(GBinderIpc *ipc)
{
    GBinderLocalRequest *request=gbinder_local_request_new(
        gbinder_ipc_io(ipc),gbinder_ipc_protocol(ipc),NULL);
    if(request){
        gbinder_local_request_append_int32(request,0x100);
        gbinder_local_request_append_int32(request,0);
        gbinder_local_request_append_string16(request,"ohos.samgr.accessToken");
    }
    return request;
}

static GBinderRemoteObject *lookup(GBinderIpc *ipc,guint32 sa,int *transport_out)
{
    GBinderLocalRequest *request=samgr_request(ipc);
    if(!request){if(transport_out)*transport_out=-ENOMEM;return NULL;}
    gbinder_local_request_append_int32(request,sa);
    int transport=-1;
    GBinderRemoteReply *reply=gbinder_ipc_transact_sync_reply_main(
        ipc,0,2,request,&transport);
    GBinderRemoteObject *object=reply&&!transport?
        gbinder_remote_reply_read_object(reply):NULL;
    if(reply)gbinder_remote_reply_unref(reply);
    gbinder_local_request_unref(request);
    if(transport_out)*transport_out=transport;
    return object;
}

static int forward_host(AncoLinuxPasteboard *self,gboolean clear,
    const guint8 *bytes,gsize length)
{
    int lookup_transport=-1;
    GBinderRemoteObject *object=lookup(self->ipc,HOST_SA,&lookup_transport);
    if(!object){
        printf("RUNTIME_PASTEBOARD_HOST_LOOKUP transport=%d resolved=0\n",lookup_transport);
        return 0;
    }
    GBinderClient *client=gbinder_client_new(object,HOST_IFACE);
    GBinderLocalRequest *request=client?gbinder_client_new_request(client):NULL;
    GByteArray *encoded=clear?NULL:anco_encode_plain_text(bytes,length);
    if(!request||(!clear&&!encoded)){
        if(request)gbinder_local_request_unref(request);
        if(encoded)g_byte_array_unref(encoded);
        if(client)gbinder_client_unref(client);
        gbinder_remote_object_unref(object);
        return 0;
    }
    if(!clear){
        gbinder_local_request_append_int32(request,1);
        GBinderWriter writer;
        gbinder_local_request_init_writer(request,&writer);
        gbinder_writer_append_byte_array(&writer,encoded->data,(gint32)encoded->len);
        if(self->last_forwarded)g_bytes_unref(self->last_forwarded);
        self->last_forwarded=g_bytes_new(bytes,length);
    }
    int transport=-1;
    GBinderRemoteReply *reply=gbinder_client_transact_sync_reply(client,
        clear?1:2,request,&transport);
    gint32 result=-1;
    const gboolean decoded=reply&&gbinder_remote_reply_read_int32(reply,&result);
    const gboolean okay=!transport&&decoded&&result==PASTEBOARD_E_OK;
    if((!okay||clear)&&self->last_forwarded){
        g_bytes_unref(self->last_forwarded);self->last_forwarded=NULL;
    }
    printf("RUNTIME_PASTEBOARD_HOST_CALL code=%d transport=%d decoded=%d result=%d\n",
        clear?1:2,transport,decoded,result);
    if(reply)gbinder_remote_reply_unref(reply);
    gbinder_local_request_unref(request);
    if(encoded)g_byte_array_unref(encoded);
    gbinder_client_unref(client);
    gbinder_remote_object_unref(object);
    return okay;
}

static void cancel_incoming(AncoLinuxPasteboard *self)
{
    IncomingRead *read=self->incoming;
    if(!read)return;
    self->incoming=NULL;
    if(read->watch)g_source_remove(read->watch);
    close(read->fd);
    g_byte_array_unref(read->bytes);
    g_free(read);
}

static gboolean incoming_ready(gint fd,GIOCondition condition,gpointer data)
{
    IncomingRead *incoming=data;
    guint8 buffer[8192];
    if(condition&(G_IO_IN|G_IO_HUP|G_IO_ERR|G_IO_NVAL)){
        for(;;){
            ssize_t count=incoming->overflow?0:read(fd,buffer,sizeof(buffer));
            if(count>0){
                if(incoming->bytes->len+(gsize)count>MAX_PASTE_BYTES){
                    incoming->overflow=TRUE;break;
                }
                g_byte_array_append(incoming->bytes,buffer,(guint)count);continue;
            }
            if(count<0&&errno==EINTR)continue;
            if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return G_SOURCE_CONTINUE;
            if(count==0&&!incoming->overflow&&
                g_utf8_validate((const char *)incoming->bytes->data,incoming->bytes->len,NULL)){
                const int accepted=forward_host(incoming->bridge,FALSE,
                    incoming->bytes->data,incoming->bytes->len);
                printf("RUNTIME_PASTEBOARD_LINUX_TO_HOST bytes=%u accepted=%d\n",
                    incoming->bytes->len,accepted);
            }else{
                printf("RUNTIME_PASTEBOARD_LINUX_REJECT overflow=%d bytes=%u\n",
                    incoming->overflow,incoming->bytes->len);
            }
            break;
        }
    }
    incoming->bridge->incoming=NULL;
    close(incoming->fd);
    g_byte_array_unref(incoming->bytes);
    g_free(incoming);
    return G_SOURCE_REMOVE;
}

static const char *plain_mime(struct wlr_data_source *source)
{
    const char **mime;
    const char *plain=NULL;
    if(!source)return NULL;
    wl_array_for_each(mime,&source->mime_types){
        if(!strcmp(*mime,"text/plain;charset=utf-8"))return *mime;
        if(!strcmp(*mime,"text/plain"))plain=*mime;
    }
    return plain;
}

static gboolean receive_linux_selection(AncoLinuxPasteboard *self,
    struct wlr_data_source *source)
{
    const char *mime=plain_mime(source);
    if(!mime)return FALSE;
    int fds[2];
    if(pipe(fds))return FALSE;
    int flags=fcntl(fds[0],F_GETFL);
    if(flags<0||fcntl(fds[0],F_SETFL,flags|O_NONBLOCK)<0){
        close(fds[0]);close(fds[1]);return FALSE;
    }
    IncomingRead *read=g_new0(IncomingRead,1);
    read->bridge=self;read->fd=fds[0];read->bytes=g_byte_array_new();
    read->watch=g_unix_fd_add(fds[0],G_IO_IN|G_IO_HUP|G_IO_ERR|G_IO_NVAL,
        incoming_ready,read);
    if(!read->watch){
        close(fds[0]);close(fds[1]);g_byte_array_unref(read->bytes);g_free(read);
        return FALSE;
    }
    self->incoming=read;
    /* The wlroots data-source implementation owns and closes the write fd. */
    wlr_data_source_send(source,mime,fds[1]);
    return TRUE;
}

static void selection_requested(struct wl_listener *listener,void *data)
{
    AncoLinuxPasteboard *self=wl_container_of(listener,self,request_selection);
    struct wlr_seat_request_set_selection_event *event=data;
    cancel_incoming(self);
    wlr_seat_set_selection(self->seat,event->source,event->serial);
    if(!event->source){
        const int accepted=forward_host(self,TRUE,NULL,0);
        printf("RUNTIME_PASTEBOARD_LINUX_CLEAR accepted=%d\n",accepted);
    }else if(!receive_linux_selection(self,event->source)){
        /* An unsupported Linux selection must not leave stale host text. */
        const int accepted=forward_host(self,TRUE,NULL,0);
        printf("RUNTIME_PASTEBOARD_LINUX_UNSUPPORTED host_cleared=%d\n",accepted);
    }
}

static gboolean write_ready(gint fd,GIOCondition condition,gpointer data)
{
    PendingWrite *pending=data;
    if(!(condition&(G_IO_HUP|G_IO_ERR|G_IO_NVAL))){
        gsize size=0;
        const guint8 *bytes=g_bytes_get_data(pending->bytes,&size);
        while(pending->offset<size){
            ssize_t count=write(fd,bytes+pending->offset,size-pending->offset);
            if(count>0){pending->offset+=(size_t)count;continue;}
            if(count<0&&errno==EINTR)continue;
            if(count<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return G_SOURCE_CONTINUE;
            break;
        }
    }
    close(pending->fd);g_bytes_unref(pending->bytes);g_free(pending);
    return G_SOURCE_REMOVE;
}

static void host_source_send(struct wlr_data_source *source,const char *mime,int32_t fd)
{
    HostTextSource *host=wl_container_of(source,host,source);
    if(strcmp(mime,"text/plain")&&strcmp(mime,"text/plain;charset=utf-8")){
        close(fd);return;
    }
    int flags=fcntl(fd,F_GETFL);
    if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0){close(fd);return;}
    PendingWrite *write=g_new0(PendingWrite,1);
    write->bytes=g_bytes_ref(host->bytes);write->fd=fd;
    if(!g_unix_fd_add(fd,G_IO_OUT|G_IO_HUP|G_IO_ERR|G_IO_NVAL,write_ready,write)){
        close(fd);g_bytes_unref(write->bytes);g_free(write);
    }
}

static void host_source_destroy(struct wlr_data_source *source)
{
    HostTextSource *host=wl_container_of(source,host,source);
    if(host->bridge&&host->bridge->host_source==host)host->bridge->host_source=NULL;
    g_bytes_unref(host->bytes);g_free(host);
}

static const struct wlr_data_source_impl host_source_impl={
    .send=host_source_send,.destroy=host_source_destroy
};

static gboolean add_mime(struct wlr_data_source *source,const char *mime)
{
    char **slot=wl_array_add(&source->mime_types,sizeof(*slot));
    if(!slot)return FALSE;
    *slot=strdup(mime);
    if(!*slot){source->mime_types.size-=sizeof(*slot);return FALSE;}
    return TRUE;
}

static gboolean set_host_text(AncoLinuxPasteboard *self,const guint8 *bytes,gsize length)
{
    HostTextSource *host=g_new0(HostTextSource,1);
    host->bridge=self;host->bytes=g_bytes_new(bytes,length);
    wlr_data_source_init(&host->source,&host_source_impl);
    if(!add_mime(&host->source,"text/plain;charset=utf-8")||
       !add_mime(&host->source,"text/plain")){
        wlr_data_source_destroy(&host->source);return FALSE;
    }
    wlr_seat_set_selection(self->seat,&host->source,
        wl_display_next_serial(self->display));
    self->host_source=host;
    return TRUE;
}

static GBinderLocalReply *on_transaction(GBinderLocalObject *object,
    GBinderRemoteRequest *request,guint code,guint flags,int *status,void *data)
{
    (void)flags;
    AncoLinuxPasteboard *self=data;
    const char *iface=gbinder_remote_request_interface(request);
    const uid_t uid=gbinder_remote_request_sender_euid(request);
    if((uid!=0&&uid!=5557)||!iface||strcmp(iface,PROXY_IFACE)||
       (code!=1&&code!=2&&code!=3)){
        *status=-1;return NULL;
    }
    ++self->busy;
    GBinderReader reader;
    gbinder_remote_request_init_reader(request,&reader);
    gboolean valid=FALSE;
    if(code==1){
        valid=gbinder_reader_at_end(&reader);
        if(valid)wlr_seat_set_selection(self->seat,NULL,
            wl_display_next_serial(self->display));
    }else if(code==3){
        gint32 user=-1;
        valid=gbinder_reader_read_int32(&reader,&user)&&
            gbinder_reader_at_end(&reader)&&user==0;
        if(valid){
            wlr_seat_set_selection(self->seat,NULL,
                wl_display_next_serial(self->display));
            printf("RUNTIME_PASTEBOARD_REVERSE_CLEAR_BY_USER user=%d\n",user);
        }
    }else{
        gint32 present=0;gsize length=0;const guint8 *bytes=NULL;
        struct paste_span text={0};
        if(gbinder_reader_read_int32(&reader,&present)&&present==1)
            bytes=gbinder_reader_read_byte_array(&reader,&length);
        valid=bytes&&length&&length<=MAX_PASTE_BYTES&&
            gbinder_reader_at_end(&reader)&&anco_paste_text(bytes,length,&text);
        if(valid&&self->last_forwarded){
            gsize prior_length=0;
            const guint8 *prior=g_bytes_get_data(self->last_forwarded,&prior_length);
            if(prior_length==text.size&&!memcmp(prior,text.data,text.size)){
                printf("RUNTIME_PASTEBOARD_REVERSE_ECHO_SUPPRESSED bytes=%zu\n",text.size);
                goto reply;
            }
        }
        if(valid)valid=set_host_text(self,text.data,text.size);
        if(valid&&self->last_forwarded){
            g_bytes_unref(self->last_forwarded);self->last_forwarded=NULL;
        }
        printf("RUNTIME_PASTEBOARD_REVERSE_SET text_accepted=%d bytes=%zu\n",
            valid,valid?text.size:(size_t)0);
    }
reply:
    if(code==1&&valid)puts("RUNTIME_PASTEBOARD_REVERSE_CLEAR");
    if(!valid)puts("RUNTIME_PASTEBOARD_REVERSE_INVALID");
    GBinderLocalReply *reply=gbinder_local_object_new_reply(object);
    if(reply)gbinder_local_reply_append_int32(reply,valid?0:(guint32)-1);
    *status=reply?0:-ENOMEM;
    --self->busy;
    return reply;
}

static int change_sa(AncoLinuxPasteboard *self,gboolean add,
    gboolean *attempted,int *transport_out,gboolean *decoded_out,gint32 *service_out)
{
    *attempted=FALSE;*transport_out=-1;*decoded_out=FALSE;*service_out=-1;
    GBinderLocalRequest *request=samgr_request(self->ipc);
    if(!request)return -ENOMEM;
    gbinder_local_request_append_int32(request,PROXY_SA);
    if(add){
        gbinder_local_request_append_local_object(request,self->object);
        gbinder_local_request_append_int32(request,0);
        gbinder_local_request_append_int32(request,0);
        gbinder_local_request_append_string16(request,"");
        gbinder_local_request_append_string16(request,"");
    }
    *attempted=TRUE;
    GBinderRemoteReply *reply=gbinder_ipc_transact_sync_reply_main(
        self->ipc,0,add?3:4,request,transport_out);
    *decoded_out=reply&&gbinder_remote_reply_read_int32(reply,service_out);
    if(reply)gbinder_remote_reply_unref(reply);
    gbinder_local_request_unref(request);
    return *transport_out?*transport_out:*decoded_out?*service_out:-1;
}

AncoLinuxPasteboard *anco_linux_pasteboard_new(GBinderIpc *ipc,
    struct wl_display *display,struct wlr_seat *seat)
{
    if(!ipc||!display||!seat)return NULL;
    int lookup_transport=-1;
    GBinderRemoteObject *existing=lookup(ipc,PROXY_SA,&lookup_transport);
    if(existing){gbinder_remote_object_unref(existing);return NULL;}
    if(lookup_transport!=7)return NULL;
    AncoLinuxPasteboard *self=g_new0(AncoLinuxPasteboard,1);
    self->ipc=ipc;self->display=display;self->seat=seat;self->owner=g_thread_self();
    const char *ifaces[]={PROXY_IFACE,NULL};
    self->object=gbinder_local_object_new(ipc,ifaces,on_transaction,self);
    if(!self->object){g_free(self);return NULL;}
    self->request_selection.notify=selection_requested;
    wl_signal_add(&seat->events.request_set_selection,&self->request_selection);
    gboolean attempted=FALSE,decoded=FALSE;int transport=-1;gint32 service=-1;
    const int error=change_sa(self,TRUE,&attempted,&transport,&decoded,&service);
    self->publication_retained=attempted&&(!transport&&!decoded?TRUE:decoded&&!service);
    printf("RUNTIME_PASTEBOARD_PUBLISH local=%d attempted=%d transport=%d decoded=%d service=%d selection_consumer=1\n",
        error,attempted,transport,decoded,service);
    if(error){
        wl_list_remove(&self->request_selection.link);
        if(!self->publication_retained){gbinder_local_object_drop(self->object);g_free(self);}
        return NULL;
    }
    return self;
}

int anco_linux_pasteboard_free(AncoLinuxPasteboard *self)
{
    if(!self)return 0;
    if(self->owner!=g_thread_self())return -EPERM;
    if(self->busy)return -EBUSY;
    cancel_incoming(self);
    wl_list_remove(&self->request_selection.link);
    if(self->host_source&&self->seat->selection_source==&self->host_source->source)
        wlr_seat_set_selection(self->seat,NULL,wl_display_next_serial(self->display));
    if(self->last_forwarded){g_bytes_unref(self->last_forwarded);self->last_forwarded=NULL;}
    int error=0;
    if(self->publication_retained){
        gboolean attempted=FALSE,decoded=FALSE;int transport=-1;gint32 service=-1;
        error=change_sa(self,FALSE,&attempted,&transport,&decoded,&service);
        printf("RUNTIME_PASTEBOARD_WITHDRAW local=%d attempted=%d transport=%d decoded=%d service=%d\n",
            error,attempted,transport,decoded,service);
        if(!error)self->publication_retained=FALSE;
    }
    if(error)return error;
    gbinder_local_object_drop(self->object);g_free(self);
    return 0;
}
