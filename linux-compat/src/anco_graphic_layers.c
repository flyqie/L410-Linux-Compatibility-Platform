#define _GNU_SOURCE
#include "anco_graphic_layers.h"
#include <errno.h>
#include <endian.h>
#include <limits.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

_Static_assert(__BYTE_ORDER == __LITTLE_ENDIAN, "paired original Parcel endian");
_Static_assert(sizeof(float) == 4, "original float32");

static void put(unsigned char **cursor, const void *value, size_t size)
{ memcpy(*cursor,value,size); *cursor += size; }
static void u32(unsigned char **cursor,uint32_t value) { put(cursor,&value,4); }
static void i32(unsigned char **cursor,int32_t value) { put(cursor,&value,4); }
static void u64(unsigned char **cursor,uint64_t value) { put(cursor,&value,8); }

static uint32_t smart(float value)
{
    /* Original single-precision FADD of0x34000000, FCVTZS, signed clamp.
     * Preserve its NaN/saturating conversion without C out-of-range casts. */
    volatile float sum=value+0x1p-23f;
    int32_t converted;
    if(isnan(sum))converted=0;
    else if(sum>=2147483648.0f)converted=INT32_MAX;
    else if(sum<=-2147483648.0f)converted=INT32_MIN;
    else converted=(int32_t)sum;
    if(converted>32766)converted=32766;
    if(converted< -32767)converted= -32767;
    return (uint32_t)converted & 0xffffU;
}

static int handle_info(const AncoGraphicLayer *layer,int32_t extras[2])
{
    extras[0]=extras[1]=0;
    if(!layer->handle)return layer->handle_bytes? -EINVAL:1;
    if(layer->handle_bytes<12)return -EMSGSIZE;
    int32_t header[3];memcpy(header,layer->handle,sizeof(header));
    if(header[0]!=12||header[1]<0||header[2]<0)return -EINVAL;
    const uint64_t needed=12+4*((uint64_t)header[1]+(uint64_t)header[2]);
    /* Writer reads the genuine native_handle extensions at0x88/0x8c. */
    if(needed!=layer->handle_bytes||needed<0x90)return -EMSGSIZE;
    for(int32_t i=0;i<header[1];++i){
        int32_t fd;struct stat status;
        memcpy(&fd,(const unsigned char *)layer->handle+12+4*(size_t)i,4);
        if(fstat(fd,&status))return 0; /* original skip, no invented FD */
    }
    memcpy(extras,(const unsigned char *)layer->handle+0x88,8);
    return 1;
}

int anco_graphic_layers_encode(const AncoGraphicLayer *const *layers,
    size_t count,void *packet,size_t capacity,size_t *bytes,uint32_t *accepted)
{
    if(!bytes||!accepted)return -EINVAL;
    *bytes=0;*accepted=0;
    if((count&&!layers)||!packet)return -EINVAL;
    if(capacity<4)return -EMSGSIZE;
    unsigned char *start=packet,*cursor=start;
    uint32_t written=0;u32(&cursor,0);
    for(size_t i=0;i<count;++i){
        const AncoGraphicLayer *layer=layers[i];
        if(!layer)continue;
        int32_t extras[2];const int usable=handle_info(layer,extras);
        if(usable<0)return usable;
        if(!usable)continue;
        if(written==UINT32_MAX)return -EOVERFLOW;
        if(capacity-(size_t)(cursor-start)<ANCO_GRAPHIC_LAYER_BYTES)return -EMSGSIZE;
        u64(&cursor,UINT64_C(0xffffffe0)+(layer->opaque!=0));
        u64(&cursor,layer->surface_view);
        u32(&cursor,layer->window);
        u64(&cursor,(uint64_t)-(int64_t)layer->snapshot_byte);
        u32(&cursor,layer->z);put(&cursor,&layer->scale,4);
        u32(&cursor,(smart(layer->smart_x)<<16)|smart(layer->smart_y));
        i32(&cursor,layer->transform);i32(&cursor,layer->dataspace);
        u32(&cursor,layer->composition==3?(layer->rgba&255U):19U);
        u32(&cursor,layer->composition==3?((layer->rgba>>8)&255U):layer->display_transform);
        u32(&cursor,(layer->rgba>>16)&255U);u32(&cursor,layer->rgba>>24);
        put(&cursor,layer->source_crop,16);put(&cursor,layer->display_frame,16);
        put(&cursor,extras,8);u32(&cursor,layer->handle?1:2);
        ++written;
    }
    memcpy(start,&written,4);*bytes=(size_t)(cursor-start);*accepted=written;
    return 0;
}

