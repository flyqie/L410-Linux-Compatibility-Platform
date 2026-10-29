/*
 * Typed MMI ANCO input consumer for the L410 Linux compatibility runtime.
 *
 * Receive only the AnCo-routed input consumer events and window table.
 * Ordinary MMI monitors also observe unrelated native input and bypass the
 * original AnCo routing/IME ordering; they must not be a fallback here.
 */
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>

#include "i_anco_consumer.h"
#include "refbase.h"

namespace OHOS::MiscServices {
/* The device exports these two native IMF methods.  The JS custom editor in
 * the same process owns the session; this adapter only dispatches the
 * original MMI KeyEvent before falling back to the Linux seat. */
class InputMethodController : public RefBase {
public:
    static sptr<InputMethodController> GetInstance();
    int32_t DispatchKeyEvent(std::shared_ptr<MMI::KeyEvent>,
        std::function<void(std::shared_ptr<MMI::KeyEvent> &, bool)>);
};
} // namespace OHOS::MiscServices

namespace OHOS::MMI {
/* Only the two exported methods used here are declared.  The singleton and
 * all object storage remain owned by libmmi-client.z.so. */
class InputManager final {
public:
    static InputManager *GetInstance();
    int32_t AncoAddConsumer(std::shared_ptr<IAncoConsumer> consumer);
    int32_t AncoRemoveConsumer(std::shared_ptr<IAncoConsumer> consumer);
};
} // namespace OHOS::MMI

enum : int32_t {
    L410_ANCO_POINTER = 1,
    L410_ANCO_KEY = 2,
    L410_ANCO_FOCUS = 3,
    L410_ANCO_WINDOW = 4,
};

struct L410AncoInputEvent {
    int32_t type;
    int32_t targetWindowId;
    int32_t agentWindowId;
    int32_t action;
    int32_t x;
    int32_t y;
    int32_t code;
    int32_t source;
    double axisHorizontal;
    double axisVertical;
    int32_t width;
    int32_t height;
    float zOrder;
};

using L410AncoInputCallback = void (*)(const L410AncoInputEvent *, void *);

namespace {
std::mutex gMutex;
L410AncoInputCallback gCallback = nullptr;
void *gContext = nullptr;
using L410ImeReadyQuery = int32_t (*)(int32_t);
std::atomic<L410ImeReadyQuery> gImeReadyQuery {nullptr};
std::atomic<unsigned> gImeDispatched {0};
std::atomic<unsigned> gImeConsumed {0};
std::atomic<unsigned> gImeFallback {0};
std::atomic<unsigned> gImeError {0};

void Dispatch(const L410AncoInputEvent &event)
{
    L410AncoInputCallback callback;
    void *context;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        callback = gCallback;
        context = gContext;
    }
    if (callback != nullptr) {
        callback(&event, context);
    }
}

void DispatchPointer(const std::shared_ptr<OHOS::MMI::PointerEvent> &event)
{
    if (!event) return;
    OHOS::MMI::PointerEvent::PointerItem item;
    if (!event->GetPointerItem(event->GetPointerId(), item)) return;
    L410AncoInputEvent out {};
    out.type = L410_ANCO_POINTER;
    out.targetWindowId = event->GetTargetWindowId();
    out.agentWindowId = event->GetAgentWindowId();
    out.action = event->GetPointerAction();
    out.x = item.GetWindowX();
    out.y = item.GetWindowY();
    out.code = event->GetButtonId();
    out.source = event->GetSourceType();
    out.axisHorizontal = event->GetAxisValue(
        OHOS::MMI::PointerEvent::AXIS_TYPE_SCROLL_HORIZONTAL);
    out.axisVertical = event->GetAxisValue(
        OHOS::MMI::PointerEvent::AXIS_TYPE_SCROLL_VERTICAL);
    Dispatch(out);
}

