#define _GNU_SOURCE
#define WLR_USE_UNSTABLE
#include "anco_shm_queue.h"
#include "anco_gralloc1_device.h"
#include "anco_native_handle_legacy.h"
#include "wayland_egl_backend.h"
#include <wlr/types/wlr_buffer.h>
#include <drm_fourcc.h>
#include <glib-unix.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/sync_file.h>
#include <arm_neon.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    AncoShmQueue *queue;
    gint32 slot;
    buffer_handle_t handle;
    guint32 width,height,stride;
    AncoDelegatorReleaseId id;
    gboolean in_flight,failed;
    int fence;
    guint fence_source;
} ShmSlot;

/* Match the original asynchronous BufferQueue limit: one acquired buffer,
 * one dequeued buffer and one async replacement.  The wire slot namespace is
 * 64 entries wide, but it is not permission to keep 64 gralloc allocations
 * simultaneously in flight. */
enum { ANCO_SHM_MAX_IN_FLIGHT = 3 };

struct AncoShmQueue {
    AncoDelegatorProducer *producer;
    AncoGralloc1Device *device;
    const AncoGralloc1Api *api;
    GRALLOC1_PFN_GET_BACKING_STORE backing;
    ShmSlot slots[64]; /* ORIGINAL host slot limit, allocate on actual demand */
    guint count;
    gboolean retiring;
    AncoShmSubmission submission;
    _Alignas(8) guint8 submission_handle[0x170];
    guint8 *staging;
    size_t staging_capacity;
    void (*changed)(void *);
    void *data;
};

/* wl_shm ARGB/XRGB8888 is BGRA/BGRX in little-endian memory, while the
 * original gralloc allocation is HAL_PIXEL_FORMAT_RGBA_8888. AArch64 always
 * provides Advanced SIMD, so preserve the exact byte permutation four pixels
 * at a time. The scalar tail keeps arbitrary Wayland widths valid. */
static void copy_bgra_to_rgba(guint8 *restrict destination,size_t destination_stride,
    const guint8 *restrict source,size_t source_stride,int width,int height,
    gboolean source_is_xrgb)
{
    static const guint8 indices_bytes[16] __attribute__((aligned(16)))={
        2,1,0,3, 6,5,4,7, 10,9,8,11, 14,13,12,15};
    const uint8x16_t indices=vld1q_u8(indices_bytes);
    const uint8x16_t opaque_alpha=vreinterpretq_u8_u32(vdupq_n_u32(0xff000000U));
    for(int y=0;y<height;++y){
        const guint8 *in=source+(size_t)y*source_stride;
        guint8 *out=destination+(size_t)y*destination_stride;
        int x=0;
        for(;x+4<=width;x+=4){
            uint8x16_t pixels=vqtbl1q_u8(vld1q_u8(in+(size_t)x*4),indices);
            if(source_is_xrgb)pixels=vorrq_u8(pixels,opaque_alpha);
            vst1q_u8(out+(size_t)x*4,pixels);
        }
        for(;x<width;++x){
            out[4*x]=in[4*x+2];
            out[4*x+1]=in[4*x+1];
            out[4*x+2]=in[4*x];
            out[4*x+3]=source_is_xrgb?255:in[4*x+3];
        }
    }
}

static void diagnostic_dump_pixels(const char *directory,const char *kind,
    const ShmSlot *slot,guint32 sequence,const void *pixels,
    guint32 width,guint32 height,size_t stride)
{
    if(!directory||!directory[0]||!kind||!slot||!pixels)return;
    char path[512];
    const int length=snprintf(path,sizeof(path),
        "%s/%s-ring%u.rgba",directory,kind,sequence&3u);
    if(length<0||(size_t)length>=sizeof(path))return;
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);
    if(fd<0){printf("RUNTIME_SHM_DIAGNOSTIC_DUMP seq=%u status=%d\n",sequence,-errno);return;}
    const guint8 *cursor=pixels;
    size_t remaining=stride*(size_t)height;
    int status=0;
    while(remaining){
        ssize_t written=write(fd,cursor,remaining);
        if(written<0){if(errno==EINTR)continue;status=-errno;break;}
        if(!written){status=-EIO;break;}
        cursor+=(size_t)written;remaining-=(size_t)written;
    }
    if(close(fd)&&!status)status=-errno;
    printf("RUNTIME_SHM_DIAGNOSTIC_DUMP kind=%s seq=%u ring=%u slot=%d width=%u height=%u stride=%zu bytes=%zu status=%d\n",
        kind,sequence,sequence&3u,slot->slot,width,height,stride,
        stride*(size_t)height,status);
}

typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;
typedef struct {
    void *library;
    cairo_surface_t *(*image_surface_create_for_data)(unsigned char *,int,int,int,int);
    cairo_t *(*create)(cairo_surface_t *);
    void (*destroy)(cairo_t *);
    void (*surface_destroy)(cairo_surface_t *);
    void (*set_source_rgb)(cairo_t *,double,double,double);
    void (*paint)(cairo_t *);
    void (*move_to)(cairo_t *,double,double);
    void (*line_to)(cairo_t *,double,double);
    void (*set_line_width)(cairo_t *,double);
    void (*stroke)(cairo_t *);
    void (*select_font_face)(cairo_t *,const char *,int,int);
    void (*set_font_size)(cairo_t *,double);
    void (*show_text)(cairo_t *,const char *);
    gboolean attempted,ready;
} CairoApi;

static CairoApi cairo_api;

static gboolean load_cairo(void)
{
    if(cairo_api.attempted)return cairo_api.ready;
    cairo_api.attempted=TRUE;
    cairo_api.library=dlopen("libcairo.so.2",RTLD_NOW|RTLD_LOCAL);
    if(!cairo_api.library)return FALSE;
#define CAIRO_SYM(field,name) do { \
    void *raw=dlsym(cairo_api.library,name); \
    memcpy(&cairo_api.field,&raw,sizeof(raw)); \
    if(!cairo_api.field)return FALSE; \
} while(0)
    CAIRO_SYM(image_surface_create_for_data,"cairo_image_surface_create_for_data");
    CAIRO_SYM(create,"cairo_create");
    CAIRO_SYM(destroy,"cairo_destroy");
    CAIRO_SYM(surface_destroy,"cairo_surface_destroy");
    CAIRO_SYM(set_source_rgb,"cairo_set_source_rgb");
    CAIRO_SYM(paint,"cairo_paint");
    CAIRO_SYM(move_to,"cairo_move_to");
    CAIRO_SYM(line_to,"cairo_line_to");
    CAIRO_SYM(set_line_width,"cairo_set_line_width");
    CAIRO_SYM(stroke,"cairo_stroke");
    CAIRO_SYM(select_font_face,"cairo_select_font_face");
    CAIRO_SYM(set_font_size,"cairo_set_font_size");
    CAIRO_SYM(show_text,"cairo_show_text");
#undef CAIRO_SYM
    cairo_api.ready=TRUE;
    return TRUE;
}

/* Cairo ARGB32 is native-endian BGRA on this little-endian target. The frame
 * uses neutral greys, so the byte order is also valid RGBA for gralloc. Client
 * pixels are copied separately with the explicit BGRA->RGBA conversion. */