struct AncoGraphicLayerStream { int fd; };
static int64_t now_ms(void)
{
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now))return -1;
    return (int64_t)now.tv_sec*1000+now.tv_nsec/1000000;
}
static int send_fully(int fd,const void *data,size_t size,int64_t deadline)
{
    const unsigned char *cursor=data;
    while(size){
        const int64_t now=now_ms();if(now<0)return -errno;
        const int64_t left=deadline-now;if(left<=0)return -ETIMEDOUT;
        struct pollfd p={.fd=fd,.events=POLLOUT};
        int ready=poll(&p,1,left>INT_MAX?INT_MAX:(int)left);
        if(ready<0){if(errno==EINTR)continue;return -errno;}
        if(!ready)return -ETIMEDOUT;
        const ssize_t sent=send(fd,cursor,size,MSG_NOSIGNAL|MSG_DONTWAIT);
        if(sent<0){if(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK)continue;return -errno;}
        if(!sent)return -ECONNRESET;
        cursor+=sent;size-=(size_t)sent;
    }
    return 0;
}
int anco_graphic_layers_open(const char *path,int timeout_ms,AncoGraphicLayerStream **out)
{
    if(!out||*out||!path||!*path||timeout_ms<=0||
        strlen(path)>=sizeof(((struct sockaddr_un *)0)->sun_path))return -EINVAL;
    const int64_t start=now_ms();if(start<0)return -errno;
    int fd=socket(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);
    if(fd<0)return -errno;
    const int64_t deadline=start+timeout_ms;
    struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,path);
    int result=0;
    if(connect(fd,(struct sockaddr *)&address,offsetof(struct sockaddr_un,sun_path)+strlen(path)+1)){
        if(errno!=EINPROGRESS){result=-errno;goto done;}
        struct pollfd p={.fd=fd,.events=POLLOUT};
        for(;;){
            const int64_t now=now_ms();if(now<0){result=-errno;goto done;}
            const int64_t left=deadline-now;if(left<=0){result=-ETIMEDOUT;goto done;}
            const int ready=poll(&p,1,left>INT_MAX?INT_MAX:(int)left);
            if(ready<0&&errno==EINTR)continue;
            if(ready<=0){result=ready? -errno:-ETIMEDOUT;goto done;}
            int error=0;socklen_t size=sizeof(error);
            if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size)){result=-errno;goto done;}
            if(error){result=-error;goto done;}
            break;
        }
    }
    const unsigned char greeting[10]={'f','d',0,0,0,0,0,0,0,0};
    result=send_fully(fd,greeting,sizeof(greeting),deadline);
    if(!result){
        AncoGraphicLayerStream *stream=malloc(sizeof(*stream));
        if(!stream){result=-ENOMEM;goto done;}
        stream->fd=fd;*out=stream;return 0;
    }
done:
    close(fd);return result;
}
int anco_graphic_layers_send(AncoGraphicLayerStream *stream,
    const AncoGraphicLayer *const *scene,size_t count,int timeout_ms,uint32_t *accepted)
{
    if(!accepted)return -EINVAL;
    *accepted=0;
    if(!stream||stream->fd<0||timeout_ms<=0)return -EINVAL;
    unsigned char packet[ANCO_GRAPHIC_LAYER_PACKET_LIMIT];
    size_t bytes=0;uint32_t number=0;
    int result=anco_graphic_layers_encode(scene,count,packet,sizeof(packet),&bytes,&number);
    if(result)return result;
    if(number<1||number>100)return -ERANGE;
    const int64_t now=now_ms();if(now<0)return -errno;
    const uint32_t length=(uint32_t)bytes;
    result=send_fully(stream->fd,&length,sizeof(length),now+timeout_ms);
    if(!result)result=send_fully(stream->fd,packet,bytes,now+timeout_ms);
    if(result){close(stream->fd);stream->fd=-1;return result;}
    *accepted=number;return 0; /* transport only: no host ack or buffer release */
}
void anco_graphic_layers_close(AncoGraphicLayerStream *stream)
{ if(stream){if(stream->fd>=0)close(stream->fd);free(stream);} }
