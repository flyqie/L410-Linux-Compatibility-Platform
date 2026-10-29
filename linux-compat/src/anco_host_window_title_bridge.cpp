// SP68 WindowSessionImpl title/hot-area mechanism, current-host ABI binding.
// Original bodies: libwm 241130,241fcc,2421a0,24fc58,24ff58,266a94,266c48.
// These are private ABI entry points, not a new Binder/window/input protocol.
// No activation/patching occurs in a constructor. The matching call sites and
// all field lifecycle hooks MUST be connected before this library is used.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "refbase.h"

namespace AncoHostVersionBinding {
struct Rect { int32_t x, y; uint32_t width, height; };
}
using Rect = AncoHostVersionBinding::Rect;
extern std::shared_ptr<void> original_ui(const void *)
    asm("_ZNK4OHOS5Rosen17WindowSessionImpl21GetUIContentSharedPtrEv");
extern int32_t original_get_decor_visible(void *, bool &)
    asm("_ZN4OHOS5Rosen17WindowSessionImpl15GetDecorVisibleERb");
extern "C" void anco_linux_window_geometry_forget_id(int32_t);
extern "C" void anco_linux_window_geometry_forget(void *);
extern "C" __attribute__((visibility("hidden")))
int32_t anco_linux_window_decor_visible(uint32_t,bool *);
extern "C" __attribute__((visibility("hidden")))
void anco_linux_window_defer_title_event(uint32_t,bool);
namespace {
static_assert(sizeof(Rect) == 16);
static_assert(sizeof(std::vector<Rect>) == 24);
static_assert(sizeof(std::shared_ptr<void>) == 16);
extern "C" Rect original_rect(const void *)
    asm("_ZNK4OHOS5Rosen17WindowSessionImpl7GetRectEv");
extern "C" void __cfi_slowpath(uint64_t, void *);

template<class T> T field(const void *object, size_t offset)
{
    T value;
    std::memcpy(&value, static_cast<const char *>(object)+offset, sizeof(value));
    return value;
}
template<class Fn> Fn method(const void *object, size_t offset)
{
    return field<Fn>(field<const void *>(object, 0), offset);
}
constexpr uint64_t window_type = 0xc02aea6b7db1f7fcULL;
constexpr uint64_t session_type = 0x32c99aa7b09f568bULL;
constexpr uint64_t ui_type = 0xffb00eff0e8b6f55ULL;
template<class Fn> Fn checked_method(const void *object, size_t offset, uint64_t type)
{
    __cfi_slowpath(type, field<void *>(object, 0));
    return method<Fn>(object, offset);
}
int32_t original_id(const void *window)
{ return checked_method<int32_t (*)(const void *)>(window, 0xdc8, session_type)(window); }
bool original_is_anco(const void *window)
{ return checked_method<bool (*)(const void *)>(window, 0xc80, session_type)(window); }
// AArch64 FCVTZS/FCVTZU semantics, including the original truncation and
// saturation. No arbitrary rounding, clamping or substituted integer geometry.
int32_t signed_position(float v)
{
    int32_t result;
    asm("fcvtzs %w0, %s1" : "=r"(result) : "w"(v));
    return result;
}
uint32_t unsigned_size(float v)
{
    uint32_t result;
    asm("fcvtzu %w0, %s1" : "=r"(result) : "w"(v));
    return result;
}
float signed_word(uint32_t word)
{
    int32_t value;
    std::memcpy(&value, &word, 4);
    return static_cast<float>(value);
}
int32_t wrapping_add(int32_t a, int32_t b)
{
    uint32_t value = static_cast<uint32_t>(a)+static_cast<uint32_t>(b);
    int32_t result;
    std::memcpy(&result, &value, 4);
    return result;
}

// Missing SP68 fields are kept by their REAL original object addresses. They
// cannot be written into guessed padding of smaller current-host objects.
// Original base ctor zeroes caption flag at21f7c0; property ctor zeroes height
// at393ac. Property CopyFrom copies height at2fc78; it is not in Marshalling.
struct FieldState {
    std::mutex mutex;
    std::map<const void *, bool> captions;
    std::map<int32_t, bool> title_visibility;
    std::map<const void *, int32_t> status_heights;
};
// Existing host objects can outlive this library's global destructor order.
// Keep private ABI state until process exit; actual object D2 bindings erase
// each field entry. This does not retain any Window/property object.
FieldState &fields()
{
    static FieldState *state = new FieldState;
    return *state;
}
void *property(const void *window) { return field<void *>(window, 0x260); }
int32_t status_height(const void *p)
{
    std::lock_guard<std::mutex> lock(fields().mutex);
    auto found = fields().status_heights.find(p);
    return found == fields().status_heights.end() ? 0 : found->second;
}
bool caption(const void *window)
{
    std::lock_guard<std::mutex> lock(fields().mutex);
    auto found = fields().captions.find(window);
    return found != fields().captions.end() && found->second;
}
bool title_visibility(const void *window, bool &visible)
{
    std::lock_guard<std::mutex> lock(fields().mutex);
    auto found = fields().title_visibility.find(original_id(window));
    if (found == fields().title_visibility.end()) return false;
    visible = found->second;
    return true;
}

// Original listener uses a virtual RefBase. The vbase displacement is stored
// at address-point-0x18, as in original Inc/DecStrongRef sequences. This owns
// original listener objects; it does not create an imitation listener vtable.
struct ListenerRef {
    void *pointer = nullptr;
    OHOS::RefBase *base() const
    {
        auto *table = field<const char *>(pointer, 0);
        ptrdiff_t displacement;
        std::memcpy(&displacement, table-0x18, sizeof(displacement));
        return reinterpret_cast<OHOS::RefBase *>(static_cast<char *>(pointer)+displacement);
    }
    explicit ListenerRef(void *p = nullptr) : pointer(p)
        { if (pointer) base()->IncStrongRef(this); }
    ListenerRef(const ListenerRef &r) : ListenerRef(r.pointer) {}
    ListenerRef(ListenerRef &&r) noexcept : pointer(r.pointer)
    {
        if (pointer) { base()->IncStrongRef(this); base()->DecStrongRef(&r); }
        r.pointer = nullptr;
    }
    ListenerRef &operator=(ListenerRef r) noexcept
    {
        if (pointer) base()->DecStrongRef(this);
        pointer = r.pointer;
        if (pointer) base()->IncStrongRef(this);
        return *this;
    }
    ~ListenerRef() { if (pointer) base()->DecStrongRef(this); }
};
static_assert(sizeof(ListenerRef) == 8);
struct ListenerState {
    std::mutex snapshot_mutex; // original TitleOrHotAreas mutex
    std::mutex registration_mutex; // original TitleChange mutex
    std::map<int32_t, std::vector<ListenerRef>> listeners;
};
ListenerState &listener_state()
{
    static ListenerState *state = new ListenerState;
    return *state;
}

bool free_window(const void *window)
{
    // SP68 IsInFreeWindowMode forwards to this exact original predicate.
    return checked_method<bool (*)(const void *)>(window, 0x6e0, session_type)(window);
}

std::vector<Rect> hot_areas(const void *window)
{
    std::vector<Rect> result;
    bool decor_visible = true;
    /* SetDecorVisible is the original WindowSession switch used when a Linux
     * client owns its frame.  In that state there is no native caption or
     * resize frame for MMI to exclude; the client-drawn controls must receive
     * the ordinary AnCo pointer stream. */
    if (!anco_linux_window_decor_visible(
            static_cast<uint32_t>(original_id(window)), &decor_visible) &&
        !title_visibility(window, decor_visible) &&
        original_get_decor_visible(const_cast<void *>(window), decor_visible) != 0)
        return result;
    if (!decor_visible) return result;
    auto ui = original_ui(window); // original shared-mutex/ref-count handling
    if (!ui) return result;
    int32_t mode = checked_method<int32_t (*)(const void *)>(window, 0x50, window_type)(window);
    float density = checked_method<float (*)(const void *)>(window, 0x8a8, session_type)(window);
    // Original241230/23c/248/254 reads W,H,X,Y in four getter invocations.
    uint32_t rect_width = original_rect(window).width;
    uint32_t rect_height = original_rect(window).height;
    int32_t rect_x = original_rect(window).x;
    int32_t rect_y = original_rect(window).y;
    int32_t title = checked_method<int32_t (*)(const void *)>(ui.get(), 0x330, ui_type)(ui.get());
    int32_t status = status_height(property(window));
    if (mode == 1 && !caption(window)) return result;
    float sx = field<float>(window, 0x470), sy = field<float>(window, 0x474);
    float width = signed_word(rect_width), height = signed_word(rect_height);
    float x = static_cast<float>(rect_x), y = static_cast<float>(rect_y);
    result.push_back({rect_x, wrapping_add(rect_y, status),
                      unsigned_size(sx*width), unsigned_size(sy*static_cast<float>(title))});
    if (mode == 1) return result;
    float border = density*24.0f;
    float edge = density*5.0f;
    float twice_border = border+border;
    float margin = border+edge;
    float outer_width = twice_border+width;
    result.push_back({signed_position(x-border*sx), signed_position(y-border*sy),
                      unsigned_size(outer_width*sx), unsigned_size(margin*sy)});
    // The original reloads BOTH scale fields before the left rectangle.
    sx = field<float>(window, 0x470); sy = field<float>(window, 0x474);
    result.push_back({signed_position(x-border*sx), signed_position(y-border*sy),
                      unsigned_size(margin*sx), unsigned_size((twice_border+height)*sy)});
    // Preserve the donor's asymmetric right-edge scaling; do not invent a fix.
    sx = field<float>(window, 0x470); sy = field<float>(window, 0x474);
    result.push_back({signed_position((sx*width+x)-edge), signed_position(y-border),
                      unsigned_size(margin), unsigned_size(sy*height+twice_border)});
    sx = field<float>(window, 0x470); sy = field<float>(window, 0x474);
    result.push_back({signed_position(x-border*sx), signed_position((height-edge)*sy+y),
                      unsigned_size(outer_width*sx), unsigned_size(margin*sy)});
    float corner = density*16.0f;
    sx = field<float>(window, 0x470); sy = field<float>(window, 0x474);
    result.push_back({rect_x, rect_y, unsigned_size(sx*corner), unsigned_size(sy*corner)});
    sx = field<float>(window, 0x470); sy = field<float>(window, 0x474);
    result.push_back({signed_position((width-corner)*sx+x), rect_y,
                      unsigned_size(sx*corner), unsigned_size(sy*corner)});
    sx = field<float>(window, 0x470); sy = field<float>(window, 0x474);
    result.push_back({rect_x, signed_position((height-corner)*sy+y),
                      unsigned_size(corner*sx), unsigned_size(corner*sy)});
    sx = field<float>(window, 0x470); sy = field<float>(window, 0x474);
    result.push_back({signed_position((width-corner)*sx+x), signed_position((height-corner)*sy+y),
                      unsigned_size(corner*sx), unsigned_size(corner*sy)});
    return result;
}

void notify(const void *window, bool shown, int32_t height)
{
    if (!original_is_anco(window) || !free_window(window)) return;
    std::vector<ListenerRef> snapshot;
    {
        std::lock_guard<std::mutex> lock(listener_state().snapshot_mutex);
        snapshot = listener_state().listeners[original_id(window)];
    }
    {
        std::lock_guard<std::mutex> lock(fields().mutex);
        fields().status_heights[property(window)] = height;
    }
    auto areas = hot_areas(window);
    for (const auto &r : snapshot) {
        if (!r.pointer) continue;
        void *table = field<void *>(r.pointer, 0);
        __cfi_slowpath(0x109f7712e8d7591eULL, table); // exact original listener type
        method<void (*)(void *, std::vector<Rect> &, bool)>(r.pointer, 0)(r.pointer, areas, shown);
    }
}
} // namespace

