/*
 * Typed shell-side window lifecycle callback used by the stock AnCo graphic
 * broker.  libgraphic_broker_client constructs its SurfaceViewListenerStub
 * around this interface and registers the resulting remote object with the
 * broker.  The method order and arguments below are taken from the exported
 * SurfaceViewListenerStub methods and their parcel decoding on this device.
 */
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

/* Never replace this with a methods-only declaration.  IPCObjectStub owns
 * mutex, descriptor and request-state fields; allocating a shortened class
 * corrupts the object before WriteRemoteObject can serialize it. */
#include <ipc_object_stub.h>
#include <surface.h>

namespace OHOS::ANCO {

struct GraphicWindowProperty {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
};

class IGraphicNapiCallback {
public:
    virtual ~IGraphicNapiCallback() = default;
    virtual int32_t CreateWindow(uint64_t displayId, int32_t windowId,
        int32_t taskId, const std::string &name) = 0;
    virtual int32_t RemoveWindow(uint64_t displayId, int32_t windowId) = 0;
    virtual int32_t ShowWindow(uint64_t displayId, int32_t windowId) = 0;
    virtual int32_t HideWindow(uint64_t displayId, int32_t windowId) = 0;
    virtual int32_t UpdateWindowProperty(uint64_t displayId, int32_t windowId,
        const GraphicWindowProperty &property) = 0;
    virtual int32_t UpdateWindowZIndex(uint64_t displayId, int32_t windowId,
        int32_t operation, uint64_t relativeId, bool top) = 0;
    virtual int32_t UpdateWindowBackgroundColor(uint64_t displayId,
        int32_t windowId, int32_t color, const std::string &name) = 0;
};

class GraphicBrokerClient final {
public:
    static GraphicBrokerClient &GetInstance();
    bool Init();
    bool RegisterWindowLifeCycleCallback(
        std::shared_ptr<IGraphicNapiCallback> callback);
    bool UnregisterWindowLifeCycleCallback();
    bool NotifyWindowLifecycleListenerStatus(int32_t status);
};

/* This is the real system proxy exported by libgraphic_broker_proxy.z.so.
 * GraphicBrokerClient::Init constructs it and stores its sptr at byte offset
 * 8.  The offset is not an estimate: both Init (stores [client + 8]) and all
 * public forwarding methods (load [client + 8]) use it in the device binary.
 * We only borrow that client-owned strong reference. */
class GraphicBroker final {
public:
    int32_t CreateSurfaceNode(uint64_t displayId, int32_t windowId,
        const std::string &name, const OHOS::sptr<OHOS::IRemoteObject> &listener);
    int32_t RemoveSurfaceNode(uint64_t displayId, int32_t windowId);
    int32_t SetSurfaceWithWindowID(long windowId,
        const OHOS::sptr<OHOS::IBufferProducer> &producer);
};

} // namespace OHOS::ANCO

enum : int32_t {
    L410_ANCO_WINDOW_CREATE = 1,
    L410_ANCO_WINDOW_REMOVE = 2,
    L410_ANCO_WINDOW_SHOW = 3,
    L410_ANCO_WINDOW_HIDE = 4,
    L410_ANCO_WINDOW_PROPERTY = 5,
    L410_ANCO_WINDOW_ZINDEX = 6,
    L410_ANCO_WINDOW_BACKGROUND = 7,
};

struct L410AncoLifecycleEvent {
    int32_t type;
    uint64_t displayId;
    int32_t windowId;
    int32_t taskId;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    int32_t operation;
    uint64_t relativeId;
    int32_t value;
    char name[160];
};

using L410AncoLifecycleCallback = void (*)(
    const L410AncoLifecycleEvent *, void *);

