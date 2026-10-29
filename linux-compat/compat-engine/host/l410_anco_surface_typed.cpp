/*
 * Typed HarmonyOS surface hand-off for the L410 Linux compatibility runtime.
 *
 * This deliberately uses the OpenHarmony Surface and sptr implementations.
 * It does not reproduce object layouts, manufacture libc++ strings, or call
 * C++ methods through untyped dlsym function pointers.
 */
#include <cstdint>
#include <new>
#include <string>
#include <unistd.h>

#include "surface.h"
#include "external_window.h"
#include "native_window.h"
#include "itransact_surface_delegator.h"
#include <iremote_stub.h>
#include <message_parcel.h>

namespace OHOS {
/* Exported by libsurface.z.so.  Only this non-layout-dependent member is
 * declared because the platform source drop does not match the device's full
 * ConsumerSurface vtable version. */
class ConsumerSurface {
public:
    sptr<IBufferProducer> GetProducer() const;
    GSError SetDefaultWidthAndHeight(int32_t width, int32_t height);
};
} // namespace OHOS

namespace OHOS::ANCO {

/* The proprietary header is not present in the platform source drop.  These
 * declarations are taken from the exported C++ symbols of
 * libgraphic_broker_client.z.so.  The class is only ever obtained by its
 * exported singleton accessor; its storage and layout remain library-owned.
 */
class GraphicBrokerClient final {
public:
    static GraphicBrokerClient &GetInstance();
    bool Init();
    bool SetSurfaceWithWindowID(long windowId, OHOS::sptr<OHOS::Surface> &surface);
    bool DestroySurfaceByWindowID(long windowId);
    bool NotifyTaskDestroyed(int32_t taskId);
    bool SetAncoStatus(int32_t state);
};

} // namespace OHOS::ANCO

namespace {
/* A complete delegator must receive the CLIENT transaction and register the
 * producer endpoint before RequestBuffer can work.  On the current device
 * broker node creation succeeds but CLIENT does not arrive; this adapter is
 * still experimental and must not be used as the default display path. */
class L410SurfaceDelegatorListener final : public OHOS::IRemoteStub<OHOS::ITransactSurfaceDelegator> {
public:
    explicit L410SurfaceDelegatorListener(OHOS::sptr<OHOS::Surface> surface)
        : surface_(std::move(surface)) {}

    int OnRemoteRequest(uint32_t code, OHOS::MessageParcel &data,
        OHOS::MessageParcel &reply, OHOS::MessageOption &option) override
    {
        if (code != 5) {
            return OHOS::IRemoteStub<OHOS::ITransactSurfaceDelegator>::OnRemoteRequest(
                code, data, reply, option);
        }
        const std::u16string token = data.ReadInterfaceToken();
        fprintf(stderr, "l410-anco: delegator CLIENT token_units=%zu\n", token.size());
        OHOS::sptr<OHOS::IRemoteObject> client = data.ReadRemoteObject();
        if (client == nullptr || surface_ == nullptr) return -1;
        const int32_t result = surface_->RegisterSurfaceDelegator(client);
        registerResult_ = result;
        fprintf(stderr, "l410-anco: delegator registration=%d\n", result);
        return result;
    }

    int32_t RegisterResult() const { return registerResult_; }

private:
    OHOS::sptr<OHOS::Surface> surface_;
    int32_t registerResult_ = INT32_MIN;
};
} // namespace

struct L410AncoSurface {
    OHOS::sptr<OHOS::Surface> consumer;
    OHOS::sptr<OHOS::Surface> producer;
    OHOS::sptr<OHOS::IBufferProducer> queue;
    OHOS::sptr<L410SurfaceDelegatorListener> delegator;
    OHOS::sptr<OHOS::IRemoteObject> delegatorRemote;
    OHNativeWindow *nativeWindow = nullptr;
    int32_t windowId = 0;
};

struct L410NativeBufferInfo {
    int32_t fd;
    int32_t width;
    int32_t stride;
    int32_t height;
    int32_t size;
    int32_t format;
    uint64_t usage;
};

static int32_t gLastCreateStage = 0;

extern "C" int32_t l410_anco_surface_last_create_stage(void)
{
    return gLastCreateStage;
}