#define ENTRY extern "C" __attribute__((visibility("default")))
extern "C" __attribute__((visibility("hidden")))
void anco_window_set_title_visibility_6_1_to_6_0(const void *window, bool shown)
{
    std::lock_guard<std::mutex> lock(fields().mutex);
    fields().title_visibility[original_id(window)] = shown;
}
__attribute__((visibility("default")))
std::vector<Rect> anco_window_get_hot_areas_6_1_to_6_0(const void *window)
    asm("anco_window_get_hot_areas_6_1_to_6_0");
std::vector<Rect> anco_window_get_hot_areas_6_1_to_6_0(const void *window)
{ return hot_areas(window); }
ENTRY int32_t anco_window_register_title_6_1_to_6_0(const void *window, void *const *listener)
{
    // Donor register/unregister use windowTitleChangeListenerMutex_,
    // while snapshot/cleanup use TitleOrHotAreasListenerMutex_. Keep that
    // distinction; do not claim the donor uses a single lock for every path.
    if (!*listener) return 1001; // original WM_ERROR_NULLPTR
    bool inserted = false;
    {
        std::lock_guard<std::mutex> lock(listener_state().registration_mutex);
        auto &list = listener_state().listeners[original_id(window)];
        const auto found = std::find_if(list.begin(), list.end(),
            [&](const ListenerRef &r) { return r.pointer == *listener; });
        if (found == list.end()) {
            list.emplace_back(*listener);
            inserted = true;
        }
    }
    /* Linux metadata can reach the real Window before ShellAssistant has
     * registered its original title listener.  Calling that listener inline
     * here would re-enter ShellAssistant while it still owns its window-table
     * mutex.  Preserve the observed producer event and send it through the
     * existing Window UI executor after this registration returns. */
    bool shown = true;
    if (inserted && title_visibility(window, shown))
        anco_linux_window_defer_title_event(
            static_cast<uint32_t>(original_id(window)), shown);
    return 0;
}
ENTRY int32_t anco_window_unregister_title_6_1_to_6_0(const void *window, void *const *listener)
{
    std::lock_guard<std::mutex> lock(listener_state().registration_mutex);
    auto &list = listener_state().listeners[original_id(window)];
    if (!*listener) return 1001;
    ListenerRef retained(*listener); // original temporary strong reference
    list.erase(std::remove_if(list.begin(), list.end(),
        [&](const ListenerRef &r) { return r.pointer == retained.pointer; }), list.end());
    return 0;
}