namespace {
std::mutex gMutex;
L410AncoLifecycleCallback gCallback = nullptr;
void *gContext = nullptr;

void CopyName(char *output, size_t capacity, const std::string &name)
{
    if (capacity == 0) return;
    const size_t count = name.size() < capacity - 1 ? name.size() : capacity - 1;
    if (count != 0) __builtin_memcpy(output, name.data(), count);
    output[count] = '\0';
}

void Dispatch(const L410AncoLifecycleEvent &event)
{
    L410AncoLifecycleCallback callback;
    void *context;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        callback = gCallback;
        context = gContext;
    }
    if (callback != nullptr) callback(&event, context);
}

class LifecycleCallback final : public OHOS::ANCO::IGraphicNapiCallback {
public:
    int32_t CreateWindow(uint64_t displayId, int32_t windowId,
        int32_t taskId, const std::string &name) override
    {
        L410AncoLifecycleEvent event {};
        event.type = L410_ANCO_WINDOW_CREATE;
        event.displayId = displayId;
        event.windowId = windowId;
        event.taskId = taskId;
        CopyName(event.name, sizeof(event.name), name);
        Dispatch(event);
        return 0;
    }

    int32_t RemoveWindow(uint64_t displayId, int32_t windowId) override
    {
        return Simple(L410_ANCO_WINDOW_REMOVE, displayId, windowId);
    }

    int32_t ShowWindow(uint64_t displayId, int32_t windowId) override
    {
        return Simple(L410_ANCO_WINDOW_SHOW, displayId, windowId);
    }

    int32_t HideWindow(uint64_t displayId, int32_t windowId) override
    {
        return Simple(L410_ANCO_WINDOW_HIDE, displayId, windowId);
    }

    int32_t UpdateWindowProperty(uint64_t displayId, int32_t windowId,
        const OHOS::ANCO::GraphicWindowProperty &property) override
    {
        L410AncoLifecycleEvent event {};
        event.type = L410_ANCO_WINDOW_PROPERTY;
        event.displayId = displayId;
        event.windowId = windowId;
        event.x = property.x;
        event.y = property.y;
        event.width = property.width;
        event.height = property.height;
        Dispatch(event);
        return 0;
    }

    int32_t UpdateWindowZIndex(uint64_t displayId, int32_t windowId,
        int32_t operation, uint64_t relativeId, bool top) override
    {
        L410AncoLifecycleEvent event {};
        event.type = L410_ANCO_WINDOW_ZINDEX;
        event.displayId = displayId;
        event.windowId = windowId;
        event.operation = operation;
        event.relativeId = relativeId;
        event.value = top ? 1 : 0;
        Dispatch(event);
        return 0;
    }

    int32_t UpdateWindowBackgroundColor(uint64_t displayId,
        int32_t windowId, int32_t color, const std::string &name) override
    {
        L410AncoLifecycleEvent event {};
        event.type = L410_ANCO_WINDOW_BACKGROUND;
        event.displayId = displayId;
        event.windowId = windowId;
        event.value = color;
        CopyName(event.name, sizeof(event.name), name);
        Dispatch(event);
        return 0;
    }

private:
    static int32_t Simple(int32_t type, uint64_t displayId, int32_t windowId)
    {
        L410AncoLifecycleEvent event {};
        event.type = type;
        event.displayId = displayId;
        event.windowId = windowId;
        Dispatch(event);
        return 0;
    }
};

std::shared_ptr<LifecycleCallback> gLifecycle;
std::unordered_map<int32_t, OHOS::sptr<OHOS::IRemoteObject>> gSurfaceListeners;

OHOS::ANCO::GraphicBroker *GetGraphicBrokerProxy()
{
    auto &client = OHOS::ANCO::GraphicBrokerClient::GetInstance();
    if (!client.Init()) return nullptr;
    auto *slot = reinterpret_cast<OHOS::ANCO::GraphicBroker **>(
        reinterpret_cast<uint8_t *>(&client) + 8);
    return slot == nullptr ? nullptr : *slot;
}
} // namespace

/* Diagnostic return code from the real broker proxy.  The public client
 * reduces every nonzero service error to false, which obscures the SP68
 * failure.  The caller owns the queue and keeps it alive for this call. */