void DispatchKey(const std::shared_ptr<OHOS::MMI::KeyEvent> &event)
{
    if (!event) return;
    L410AncoInputEvent out {};
    out.type = L410_ANCO_KEY;
    out.targetWindowId = event->GetTargetWindowId();
    out.agentWindowId = event->GetAgentWindowId();
    out.action = event->GetKeyAction();
    out.code = event->GetKeyCode();
    const auto ready = gImeReadyQuery.load(std::memory_order_acquire);
    if (ready != nullptr && ready(out.targetWindowId) != 0) {
        auto controller = OHOS::MiscServices::InputMethodController::GetInstance();
        if (controller != nullptr) {
            /* The IMF result is asynchronous.  A consumed pinyin key must
             * never also reach Wayland; an unconsumed shortcut must.  The
             * callback retains only the value fields, not a stale task id. */
            const int32_t result = controller->DispatchKeyEvent(event,
                [out](std::shared_ptr<OHOS::MMI::KeyEvent> &, bool consumed) {
                    if (consumed) {
                        gImeConsumed.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        gImeFallback.fetch_add(1, std::memory_order_relaxed);
                        Dispatch(out);
                    }
                });
            if (result == 0) {
                gImeDispatched.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            gImeError.fetch_add(1, std::memory_order_relaxed);
        }
    }
    Dispatch(out);
}

class L410AncoConsumer final : public OHOS::MMI::IAncoConsumer {
public:
    int32_t SyncInputEvent(std::shared_ptr<OHOS::MMI::PointerEvent> event) override
    {
        if (!event) return -1;
        DispatchPointer(event);
        return 0;
    }

    int32_t SyncInputEvent(std::shared_ptr<OHOS::MMI::KeyEvent> event) override
    {
        if (!event) return -1;
        DispatchKey(event);
        return 0;
    }

    int32_t UpdateWindowInfo(std::shared_ptr<OHOS::MMI::AncoWindows> table) override
    {
        if (!table) return -1;
        L410AncoInputEvent focus {};
        focus.type = L410_ANCO_FOCUS;
        focus.targetWindowId = table->focusWindowId;
        focus.action = static_cast<int32_t>(table->updateType);
        Dispatch(focus);
        for (const auto &window : table->windows) {
            L410AncoInputEvent out {};
            out.type = L410_ANCO_WINDOW;
            out.targetWindowId = window.id;
            out.action = static_cast<int32_t>(window.action);
            out.code = static_cast<int32_t>(window.flags);
            out.source = window.displayId;
            out.zOrder = window.zOrder;
            if (!window.defaultHotAreas.empty()) {
                out.x = window.defaultHotAreas[0].x;
                out.y = window.defaultHotAreas[0].y;
                out.width = window.defaultHotAreas[0].width;
                out.height = window.defaultHotAreas[0].height;
            }
            Dispatch(out);
        }
        return 0;
    }

    int32_t SyncKnuckleStatus(bool) override { return 0; }
    int32_t UpdateOneHandData(const OHOS::MMI::AncoOneHandData &) override { return 0; }
};

std::shared_ptr<L410AncoConsumer> gConsumer;
} // namespace

extern "C" int32_t l410_anco_input_start(
    L410AncoInputCallback callback, void *context)
{
    if (callback == nullptr) return -1;
    std::lock_guard<std::mutex> lock(gMutex);
    gCallback = callback;
    gContext = context;
    if (gConsumer) return 0;
    auto *manager = OHOS::MMI::InputManager::GetInstance();
    if (manager == nullptr) return -2;
    gConsumer = std::make_shared<L410AncoConsumer>();
    const int32_t result = manager->AncoAddConsumer(gConsumer);
    if (result != 0) {
        gConsumer.reset();
        return result;
    }
    return 0;
}

extern "C" int32_t l410_anco_input_stop(void)
{
    std::shared_ptr<L410AncoConsumer> consumer;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gCallback = nullptr;
        gContext = nullptr;
        consumer = gConsumer;
        gConsumer.reset();
    }
    if (!consumer) return 0;
    auto *manager = OHOS::MMI::InputManager::GetInstance();
    return manager == nullptr ? -2 : manager->AncoRemoveConsumer(consumer);
}

extern "C" void l410_anco_input_set_ime_ready_query(L410ImeReadyQuery query)
{
    gImeReadyQuery.store(query, std::memory_order_release);
}

extern "C" void l410_anco_input_ime_stats(uint32_t *dispatched,
    uint32_t *consumed, uint32_t *fallback, uint32_t *error)
{
    if (dispatched) *dispatched = gImeDispatched.load(std::memory_order_relaxed);
    if (consumed) *consumed = gImeConsumed.load(std::memory_order_relaxed);
    if (fallback) *fallback = gImeFallback.load(std::memory_order_relaxed);
    if (error) *error = gImeError.load(std::memory_order_relaxed);
}