// These are the necessary original producer/field-lifecycle bindings. They
// must be called at the original equivalent branch points, NOT from an invented
// timer or UI/event stream. No patcher/constructor currently activates them.
ENTRY void anco_window_title_position_6_1_to_6_0(const void *window, bool shown, int32_t position)
{
    // Original1ecf9c/1ecfa0 skips the virtual mode call when hidden/negative.
    bool fullscreen_caption = false;
    if (shown && position >= 0)
        fullscreen_caption = checked_method<int32_t (*)(const void *)>(
            window, 0x50, window_type)(window) == 1;
    {
        std::lock_guard<std::mutex> lock(fields().mutex);
        fields().captions[window] = fullscreen_caption;
        fields().title_visibility[original_id(window)] = shown;
    }
    notify(window, shown, position);
}
ENTRY void anco_window_title_scale_6_1_to_6_0(const void *window)
{ notify(window, true, 0); }
ENTRY void anco_window_title_size_6_1_to_6_0(const void *window, int32_t reason)
{ if (reason != 4 && reason != 22) notify(window, true, 0); }
ENTRY void anco_window_title_recover_6_1_to_6_0(const void *window)
{
    std::lock_guard<std::mutex> lock(fields().mutex);
    fields().captions[window] = false; // only original validated compatible-recover branch
}
ENTRY void anco_window_title_clear_6_1_to_6_0(int32_t persistent_id)
{
    anco_linux_window_geometry_forget_id(persistent_id);
    {
        std::lock_guard<std::mutex> lock(listener_state().snapshot_mutex);
        listener_state().listeners.erase(persistent_id);
    }
    std::lock_guard<std::mutex> lock(fields().mutex);
    fields().title_visibility.erase(persistent_id);
}
ENTRY void anco_window_title_destroy_6_1_to_6_0(const void *window)
{
    const int32_t persistent_id = original_id(window);
    anco_linux_window_geometry_forget(const_cast<void *>(window));
    std::lock_guard<std::mutex> lock(fields().mutex);
    fields().captions.erase(window);
    fields().title_visibility.erase(persistent_id);
}
ENTRY void anco_window_property_copy_6_1_to_6_0(const void *destination, const void *source)
{
    std::lock_guard<std::mutex> lock(fields().mutex);
    auto found = fields().status_heights.find(source);
    fields().status_heights[destination] = found == fields().status_heights.end() ? 0 : found->second;
}
ENTRY void anco_window_property_destroy_6_1_to_6_0(const void *p)
{
    std::lock_guard<std::mutex> lock(fields().mutex);
    fields().status_heights.erase(p);
}