extern "C" int32_t l410_anco_surface_publish_raw_typed(
    const void *queueRef, int32_t windowId)
{
    if (queueRef == nullptr || windowId <= 0) return -1;
    auto *broker = GetGraphicBrokerProxy();
    if (broker == nullptr) return -2;
    const auto &producer = *static_cast<const OHOS::sptr<OHOS::IBufferProducer> *>(queueRef);
    if (producer == nullptr) return -3;
    return broker->SetSurfaceWithWindowID(static_cast<long>(windowId), producer);
}

extern "C" int32_t l410_anco_lifecycle_start(
    L410AncoLifecycleCallback callback, void *context)
{
    if (callback == nullptr) return -1;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gCallback = callback;
        gContext = context;
        if (gLifecycle) return 0;
    }
    auto &client = OHOS::ANCO::GraphicBrokerClient::GetInstance();
    if (!client.Init()) {
        std::lock_guard<std::mutex> lock(gMutex);
        gCallback = nullptr;
        gContext = nullptr;
        return -2;
    }
    auto lifecycle = std::make_shared<LifecycleCallback>();
    if (!client.RegisterWindowLifeCycleCallback(lifecycle)) {
        std::lock_guard<std::mutex> lock(gMutex);
        gCallback = nullptr;
        gContext = nullptr;
        return -3;
    }
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gLifecycle = lifecycle;
    }
    client.NotifyWindowLifecycleListenerStatus(1);
    return 0;
}

extern "C" int32_t l410_anco_surface_node_create_typed(
    uint64_t displayId, int32_t windowId, const char *name)
{
    if (windowId <= 0) return -1;
    auto *broker = GetGraphicBrokerProxy();
    if (broker == nullptr) return -2;

    OHOS::sptr<OHOS::IPCObjectStub> stub = new (std::nothrow)
        OHOS::IPCObjectStub(u"l410.anco.surface.listener", false);
    if (stub == nullptr) return -3;
    OHOS::sptr<OHOS::IRemoteObject> listener = stub;
    const int32_t result = broker->CreateSurfaceNode(displayId, windowId,
        std::string(name == nullptr ? "l410-linux-window" : name), listener);
    if (result == 0) {
        std::lock_guard<std::mutex> lock(gMutex);
        gSurfaceListeners[windowId] = listener;
    }
    return result;
}

extern "C" int32_t l410_anco_surface_node_create_delegated_typed(
    uint64_t displayId, int32_t windowId, const char *name, void *remoteObject)
{
    if (windowId <= 0 || remoteObject == nullptr) return -1;
    auto *broker = GetGraphicBrokerProxy();
    if (broker == nullptr) return -2;
    auto *raw = static_cast<OHOS::IRemoteObject *>(remoteObject);
    OHOS::sptr<OHOS::IRemoteObject> listener = raw;
    const int32_t result = broker->CreateSurfaceNode(displayId, windowId,
        std::string(name == nullptr ? "l410-linux-window" : name), listener);
    if (result == 0) {
        std::lock_guard<std::mutex> lock(gMutex);
        gSurfaceListeners[windowId] = listener;
    }
    return result;
}

extern "C" int32_t l410_anco_surface_node_remove_typed(
    uint64_t displayId, int32_t windowId)
{
    if (windowId <= 0) return -1;
    auto *broker = GetGraphicBrokerProxy();
    if (broker == nullptr) return -2;
    const int32_t result = broker->RemoveSurfaceNode(displayId, windowId);
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gSurfaceListeners.erase(windowId);
    }
    return result;
}

extern "C" int32_t l410_anco_lifecycle_stop(void)
{
    std::shared_ptr<LifecycleCallback> lifecycle;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gCallback = nullptr;
        gContext = nullptr;
        lifecycle = gLifecycle;
        gLifecycle.reset();
    }
    if (!lifecycle) return 0;
    auto &client = OHOS::ANCO::GraphicBrokerClient::GetInstance();
    client.NotifyWindowLifecycleListenerStatus(0);
    return client.UnregisterWindowLifeCycleCallback() ? 0 : -3;
}