static void draw_server_decoration(guint8 *destination,size_t stride,
    guint32 width,guint32 height,const AncoShmDecoration *decoration)
{
    for(guint32 y=0;y<height;++y){
        guint8 *row=destination+(size_t)y*stride;
        for(guint32 x=0;x<width;++x){
            row[4*x]=0x38;row[4*x+1]=0x38;row[4*x+2]=0x38;row[4*x+3]=0xff;
        }
    }
    if(!load_cairo()){
        for(guint32 y=1;y+1<decoration->top&&y<height;++y){
            guint8 *row=destination+(size_t)y*stride;
            for(guint32 x=1;x+1<width;++x)
                row[4*x]=row[4*x+1]=row[4*x+2]=0x45;
        }
        return;
    }
    cairo_surface_t *surface=cairo_api.image_surface_create_for_data(destination,0,
        (int)width,(int)height,(int)stride);
    if(!surface)return;
    cairo_t *cr=cairo_api.create(surface);
    if(!cr){cairo_api.surface_destroy(surface);return;}
    cairo_api.set_source_rgb(cr,0.22,0.22,0.24);cairo_api.paint(cr);
    if(decoration->top>2){
        cairo_api.set_source_rgb(cr,0.30,0.30,0.33);
        cairo_api.move_to(cr,1,1);cairo_api.line_to(cr,width-1,1);
        cairo_api.line_to(cr,width-1,decoration->top-1);
        cairo_api.line_to(cr,1,decoration->top-1);cairo_api.line_to(cr,1,1);
        cairo_api.set_line_width(cr,2);cairo_api.stroke(cr);
        const double font=MAX(11.0,MIN(18.0,(double)decoration->top*0.42));
        cairo_api.select_font_face(cr,"sans",0,0);cairo_api.set_font_size(cr,font);
        cairo_api.set_source_rgb(cr,0.94,0.94,0.94);
        cairo_api.move_to(cr,MAX(8.0,(double)decoration->left+8.0),
            ((double)decoration->top+font)*0.53);
        cairo_api.show_text(cr,decoration->title&&decoration->title[0]?
            decoration->title:"Linux");
        const double box=MAX(16.0,(double)decoration->top*0.50);
        const double cx=(double)width-MAX(8.0,(double)decoration->right)-box*0.65;
        const double cy=(double)decoration->top*0.5;
        cairo_api.set_line_width(cr,MAX(1.5,(double)decoration->top/18.0));
        cairo_api.move_to(cr,cx-box*0.25,cy-box*0.25);
        cairo_api.line_to(cr,cx+box*0.25,cy+box*0.25);
        cairo_api.move_to(cr,cx+box*0.25,cy-box*0.25);
        cairo_api.line_to(cr,cx-box*0.25,cy+box*0.25);cairo_api.stroke(cr);
    }
    cairo_api.destroy(cr);cairo_api.surface_destroy(surface);
}

