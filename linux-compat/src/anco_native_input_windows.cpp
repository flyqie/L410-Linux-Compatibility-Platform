#include "anco_native_input_windows.hpp"
#include <algorithm>
#include <cstdint>
namespace AncoNativeInput {
void HostWindows::Apply(const AncoOriginalInput::WindowsFields& fields)
{
    std::lock_guard<std::mutex> guard(lock_);
    focus_=fields.focus_window_id;
    // Original OperateAncoWindows af54 sets focus before clearing/rebuilding.
    if(fields.update_type==0)records_.clear();
    for(const auto& incoming:fields.windows){
        auto old=std::find_if(records_.begin(),records_.end(),
            [&](const auto& window){return window.window_id==incoming.window_id;});
        if(old!=records_.end())records_.erase(old);
        // Original UpdateAncoWindows a5a0: 1=ADD, 2=DEL, other=CHANGE.
        // ADD and CHANGE both insert; no action4-as-delete shortcut.
        if(incoming.action==2)continue;
        // Original AddAncoWindow a28c inserts before the first lower zOrder;
        // equal/NaN values follow the original strict comparison behavior.
        auto position=std::find_if(records_.begin(),records_.end(),
            [&](const auto& window){return window.z_order<incoming.z_order;});
        records_.insert(position,incoming);
    }
}
HostWindows::Snapshot HostWindows::Read() const
{
    std::lock_guard<std::mutex> guard(lock_);
    Snapshot result;
    result.focus=focus_;
    result.key_excluded=std::find(excluded_keys_.begin(),excluded_keys_.end(),focus_)!=excluded_keys_.end();
    result.last_down_in_container=last_down_;
    result.records=records_;
    // Original IsFocusInContainer a508 / IsContainerWindow b700.
    for(const auto& window:records_)
        if(window.window_id==focus_&&(window.flags&2U)){result.container=true;break;}
    return result;
}
namespace {
int transformed_component(float value)
{
    // Exact original IsInRectArea FRINTA then FCVTZS (ties away, saturating
    // conversion including NaN). Avoid C++ float-to-int undefined behavior.
    float rounded;
    int result;
    __asm__("frinta %s0, %s1" : "=w"(rounded) : "w"(value));
    __asm__("fcvtzs %w0, %s1" : "=r"(result) : "w"(rounded));
    return result;
}
#pragma clang fp contract(off)
bool in_rect(const AncoOriginalInput::WindowFields& window,int x,int y,
             const AncoOriginalInput::RectFields& area)
{
    if(window.transform.size()==9){
        const auto& m=window.transform;
        const float fx=static_cast<float>(x),fy=static_cast<float>(y);
        const float xx=m[0]*fx,xy=m[3]*fy;
        const float yx=m[1]*fx,yy=m[4]*fy;
        const float tx=m[6]+(xx+xy),ty=m[7]+(yx+yy);
        x=transformed_component(tx);y=transformed_component(ty);
    }
    // Original integer ADD W wraps; inclusive right/bottom comparisons.
    const auto right=static_cast<std::int32_t>(static_cast<std::uint32_t>(area.values[0])+static_cast<std::uint32_t>(area.values[2]));
    const auto bottom=static_cast<std::int32_t>(static_cast<std::uint32_t>(area.values[1])+static_cast<std::uint32_t>(area.values[3]));
    return x>=area.values[0]&&x<=right&&y>=area.values[1]&&y<=bottom;
}
bool in_areas(const AncoOriginalInput::WindowFields& window,int x,int y,
              const std::vector<AncoOriginalInput::RectFields>& areas)
{
    for(const auto& area:areas)if(in_rect(window,x,y,area))return true;
    return false;
}
}
void HostWindows::SetExcludedKeys(const std::vector<int>& ids)
{
    std::lock_guard<std::mutex> guard(lock_);excluded_keys_=ids;
}
void HostWindows::SetOneHandBounds(int left,int top,int right,int bottom)
{
    std::lock_guard<std::mutex> guard(lock_);
    one_hand_[0]=left;one_hand_[1]=top;one_hand_[2]=right;one_hand_[3]=bottom;
}
int HostWindows::ContainerAtLocked(int x,int y,int display) const
{
    for(const auto& window:records_){
        if(window.display_id!=display||(window.flags&1U)||!in_areas(window,x,y,window.hot_areas))continue;
        // First touched non-container blocks windows behind it. Excluded area
        // and one-hand failure block too; never search through to lower z.
        if(!(window.flags&2U)||in_areas(window,x,y,window.pointer_areas))return -1;
        if(one_hand_[1]>=1&&(x<one_hand_[0]||y<one_hand_[1]||x>one_hand_[2]||y>one_hand_[3]))return -1;
        return window.window_id;
    }
    return -1;
}
int HostWindows::ContainerAt(int x,int y,int display) const
{
    std::lock_guard<std::mutex> guard(lock_);return ContainerAtLocked(x,y,display);
}
bool HostWindows::NeedDropTouch(int x,int y,int display)
{
    std::lock_guard<std::mutex> guard(lock_);
    last_down_=ContainerAtLocked(x,y,display)!=-1;return !last_down_;
}
bool HostWindows::LastDownInContainer() const
{
    std::lock_guard<std::mutex> guard(lock_);return last_down_;
}
void HostWindows::SetLastDownInContainer(bool state)
{
    std::lock_guard<std::mutex> guard(lock_);last_down_=state;
}

}
