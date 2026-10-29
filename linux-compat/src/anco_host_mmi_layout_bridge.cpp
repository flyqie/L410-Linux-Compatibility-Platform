/* Private SP68-package -> current L410 MMI C++ ABI conversion.
 * Original wire/API remains InputManager::UpdateWindowInfo(WindowGroupInfo).
 * Do not preload this globally or pass current-host records to this entry.
 *
 * Evidence: package copy ctor 0xc6cc and actionAdd 0xc530 advance0xd8;
 * byte-matched actual host libmmi-client IsValiadWindowAreas0xb4160 and
 * WindowInfo copy ctor0xd1014 advance0xd0. Five vectors retain their offsets.
 * The donor-only trailing int at0xd0 has no member in the host layout.
 */
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <vector>

namespace {
struct Rect { std::int32_t x, y; std::uint32_t width, height; };
struct HostWindow {
    unsigned char identity_and_area[32];
    std::vector<Rect> default_hot_areas;
    std::vector<Rect> pointer_hot_areas;
    unsigned char agent_flags_action_display_zorder[24];
    std::vector<std::int32_t> pointer_change_areas;
    std::vector<float> transform;
    unsigned char pixelmap_input_privacy_type[24];
    std::vector<HostWindow> ui_extension_windows;
    unsigned char trailing_host_state[8];
};
struct HostGroup {
    std::int32_t focus, display;
    std::vector<HostWindow> windows;
};
static_assert(sizeof(Rect) == 16);
static_assert(sizeof(HostWindow) == 208);
static_assert(offsetof(HostWindow, default_hot_areas) == 32);
static_assert(offsetof(HostWindow, pointer_hot_areas) == 56);
static_assert(offsetof(HostWindow, pointer_change_areas) == 104);
static_assert(offsetof(HostWindow, transform) == 128);
static_assert(offsetof(HostWindow, ui_extension_windows) == 176);
static_assert(offsetof(HostWindow, trailing_host_state) == 200);
static_assert(sizeof(HostGroup) == 32 && offsetof(HostGroup, windows) == 8);

// Read the original three-pointer vector ABI through bytes, without treating
// donor std::vector<WindowInfo> as a vector of our differently sized type.
struct VectorSpan { const unsigned char *begin, *end, *capacity; };
static_assert(sizeof(VectorSpan) == 24);
bool read_span(const unsigned char *record, std::size_t offset,
               std::size_t stride, VectorSpan &span, std::size_t &count)
{
    std::memcpy(&span, record+offset, sizeof(span));
    const auto first = reinterpret_cast<std::uintptr_t>(span.begin);
    const auto last = reinterpret_cast<std::uintptr_t>(span.end);
    const auto limit = reinterpret_cast<std::uintptr_t>(span.capacity);
    if (last < first || limit < last || (last-first)%stride) return false;
    count = (last-first)/stride;
    return !count || span.begin != nullptr;
}
template<class T>
bool copy_plain_vector(const unsigned char *record, std::size_t offset,
                       std::vector<T> &destination)
{
    VectorSpan span{};
    std::size_t count = 0;
    if (!read_span(record, offset, sizeof(T), span, count)) return false;
    destination.resize(count);
    if (count) std::memcpy(destination.data(), span.begin, count*sizeof(T));
    return true;
}
bool copy_windows(const unsigned char *record, std::size_t offset,
                  std::vector<HostWindow> &destination)
{
    VectorSpan span{};
    std::size_t count = 0;
    if (!read_span(record, offset, 216, span, count)) return false;
    destination.resize(count);
    for (std::size_t index=0; index<count; ++index) {
        const auto *source = span.begin + index*216;
        auto &target = destination[index];
        std::memcpy(target.identity_and_area, source, 32);
        std::memcpy(target.agent_flags_action_display_zorder, source+80, 24);
        std::memcpy(target.pixelmap_input_privacy_type, source+152, 24);
        std::memcpy(target.trailing_host_state, source+200, 8);
        if (!copy_plain_vector(source, 32, target.default_hot_areas) ||
            !copy_plain_vector(source, 56, target.pointer_hot_areas) ||
            !copy_plain_vector(source, 104, target.pointer_change_areas) ||
            !copy_plain_vector(source, 128, target.transform) ||
            !copy_windows(source, 176, target.ui_extension_windows)) return false;
    }
    return true;
}
}

extern "C" std::int32_t original_update_window_info(void *, const HostGroup &)
    __asm__("_ZN4OHOS3MMI12InputManager16UpdateWindowInfoERKNS0_15WindowGroupInfoE");

// Component-private entry for a donor call-site ABI port. No new host Binder,
// window, input or task protocol; original manager pointer and return survive.
extern "C" __attribute__((visibility("default")))
std::int32_t anco_mmi_update_window_info_6_1_to_6_0(void *manager, const void *donor_group)
{
    if (!manager || !donor_group) return -EINVAL;
    const auto *source = static_cast<const unsigned char *>(donor_group);
    HostGroup group{};
    std::memcpy(&group.focus, source, 4);
    std::memcpy(&group.display, source+4, 4);
    if (!copy_windows(source, 8, group.windows)) return -EINVAL;
    return original_update_window_info(manager, group);
}
