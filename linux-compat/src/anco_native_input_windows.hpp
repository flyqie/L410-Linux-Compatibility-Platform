// Linux semantic counterpart of the original AncoWindows host-window table.
// This table is separate from Linux app Window/Task/Wayland identities.
#pragma once
#include "anco_stock_input_events.hpp"
#include <mutex>
namespace AncoNativeInput {
class HostWindows final {
public:
    // Original OperateAncoWindows and IsFocusInContainer use the same lock.
    // A Linux snapshot may cross to the Wayland loop; never borrow live records.
    struct Snapshot {
        int focus=-1;
        bool container=false;
        bool key_excluded=false;
        bool last_down_in_container=false;
        std::vector<AncoOriginalInput::WindowFields> records;
    };
    void Apply(const AncoOriginalInput::WindowsFields&);
    Snapshot Read() const;
    void SetExcludedKeys(const std::vector<int>&);
    // Explicit ALREADY RESOLVED InputReader bounds only. This is not raw
    // callback4 data: original TouchInputMapper scales width by percentage.
    void SetOneHandBounds(int left,int top,int right,int bottom);
    int ContainerAt(int x,int y,int display) const;
    // Original CheckIsNeedDropTouchEvent also updates last-down state.
    bool NeedDropTouch(int x,int y,int display);
    bool LastDownInContainer() const;
    void SetLastDownInContainer(bool);
private:
    mutable std::mutex lock_;
    int focus_=-1;
    bool last_down_=false;
    int one_hand_[4]={};
    std::vector<int> excluded_keys_;
    int ContainerAtLocked(int x,int y,int display) const;
    std::vector<AncoOriginalInput::WindowFields> records_;
};
}