extern "C" L410AncoSurface *l410_anco_surface_create_typed(
    const char *name, int32_t width, int32_t height)
{
    gLastCreateStage = 1;
    auto *handle = new (std::nothrow) L410AncoSurface;
    if (handle == nullptr) {
        gLastCreateStage = -1;
        return nullptr;
    }

    handle->consumer = OHOS::Surface::CreateSurfaceAsConsumer(
        std::string(name == nullptr ? "l410-linux-window" : name));
    if (handle->consumer == nullptr) {
        gLastCreateStage = -2;
        delete handle;
        return nullptr;
    }

    auto *consumer = reinterpret_cast<OHOS::ConsumerSurface *>(handle->consumer.GetRefPtr());
    if (width > 0 && height > 0 &&
        consumer->OHOS::ConsumerSurface::SetDefaultWidthAndHeight(width, height) != OHOS::GSERROR_OK) {
        gLastCreateStage = -5;
        delete handle;
        return nullptr;
    }
    OHOS::sptr<OHOS::IBufferProducer> queue =
        consumer->OHOS::ConsumerSurface::GetProducer();
    if (queue == nullptr) {
        gLastCreateStage = -3;
        delete handle;
        return nullptr;
    }
    handle->queue = queue;
    handle->producer = OHOS::Surface::CreateSurfaceAsProducer(queue);
    if (handle->producer == nullptr) {
        gLastCreateStage = -4;
        delete handle;
        return nullptr;
    }

    handle->delegator = new (std::nothrow)
        L410SurfaceDelegatorListener(handle->consumer);
    if (handle->delegator == nullptr) {
        gLastCreateStage = -7;
        delete handle;
        return nullptr;
    }
    handle->delegatorRemote = handle->delegator;
    if (handle->delegatorRemote == nullptr) {
        gLastCreateStage = -8;
        delete handle;
        return nullptr;
    }
    /* The NDK function expects a pointer to the real sptr<Surface>.  This
     * preserves the producer queue object and gives EGL plus the buffer/fence
     * bridge the same OHNativeWindow used by native Harmony rendering. */
    handle->nativeWindow = OH_NativeWindow_CreateNativeWindow(&handle->producer);
    if (handle->nativeWindow == nullptr) {
        gLastCreateStage = -6;
        delete handle;
        return nullptr;
    }
    gLastCreateStage = 9;
    return handle;
}

extern "C" void *l410_anco_surface_delegator_remote_typed(
    L410AncoSurface *handle)
{
    return handle == nullptr ? nullptr : handle->delegatorRemote.GetRefPtr();
}

extern "C" int32_t l410_anco_surface_delegator_register_result_typed(
    L410AncoSurface *handle)
{
    return handle == nullptr || handle->delegator == nullptr ? INT32_MIN :
        handle->delegator->RegisterResult();
}

extern "C" int32_t l410_anco_surface_publish_typed(
    L410AncoSurface *handle, int32_t windowId)
{
    if (handle == nullptr || handle->producer == nullptr || windowId <= 0) {
        return -1;
    }
    auto &client = OHOS::ANCO::GraphicBrokerClient::GetInstance();
    if (!client.Init()) {
        return -2;
    }
    const bool published = client.SetSurfaceWithWindowID(
        static_cast<long>(windowId), handle->producer);
    if (published) {
        handle->windowId = windowId;
    }
    return published ? 0 : -3;
}

/* Publish the Surface already owned by an ArkUI XComponent.  This queue is
 * connected to RenderService's consumer, matching the SurfaceView hand-off. */
extern "C" int32_t l410_anco_native_window_publish_typed(
    void *nativeWindow, int32_t windowId)
{
    auto *window = static_cast<NativeWindow *>(nativeWindow);
    if (window == nullptr || window->surface == nullptr || windowId <= 0) return -1;
    auto &client = OHOS::ANCO::GraphicBrokerClient::GetInstance();
    if (!client.Init()) return -2;
    return client.SetSurfaceWithWindowID(static_cast<long>(windowId), window->surface) ? 0 : -3;
}

extern "C" int32_t l410_anco_native_window_unpublish_typed(int32_t windowId)
{
    if (windowId <= 0) return -1;
    auto &client = OHOS::ANCO::GraphicBrokerClient::GetInstance();
    if (!client.Init()) return -2;
    return client.DestroySurfaceByWindowID(static_cast<long>(windowId)) ? 0 : -3;
}

extern "C" OHOS::Surface *l410_anco_surface_producer_typed(L410AncoSurface *handle)
{
    return handle == nullptr ? nullptr : handle->producer.GetRefPtr();
}

/* Diagnostic proxy call borrows the exact sptr obtained from the consumer.
 * The surface handle retains it until the call completes. */
extern "C" const void *l410_anco_surface_queue_ref_typed(L410AncoSurface *handle)
{
    return handle == nullptr ? nullptr : &handle->queue;
}

extern "C" OHNativeWindow *l410_anco_surface_native_window_typed(L410AncoSurface *handle)
{
    return handle == nullptr ? nullptr : handle->nativeWindow;
}

