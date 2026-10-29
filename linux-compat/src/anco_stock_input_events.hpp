#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>

// Field order follows the original guest VirtKeyEvent/VirtPointerEvent
// Unmarshalling disassembly. Names are offsets until their semantics are
// confirmed with original Transform*Event and live targeted events.
namespace AncoOriginalInput {

struct KeyFields {
    long field_00 = 0;
    long field_08 = 0;
    int field_10 = 0;
    int field_14 = 0;
    int field_18 = 0;
    int field_1c = 0;
    unsigned int field_20 = 0;
};

struct PointerItem {
    long field_00 = 0;
    int field_08 = 0;
    int field_0c = 0;
    int field_10 = 0;
    int field_14 = 0;
    int field_18 = 0;
    int field_1c = 0;
};

struct PointerFields {
    long field_00 = 0;
    long field_08 = 0;
    int fields_10_to_28[7] = {};
    unsigned int field_2c = 0;
    float field_30 = 0;
    float field_34 = 0;
    int fields_38_to_48[5] = {};
    static constexpr size_t max_items = 32;
    size_t item_count = 0;
    PointerItem items[max_items] = {};
};

struct RectFields {
    int values[4] = {};
};

struct WindowFields {
    int display_id = 0;
    int window_id = 0;
    unsigned int action = 0;
    unsigned int flags = 0;
    float z_order = 0;
    std::vector<float> transform;
    std::vector<RectFields> hot_areas;
    // Historical member name retained for old diagnostic sources. Original
    // OnUpdateWindowInfo passes this SECOND rect vector to SetAncoExcludedAreas.
    // It is not a pointer hit-test region; hot_areas is the positive region.
    std::vector<RectFields> pointer_areas;
};

struct WindowsFields {
    // Original VirtWindows member0 is updateType, not a display ID.
    // ContainerIpcMgr passes it to AncoWindows::OperateAncoWindows.
    unsigned int update_type = 0;
    int focus_window_id = 0;
    std::vector<WindowFields> windows;
};

// Original callback4 VirtTpData. Its four wire values are interpreted by
// InputReader/TouchInputMapper, not already-computed AncoWindows bounds.
struct OneHandFields { int values[4] = {0,0,0,100}; };
struct DeviceFields {
    int id = -1;
    std::string field_08;
    unsigned short fields_20_to_26[4] = {};
    std::string field_28;
    std::string field_40;
};

template<class Parcel> bool ReadOneHand(Parcel& parcel,OneHandFields& out)
{
    for(int& value:out.values)if(!parcel.ReadInt32(value))return false;
    return true;
}
template<class Parcel> bool ReadExcludedKeys(Parcel& parcel,std::vector<int>& out)
{
    return parcel.ReadInt32Vector(&out);
}
template<class Parcel> bool ReadDevicesCallback(Parcel& parcel,std::vector<DeviceFields>& out)
{
    // The actual AncoInputChannel StubSyncDeviceInfoList reads UINT32 count,
    // maximum512. Do not call the distinct VirtAncoDeviceInfoList reader,
    // which reads UINT64 and is NOT the callback's real wire path.
    unsigned count=0;
    if(!parcel.ReadUint32(count)||count>512)return false;
    out.resize(count);
    for(auto& device:out){
        if(!parcel.ReadInt32(device.id)||!parcel.ReadString(device.field_08))return false;
        for(auto& value:device.fields_20_to_26)if(!parcel.ReadUint16(value))return false;
        if(!parcel.ReadString(device.field_28)||!parcel.ReadString(device.field_40))return false;
    }
    return true;
}

template <class Parcel>
bool ReadRects(Parcel &parcel, std::vector<RectFields> &out)
{
    unsigned long count = 0;
    if (!parcel.ReadUint64(count) || count > 64) return false;
    out.resize(static_cast<size_t>(count));
    for (auto &rect : out) {
        for (int &value : rect.values) {
            if (!parcel.ReadInt32(value)) return false;
        }
    }
    return true;
}

template <class Parcel>
bool ReadWindows(Parcel &parcel, WindowsFields &out)
{
    if (parcel.GetReadableBytes() > 256 * 1024 ||
        !parcel.ReadUint32(out.update_type) ||
        !parcel.ReadInt32(out.focus_window_id)) return false;
    unsigned long count = 0;
    if (!parcel.ReadUint64(count) || count > 64) return false;
    out.windows.resize(static_cast<size_t>(count));
    for (auto &window : out.windows) {
        if (!parcel.ReadInt32(window.display_id) ||
            !parcel.ReadInt32(window.window_id) ||
            !parcel.ReadUint32(window.action) ||
            !parcel.ReadUint32(window.flags) ||
            !parcel.ReadFloat(window.z_order) ||
            !parcel.ReadFloatVector(&window.transform) ||
            window.transform.size() > 16 ||
            !ReadRects(parcel, window.hot_areas) ||
            !ReadRects(parcel, window.pointer_areas)) return false;
    }
    return true;
}

template <class Parcel> bool ReadKey(Parcel &parcel, KeyFields &out)
{
    return parcel.ReadInt64(out.field_00) &&
           parcel.ReadInt64(out.field_08) &&
           parcel.ReadInt32(out.field_10) &&
           parcel.ReadInt32(out.field_14) &&
           parcel.ReadInt32(out.field_18) &&
           parcel.ReadInt32(out.field_1c) &&
           parcel.ReadUint32(out.field_20);
}

template <class Parcel> bool ReadPointer(Parcel &parcel, PointerFields &out)
{
    if (!parcel.ReadInt64(out.field_00) ||
        !parcel.ReadInt64(out.field_08)) return false;
    for (int &field : out.fields_10_to_28) {
        if (!parcel.ReadInt32(field)) return false;
    }
    if (!parcel.ReadUint32(out.field_2c) ||
        !parcel.ReadFloat(out.field_30) ||
        !parcel.ReadFloat(out.field_34)) return false;
    for (int &field : out.fields_38_to_48) {
        if (!parcel.ReadInt32(field)) return false;
    }
    unsigned long count = 0;
    if (!parcel.ReadUint64(count) || count > PointerFields::max_items)
        return false;
    out.item_count = static_cast<size_t>(count);
    for (size_t i = 0; i < out.item_count; ++i) {
        auto &item = out.items[i];
        if (!parcel.ReadInt64(item.field_00) ||
            !parcel.ReadInt32(item.field_08) ||
            !parcel.ReadInt32(item.field_0c) ||
            !parcel.ReadInt32(item.field_10) ||
            !parcel.ReadInt32(item.field_14) ||
            !parcel.ReadInt32(item.field_18) ||
            !parcel.ReadInt32(item.field_1c)) return false;
    }
    return true;
}

} // namespace AncoOriginalInput