static void changed(void *data)
{
    AncoShmQueue *self=data;
    if(!self->retiring&&self->changed)self->changed(self->data);
}
static gboolean fence_ready(gint fd,GIOCondition condition,gpointer data)
{
    ShmSlot *slot=data;
    struct sync_file_info info={0};
    int rc=ioctl(fd,SYNC_IOC_FILE_INFO,&info);
    /* POLLIN is an actual sync_file event; also require the kernel status.
     * Error/HUP/unsupported fd never authorizes rewriting the allocation. */
    if(!rc&&info.status==0&&!(condition&(G_IO_ERR|G_IO_HUP|G_IO_NVAL)))return G_SOURCE_CONTINUE;
    slot->fence_source=0;close(slot->fence);slot->fence=-1;
    slot->failed=rc||info.status!=1;
    if(!slot->failed)slot->in_flight=FALSE;
    printf("RUNTIME_SHM_FENCE slot=%d ioctl=%d status=%d reusable=%d\n",
        slot->slot,rc,info.status,!slot->failed);
    if(!slot->failed)changed(slot->queue);
    return G_SOURCE_REMOVE;
}
static void released(const AncoDelegatorReleaseId *id,int fd,void *data)
{
    AncoShmQueue *self=data;
    ShmSlot *slot=NULL;
    for(guint i=0;i<self->count;++i)
        if(self->slots[i].in_flight&&self->slots[i].id.first==id->first&&
            self->slots[i].id.second==id->second){slot=&self->slots[i];break;}
    AncoDelegatorProducerSnapshot state;
    if(!slot||self->retiring||anco_delegator_producer_snapshot(self->producer,&state)||
        state.destroyed){if(fd>=0)close(fd);return;}
    printf("RUNTIME_SHM_RELEASE slot=%d allocation=%llu commit=%llu fence=%d\n",
        slot->slot,(unsigned long long)id->first,(unsigned long long)id->second,fd);
    if(slot->fence_source||slot->fence>=0){if(fd>=0)close(fd);return;}
    if(fd<0){slot->in_flight=FALSE;changed(self);return;}
    slot->fence=fd;
    slot->fence_source=g_unix_fd_add(fd,G_IO_IN|G_IO_ERR|G_IO_HUP|G_IO_NVAL,fence_ready,slot);
}
AncoShmQueue *anco_shm_queue_new(AncoDelegatorProducer *producer,
    void (*notify)(void *),void *data)
{
    AncoShmQueue *self=g_new0(AncoShmQueue,1);
    self->producer=anco_delegator_producer_ref(producer);
    if(!self->producer){g_free(self);return NULL;}
    self->changed=notify;self->data=data;
    for(guint i=0;i<G_N_ELEMENTS(self->slots);++i){
        self->slots[i].queue=self;self->slots[i].slot=(gint32)i;self->slots[i].fence=-1;
    }
    anco_delegator_producer_set_listener(producer,released,self);
    anco_delegator_producer_observe_client(producer,changed,self);
    return self;
}
static int open_device(AncoShmQueue *self)
{
    if(self->device)return 0;
    const char *path=g_getenv("ANCO_GRALLOC_MODULE");
    if(!path||!path[0])return -ENODATA;
    /* Standard resident libhybris ABI loader, no project hooks or Android
     * framework/bootstrap. All loaded code remains resident until exit. */
    void *hybris=dlopen("libhybris-common.so.1",RTLD_NOW|RTLD_GLOBAL);
    if(!hybris){printf("RUNTIME_SHM_LOAD_HYBRIS %s\n",dlerror());return -ELIBACC;}
    void *(*load)(const char *,int)=dlsym(hybris,"android_dlopen");
    void *(*symbol)(void *,const char *)=dlsym(hybris,"android_dlsym");
    if(!load||!symbol)return -ELIBBAD;
    void *library=load(path,RTLD_NOW);
    if(!library)return -ELIBACC;
    const hw_module_t *module=symbol(library,"HMI");
    AncoGralloc1OpenReport report;
    int rc=anco_gralloc1_device_open(module,&self->device,&report);
    printf("RUNTIME_SHM_GRALLOC local=%d open=%d missing=%d\n",rc,report.open_status,report.missing_function);
    if(rc)return rc;
    self->api=anco_gralloc1_device_api(self->device);
#define RESOLVE(field,ID) do { \
    gralloc1_function_pointer_t raw=self->api->device->getFunction(self->api->device,GRALLOC1_FUNCTION_##ID); \
    memcpy(&self->field,&raw,sizeof(raw));if(!self->field){ \
        anco_gralloc1_device_close(self->device);self->device=NULL;self->api=NULL;return -ENOTSUP;} \
} while(0)
    RESOLVE(backing,GET_BACKING_STORE);
#undef RESOLVE
    return 0;
}
static int allocate(AncoShmQueue *self,ShmSlot *slot,guint32 w,guint32 h)
{
    const AncoGralloc1Api *api=self->api;
    gralloc1_buffer_descriptor_t descriptor=0;
    int rc=api->create(api->device,&descriptor);
    if(rc)return -EREMOTEIO;
    buffer_handle_t handle=NULL;
    if(!(rc=api->dimensions(api->device,descriptor,w,h))&&
       !(rc=api->format(api->device,descriptor,1))&&
       !(rc=api->producer(api->device,descriptor,
           GRALLOC1_PRODUCER_USAGE_GPU_RENDER_TARGET |
           GRALLOC1_PRODUCER_USAGE_CPU_WRITE_OFTEN))&&
       !(rc=api->consumer(api->device,descriptor,GRALLOC1_CONSUMER_USAGE_GPU_TEXTURE)))
        rc=api->allocate(api->device,1,&descriptor,&handle);
    const gboolean allocated=handle&&(rc==GRALLOC1_ERROR_NONE||rc==GRALLOC1_ERROR_NOT_SHARED);
    int destroy=api->destroy(api->device,descriptor);
    if(!allocated){printf("RUNTIME_SHM_ALLOCATE status=%d handle=%d\n",rc,handle!=NULL);return -EREMOTEIO;}
    guint32 stride=0;gralloc1_backing_store_t store=0;
    int stride_rc=api->stride(api->device,handle,&stride);
    int id_rc=self->backing(api->device,handle,&store);
    if(destroy||stride_rc||stride<w||id_rc||handle->version!=sizeof(native_handle_t)||
       handle->numFds<0||handle->numFds>16||handle->numInts<0||handle->numInts>256){
        api->release(api->device,handle);return -EPROTO;
    }
    slot->handle=handle;slot->width=w;slot->height=h;slot->stride=stride;
    slot->id.first=store;
    printf("RUNTIME_SHM_ALLOCATION slot=%d width=%u height=%u stride=%u backing=%llu fds=%d ints=%d\n",
        slot->slot,w,h,stride,(unsigned long long)store,handle->numFds,handle->numInts);
    return 0;
}
int anco_shm_queue_submit(AncoShmQueue *self,struct wlr_buffer *source,guint32 seq,
    const AncoShmDecoration *decoration)
{
    if(!self||!source||self->retiring)return -EINVAL;
    GBinderRemoteObject *peer=anco_delegator_producer_ref_client(self->producer);
    if(!peer)return -EAGAIN;
    gbinder_remote_object_unref(peer);
    int rc=open_device(self);
    if(rc)return rc;
    ShmSlot *slot=NULL;
    for(guint i=0;i<self->count;++i)if(!self->slots[i].in_flight&&!self->slots[i].failed){slot=&self->slots[i];break;}
    if(!slot){
        if(self->count==ANCO_SHM_MAX_IN_FLIGHT){
            if(g_getenv("ANCO_SHM_DIAGNOSTIC_LAYOUT"))
                printf("RUNTIME_SHM_BACKPRESSURE allocated=%u limit=%u\n",
                    self->count,ANCO_SHM_MAX_IN_FLIGHT);
            return -ENOBUFS;
        }
        slot=&self->slots[self->count++];
    }
    if(source->width<=0||source->height<=0)return -EINVAL;
    const guint64 outer_width=(guint64)source->width+(decoration?
        (guint64)decoration->left+decoration->right:0);
    const guint64 outer_height=(guint64)source->height+(decoration?
        (guint64)decoration->top+decoration->bottom:0);
    if(!outer_width||!outer_height||outer_width>INT32_MAX||outer_height>INT32_MAX)
        return -ERANGE;
    if(slot->handle&&(slot->width!=(guint32)outer_width||slot->height!=(guint32)outer_height)){
        AncoDelegatorResult clear;
        rc=anco_delegator_producer_clear_slot(self->producer,slot->slot,&clear);
        /* Original BufferQueueCore::clearBufferSlotLocked is a void caller.
         * dequeueBuffer continues after its code4 request, including a host
         * nonzero status. Do not invent a successful clear receipt or make
         * its raw status an extra allocation gate. This slot is selectable
         * only after its actual release callback and fence have authorized
         * reuse. The following real code0/1 still decides submission. */
        printf("RUNTIME_SHM_REPLACE_CLEAR slot=%d status=%d local=%d transport=%d original_void_caller=1\n",
            slot->slot,rc,clear.local_error,clear.transport);
        rc=self->api->release(self->api->device,slot->handle);slot->handle=NULL;
        if(rc)return -EREMOTEIO;
    }
    if(!slot->handle&&(rc=allocate(self,slot,(guint32)outer_width,(guint32)outer_height)))return rc;
    void *pixels=NULL;guint32 format=0;size_t stride=0;
    if(!wlr_buffer_begin_data_ptr_access(source,WLR_BUFFER_DATA_PTR_ACCESS_READ,&pixels,&format,&stride))return -ENOTSUP;
    if(!pixels||(format!=DRM_FORMAT_ARGB8888&&format!=DRM_FORMAT_XRGB8888)||
        stride<(size_t)source->width*4||stride>SIZE_MAX/(size_t)source->height){
        wlr_buffer_end_data_ptr_access(source);return -ENOTSUP;
    }
    if(g_getenv("ANCO_SHM_DIAGNOSTIC_LAYOUT"))
        printf("RUNTIME_SHM_SOURCE width=%d height=%d stride=%zu format=0x%08x xrgb=%d\n",
            source->width,source->height,stride,format,format==DRM_FORMAT_XRGB8888);
    diagnostic_dump_pixels(g_getenv("ANCO_SHM_DIAGNOSTIC_DUMP_DIR"),"source",
        slot,seq,pixels,(guint32)source->width,(guint32)source->height,stride);
    /* Original BufferQueue order is dequeue/AttachBuffer before the producer
     * locks and writes the returned allocation.  Keep that order here so the
     * host import exists when CPU cache ownership starts; code1 remains after
     * unlock, as the original queueBuffer operation. */
    AncoDelegatorMetadataUpdate metadata;
    anco_delegator_producer_set_transform(self->producer,0,&metadata);
    anco_delegator_producer_set_queue_size(self->producer,
        ANCO_SHM_MAX_IN_FLIGHT,&metadata);
    _Alignas(8) guint8 legacy[0x170];
    const void *wire_handle=slot->handle;
    size_t wire_bytes=12u+4u*(slot->handle->numFds+slot->handle->numInts);
    const char *abi=g_getenv("ANCO_NATIVE_BUFFER_ABI");
    if(abi&&strcmp(abi,"6.0-linear-rgba")==0){
        size_t encoded=0;
        rc=anco_native_handle_legacy87_rgba(slot->handle,wire_bytes,legacy,sizeof(legacy),&encoded);
        printf("RUNTIME_SHM_ORIGINAL_ABI status=%d source_bytes=%zu encoded_bytes=%zu\n",rc,wire_bytes,encoded);
        if(rc){wlr_buffer_end_data_ptr_access(source);slot->failed=TRUE;return rc;}
        wire_handle=legacy;wire_bytes=encoded;
    }else if(abi&&abi[0]){
        wlr_buffer_end_data_ptr_access(source);slot->failed=TRUE;return -EINVAL;
    }
    AncoDelegatorDequeueCall dequeue={.request_flag=1,.slot=slot->slot,
        .handle=wire_handle,.handle_bytes=wire_bytes};
    AncoDelegatorDequeueReport deq;
    rc=anco_delegator_producer_dequeue(self->producer,&dequeue,&deq);
    printf("RUNTIME_SHM_DEQUEUE slot=%d status=%d transport=%d decoded=%d reply=%d\n",
        slot->slot,rc,deq.wire.transport,deq.wire.reply_decoded,deq.wire.reply_value);
    /* Original guest records reply10 but DOES NOT reject subsequent queue.
     * Host writes10 for the accepted AttachBuffer predicate (including0).
     * The historical conversion_failed accessor name is not that predicate. */
    if(rc||deq.pending_cached||!deq.wire.reply_decoded){
        wlr_buffer_end_data_ptr_access(source);
        slot->failed=TRUE;return rc?rc:-EPROTO;
    }
    const guint64 staging_stride64=outer_width*4u;
    const guint64 staging_bytes64=staging_stride64*outer_height;
    const guint64 target_stride64=(guint64)slot->stride*4u;
    const guint64 target_bytes64=target_stride64*outer_height;
    if(outer_width>3840||outer_height>2160||staging_stride64>INT32_MAX||
       staging_bytes64>SIZE_MAX||target_stride64>INT32_MAX||
       target_bytes64>INT32_MAX||slot->handle->numFds<1){
        wlr_buffer_end_data_ptr_access(source);slot->failed=TRUE;return -ERANGE;
    }
    const size_t staging_stride=(size_t)staging_stride64;
    const size_t staging_bytes=(size_t)staging_bytes64;
    if(self->staging_capacity<staging_bytes){
        guint8 *replacement=g_try_realloc(self->staging,staging_bytes);
        if(!replacement){wlr_buffer_end_data_ptr_access(source);return -ENOMEM;}
        self->staging=replacement;self->staging_capacity=staging_bytes;
    }
    if(decoration)draw_server_decoration(self->staging,staging_stride,
        (guint32)outer_width,(guint32)outer_height,decoration);
    else memset(self->staging,0,staging_bytes);
    copy_bgra_to_rgba(self->staging+
        (size_t)(decoration?decoration->top:0)*staging_stride+
        (size_t)(decoration?decoration->left:0)*4,staging_stride,
        pixels,stride,source->width,source->height,format==DRM_FORMAT_XRGB8888);
    diagnostic_dump_pixels(g_getenv("ANCO_SHM_DIAGNOSTIC_DUMP_DIR"),"target",
        slot,seq,self->staging,slot->width,slot->height,staging_stride);
    /* The stock host queue still owns allocation, registration, release and
     * fences.  Write its gralloc allocation through the already validated
     * Mali EGL DMA-BUF target path so no CPU map or DMA_BUF_SYNC ioctl can
     * reach the SP68 ION end_cpu_access kernel path that panicked. */
    rc=l410_egl_compose_pixels(self->staging,(int)staging_stride,
        (int)outer_width,(int)outer_height,slot->handle->data[0],
        (int)outer_width,(int)outer_height,(int)target_stride64,
        (int)target_bytes64,12);
    wlr_buffer_end_data_ptr_access(source);
    printf("RUNTIME_SHM_GPU_TARGET seq=%u slot=%d status=%d cpu_target_map=0 target_fd=%d\n",
        seq,slot->slot,rc,slot->handle->data[0]);
    if(rc){
        AncoDelegatorResult clear;
        (void)anco_delegator_producer_clear_slot(self->producer,slot->slot,&clear);
        slot->failed=TRUE;return -EREMOTEIO;
    }
    const int acquire=-1;
    slot->id.second=seq;slot->in_flight=TRUE;
    const gint32 damage[4]={0,0,(gint32)outer_width,(gint32)outer_height};
    const AncoDelegatorQueueCall queue={slot->slot,damage,acquire};
    AncoDelegatorQueueReport queued;
    rc=anco_delegator_producer_queue(self->producer,&queue,&queued);
    if(!queued.pending_cached&&acquire>=0)close(acquire);
    /* The two internal release fields identify the ACTUAL native allocation
     * and actual Wayland commit. They are not sent on the host wire or forged
     * Android GraphicBuffer IDs / drawing-completed receipts. Pending follows
     * the original queue/acquisition order, including a genuine early code3. */
    const gboolean pending=anco_delegator_producer_add_pending(self->producer,&slot->id,slot->slot);
    if(!pending&&!anco_delegator_producer_registered(self->producer,slot->slot)){
        int release_fence=anco_delegator_producer_take_fence(self->producer,slot->slot);
        released(&slot->id,release_fence,self);
    }
    printf("RUNTIME_SHM_QUEUE slot=%d commit=%u status=%d transport=%d notify=%d pending=%d dup_error=%d host_submitted=%d\n",
        slot->slot,seq,rc,queued.wire.transport,queued.notify_status,pending,
        queued.fence_dup_error,!rc&&!queued.pending_cached&&!queued.fence_dup_error);
    if(rc||queued.pending_cached||queued.fence_dup_error){slot->failed=TRUE;return rc?rc:-EPROTO;}
    /* Snapshot only the actually accepted code0/1 pair. Keep the independent
     * allocation/release lease unchanged; neither this copy nor its getter
     * grants reuse of a buffer whose original fence is still outstanding. */
    if(wire_handle==legacy){
        memcpy(self->submission_handle,legacy,wire_bytes);
        wire_handle=self->submission_handle;
    }
    self->submission=(AncoShmSubmission){wire_handle,wire_bytes,
        slot->width,slot->height,seq};
    return 0;
}
int anco_shm_queue_submission(AncoShmQueue *self,AncoShmSubmission *out)
{
    if(!self||!out)return -EINVAL;
    if(self->retiring||!self->submission.handle)return -ENODATA;
    *out=self->submission;
    return 0;
}
void anco_shm_queue_free(AncoShmQueue *self)
{
    if(!self)return;
    self->retiring=TRUE;
    anco_delegator_producer_observe_client(self->producer,NULL,NULL);
    AncoDelegatorResult clear;
    int rc=anco_delegator_producer_clear_all(self->producer,&clear);
    printf("RUNTIME_SHM_CLEAR status=%d transport=%d\n",rc,clear.transport);
    anco_delegator_producer_set_listener(self->producer,NULL,NULL);
    for(guint i=0;i<self->count;++i){
        ShmSlot *slot=&self->slots[i];
        if(slot->fence_source)g_source_remove(slot->fence_source);
        if(slot->fence>=0)close(slot->fence);
        if(slot->handle){
            rc=self->api->release(self->api->device,slot->handle);
            printf("RUNTIME_SHM_DROP_REFERENCE slot=%u inflight=%d status=%d\n",i,slot->in_flight,rc);
        }
    }
    if(self->device)printf("RUNTIME_SHM_DEVICE_CLOSE status=%d\n",anco_gralloc1_device_close(self->device));
    g_free(self->staging);
    anco_delegator_producer_unref(self->producer);g_free(self);
}