extern "C" int32_t l410_anco_surface_id_typed(
    L410AncoSurface *handle, uint64_t *surfaceId)
{
    if (handle == nullptr || handle->nativeWindow == nullptr || surfaceId == nullptr) {
        return -1;
    }
    return OH_NativeWindow_GetSurfaceId(handle->nativeWindow, surfaceId);
}

extern "C" int32_t l410_anco_surface_configure_typed(
    L410AncoSurface *handle, int32_t width, int32_t height)
{
    if (handle == nullptr || handle->nativeWindow == nullptr || width < 16 || height < 16) {
        return -1;
    }
    int32_t result = OH_NativeWindow_NativeWindowHandleOpt(
        handle->nativeWindow, SET_BUFFER_GEOMETRY, width, height);
    if (result == 0) {
        result = OH_NativeWindow_NativeWindowHandleOpt(
            handle->nativeWindow, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888);
    }
    if (result == 0) {
        /* CPU read/write keeps the first Qt/GTK milestone directly mappable.
         * The same dma-buf fd is later imported by EGL/Vulkan instead of copied. */
        result = OH_NativeWindow_NativeWindowHandleOpt(
            handle->nativeWindow, SET_USAGE, static_cast<uint64_t>(3 | 256 | 512));
    }
    if (result == 0) {
        result = OH_NativeWindow_NativeWindowHandleOpt(
            handle->nativeWindow, SET_TIMEOUT, 200);
    }
    if (result == 0) {
        result = OH_NativeWindow_NativeWindowSetScalingModeV2(
            handle->nativeWindow, OH_SCALING_MODE_FREEZE_V2);
    }
    return result;
}

extern "C" int32_t l410_anco_surface_request_buffer_typed(
    L410AncoSurface *handle, void **bufferOut, int32_t *fenceFd,
    L410NativeBufferInfo *info)
{
    if (handle == nullptr || handle->nativeWindow == nullptr || bufferOut == nullptr ||
        fenceFd == nullptr || info == nullptr) {
        return -1;
    }
    OHNativeWindowBuffer *buffer = nullptr;
    int fence = -1;
    const int32_t result = OH_NativeWindow_NativeWindowRequestBuffer(
        handle->nativeWindow, &buffer, &fence);
    if (result != 0 || buffer == nullptr) {
        return result != 0 ? result : -2;
    }
    BufferHandle *native = OH_NativeWindow_GetBufferHandleFromNative(buffer);
    if (native == nullptr || native->fd < 0) {
        OH_NativeWindow_NativeWindowAbortBuffer(handle->nativeWindow, buffer);
        if (fence >= 0) {
            close(fence);
        }
        return -3;
    }
    info->fd = native->fd;
    info->width = native->width;
    info->stride = native->stride;
    info->height = native->height;
    info->size = native->size;
    info->format = native->format;
    info->usage = native->usage;
    *bufferOut = buffer;
    *fenceFd = fence;
    return 0;
}

extern "C" int32_t l410_anco_surface_flush_buffer_typed(
    L410AncoSurface *handle, void *buffer, int32_t fenceFd)
{
    if (handle == nullptr || handle->nativeWindow == nullptr || buffer == nullptr) {
        return -1;
    }
    Region region {nullptr, 0};
    return OH_NativeWindow_NativeWindowFlushBuffer(
        handle->nativeWindow, static_cast<OHNativeWindowBuffer *>(buffer), fenceFd, region);
}

extern "C" int32_t l410_anco_surface_abort_buffer_typed(
    L410AncoSurface *handle, void *buffer)
{
    if (handle == nullptr || handle->nativeWindow == nullptr || buffer == nullptr) {
        return -1;
    }
    return OH_NativeWindow_NativeWindowAbortBuffer(
        handle->nativeWindow, static_cast<OHNativeWindowBuffer *>(buffer));
}

extern "C" int32_t l410_anco_surface_destroy_typed(L410AncoSurface *handle)
{
    if (handle == nullptr) {
        return -1;
    }
    int32_t result = 0;
    if (handle->windowId > 0) {
        auto &client = OHOS::ANCO::GraphicBrokerClient::GetInstance();
        if (client.Init()) {
            result = client.DestroySurfaceByWindowID(static_cast<long>(handle->windowId)) ? 0 : -3;
        } else {
            result = -2;
        }
    }
    if (handle->nativeWindow != nullptr) {
        OH_NativeWindow_NativeObjectUnreference(handle->nativeWindow);
        handle->nativeWindow = nullptr;
    }
    delete handle;
    return result;
}
