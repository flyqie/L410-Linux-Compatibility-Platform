// Diagnostic native binding to existing Rosen/AnCo window capabilities.
// No host protocol or policy changes. Never use a Task/guest Window as a
// Rosen ID. Real lookup, native property getters and native reference lifetime
// are preserved; capabilities must come from the same-round original BMS JSON.
#include "json.hpp"
#include "refbase.h"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include <vector>

namespace L410NativeWindowBinding {
// The original Window and property sptr returns are nontrivial 8-byte sret.
// Original Inc/DecStrongRef adjusts the virtual RefBase by vtable[-0x18].
// This owns original objects; it never constructs a substitute Window/vtable.
struct NativeRef {
    void *pointer = nullptr;
    OHOS::RefBase *base() const {
        const char *table;
        std::memcpy(&table,pointer,sizeof(table));
        ptrdiff_t offset;
        std::memcpy(&offset,table-0x18,sizeof(offset));
        return reinterpret_cast<OHOS::RefBase *>(static_cast<char *>(pointer)+offset);
    }
    NativeRef() = default;
    NativeRef(const NativeRef &r):pointer(r.pointer) { if(pointer)base()->IncStrongRef(this); }
    NativeRef(NativeRef &&r):pointer(r.pointer) {
        if(pointer) {base()->IncStrongRef(this);base()->DecStrongRef(&r);} r.pointer=nullptr;
    }
    ~NativeRef() { if(pointer)base()->DecStrongRef(this); }
};
static_assert(sizeof(NativeRef)==8);
static_assert(sizeof(std::vector<int32_t>)==24);
}
using L410NativeWindowBinding::NativeRef;
extern NativeRef original_window(uint32_t)
    asm("_ZN4OHOS5Rosen6Window15GetWindowWithIdEj");
extern NativeRef original_property(const void *)
    asm("_ZNK4OHOS5Rosen17WindowSessionImpl11GetPropertyEv");
extern std::string original_real_bundle(const void *)
    asm("_ZNK4OHOS5Rosen21WindowSessionProperty21GetAncoRealBundleNameEv");
extern "C" int32_t original_collaborator(const void *)
    asm("_ZNK4OHOS5Rosen21WindowSessionProperty19GetCollaboratorTypeEv");
extern "C" int32_t original_type(const void *)
    asm("_ZNK4OHOS5Rosen21WindowSessionProperty13GetWindowTypeEv");
extern "C" uint32_t original_support_type(const void *)
    asm("_ZNK4OHOS5Rosen21WindowSessionProperty24GetWindowModeSupportTypeEv");
extern "C" int32_t original_mode(const void *)
    asm("_ZNK4OHOS5Rosen21WindowSessionProperty13GetWindowModeEv");
extern "C" void original_modes(const void *,std::vector<int32_t> &)
    asm("_ZNK4OHOS5Rosen21WindowSessionProperty23GetSupportedWindowModesERNSt3__h6vectorINS_10AppExecFwk17SupportWindowModeENS2_9allocatorIS5_EEEE");
extern "C" int32_t original_set_modes(void *,const std::vector<int32_t> &,bool)
    asm("_ZN4OHOS5Rosen22WindowSceneSessionImpl23SetSupportedWindowModesERKNSt3__h6vectorINS_10AppExecFwk17SupportWindowModeENS2_9allocatorIS5_EEEEb");
struct NativeRect { int32_t x,y;uint32_t width,height; };
static_assert(sizeof(NativeRect)==16);
extern "C" NativeRect original_rect(const void *)
    asm("_ZNK4OHOS5Rosen17WindowSessionImpl7GetRectEv");
extern "C" int32_t original_resize(void *,uint32_t,uint32_t)
    asm("_ZN4OHOS5Rosen22WindowSceneSessionImpl6ResizeEjj");
extern "C" int32_t original_recover(void *)
    asm("_ZN4OHOS5Rosen22WindowSceneSessionImpl7RecoverEv");
extern "C" int32_t original_set_mode(void *,int32_t)
    asm("_ZN4OHOS5Rosen22WindowSceneSessionImpl13SetWindowModeENS0_10WindowModeE");

namespace {
nlohmann::json load(const char *text) {
    if(!text)return nullptr;
    // Caller passes the complete same-round original BMS decode, not a file
    // path assumed visible inside the original frontend's mount namespace.
    const size_t length=std::strlen(text);
    if(length>1024*1024)return nullptr;
    return nlohmann::json::parse(text,text+length,nullptr,false);
}
}
// Linux component-private diagnostic entry. Called only in the actual owning
// frontend process. Operation0 reads; 1 forwards BMS capabilities; 2 forwards
// the same real launch request after capabilities have actually been observed.
// Operation3 invokes the original Recover event for the real floating request;
// native SetWindowMode alone is not treated as desktop layout completion.
// Operation4 is a finite original Resize diagnostic on a recovered window.
// Only native getters are used; no private property/SessionInfo writes.
extern "C" __attribute__((visibility("default")))
int l410_anco_window_capabilities_diagnostic(uint32_t real_rosen_id,
    const char *actual_bms_json_text,const char *actual_ability,int operation,int requested_mode) {
    if(!real_rosen_id||!actual_ability||operation<0||operation>4)return -EINVAL;
    auto window=original_window(real_rosen_id);
    if(!window.pointer)return -ENOENT;
    // The current original WindowImpl lookup is null; a non-null result comes
    // from the original SceneBoard WindowSessionImpl map, not a reinterpret
    // cast of a different platform's Window object.
    auto property=original_property(window.pointer);
    if(!property.pointer)return -ENOENT;
    const auto bundle=original_real_bundle(property.pointer);
    const int collaborator=original_collaborator(property.pointer);
    const int type=original_type(property.pointer);
    std::vector<int32_t> before;original_modes(property.pointer,before);
    std::printf("ORIGINAL_WINDOW_CAPS_LOOKUP id=%u collaborator=%d type=%d mode=%d bundle=%s native_pointer=%p native_property=%p before=%s operation=%d\n",
        real_rosen_id,collaborator,type,original_mode(property.pointer),bundle.c_str(),
        window.pointer,property.pointer,nlohmann::json(before).dump().c_str(),operation);
    if(collaborator!=1||type!=1||bundle.empty())return -EPERM;
    const auto record=load(actual_bms_json_text);
    if(!record.is_object()||record.value("name",std::string())!=bundle||
       !record.contains("hapModuleInfos")||!record["hapModuleInfos"].is_array())return -ESTALE;
    const nlohmann::json *ability=nullptr;
    for(const auto &module:record["hapModuleInfos"]) {
        if(!module.is_object()||!module.contains("abilityInfos")||!module["abilityInfos"].is_array())continue;
        for(const auto &candidate:module["abilityInfos"])
            if(candidate.is_object()&&candidate.value("name",std::string())==actual_ability) {
                if(ability)return -EINVAL;ability=&candidate;
            }
    }
    if(!ability||!ability->contains("supportWindowMode")||!(*ability)["supportWindowMode"].is_array())return -EINVAL;
    std::vector<int32_t> modes;
    for(const auto &value:(*ability)["supportWindowMode"]) {
        if(!value.is_number_integer())return -EINVAL;
        const int m=value.get<int>();if(m<0||m>3)return -EINVAL;
        modes.push_back(m);
    }
    if(modes.empty()||modes.size()>4)return -EINVAL;
    std::printf("ORIGINAL_WINDOW_CAPS_SOURCE id=%u ability=%s bms_modes=%s\n",real_rosen_id,actual_ability,nlohmann::json(modes).dump().c_str());
    int result=0;
    if(operation==1)result=original_set_modes(window.pointer,modes,false);
    else if(operation==2 || operation==3 || operation==4) {
        uint32_t expected=0;
        for(int m:modes) {if(m==0)expected|=1;else if(m==1)expected|=12;else if(m==2)expected|=2;}
        // Native SetSupportedWindowModes changes the support bitmask and
        // notifies the actual session; it does NOT rewrite the initial list.
        if(original_support_type(property.pointer)!=expected)return -EAGAIN;
        const int needed=requested_mode==1?0:requested_mode==102?2:-1;
        bool supported=false;for(int m:modes)if(m==needed)supported=true;
        if(!supported)return -EINVAL;
        if(operation==3 && requested_mode!=102)return -EINVAL;
        if(operation==4) {
            if(requested_mode!=102||original_mode(property.pointer)!=102)return -EAGAIN;
            const auto rect=original_rect(window.pointer);
            if(rect.width<2||rect.height<2)return -EINVAL;
            // Finite diagnostic only: half the real current native rectangle.
            // This is not a new compositor size policy or default app size.
            std::printf("ORIGINAL_RESIZE_SOURCE id=%u current=%d,%d,%u,%u requested=%u,%u\n",
                real_rosen_id,rect.x,rect.y,rect.width,rect.height,rect.width/2,rect.height/2);
            result=original_resize(window.pointer,rect.width/2,rect.height/2);
        } else result=operation==3? original_recover(window.pointer):original_set_mode(window.pointer,requested_mode);
    }
    std::vector<int32_t> after;original_modes(property.pointer,after);
    std::printf("ORIGINAL_WINDOW_CAPS_RESULT id=%u operation=%d native_result=%d property_modes=%s property_mode=%d support_type=%u ui_confirmed=0\n",
        real_rosen_id,operation,result,nlohmann::json(after).dump().c_str(),original_mode(property.pointer),original_support_type(property.pointer));
    std::fflush(stdout);return result;
}

// Read-only, Linux component-private diagnostic. The JSON is a local tooling
// return value, not a host window protocol or a new renderer interface.
extern "C" int original_decor_height(void *,int32_t &)
    asm("_ZN4OHOS5Rosen17WindowSessionImpl14GetDecorHeightERi");
extern "C" int original_window_vpr(void *,float &)
    asm("_ZN4OHOS5Rosen17WindowSessionImpl20GetVirtualPixelRatioERf");
extern std::shared_ptr<void> original_ui(const void *)
    asm("_ZNK4OHOS5Rosen17WindowSessionImpl21GetUIContentSharedPtrEv");
extern "C" bool original_layout_full(const void *)
    asm("_ZNK4OHOS5Rosen21WindowSessionProperty18IsLayoutFullScreenEv");
extern "C" void original_app_paint(void *,NativeRect &)
    asm("_ZN4OHOS3Ace13UIContentImpl15GetAppPaintSizeERNS_5Rosen4RectE");
extern "C" void original_window_paint(void *,NativeRect &)
    asm("_ZN4OHOS3Ace13UIContentImpl18GetWindowPaintSizeERNS_5Rosen4RectE");
extern "C" void __cfi_slowpath(uint64_t,void *);
extern "C" __attribute__((visibility("default")))
const char *l410_anco_window_metrics_diagnostic(uint32_t id) {
    static std::string result;
    auto w=original_window(id);
    if(!w.pointer){result="{\"error\":\"missing native window\"}";return result.c_str();}
    auto p=original_property(w.pointer);
    if(!p.pointer){result="{\"error\":\"missing native property\"}";return result.c_str();}
    const auto bundle=original_real_bundle(p.pointer);
    if(original_collaborator(p.pointer)!=1||original_type(p.pointer)!=1) {
        result="{\"error\":\"not an original AnCo main window\"}";return result.c_str();
    }
    int32_t decor=0;float vpr=0;
    const int decor_result=original_decor_height(w.pointer,decor);
    const int vpr_result=original_window_vpr(w.pointer,vpr);
    const auto rect=original_rect(w.pointer);
    auto ui=original_ui(w.pointer);int32_t title=-1;
    if(ui) {
        void *table;std::memcpy(&table,ui.get(),sizeof(table));
        __cfi_slowpath(0xffb00eff0e8b6f55ULL,table);
        int32_t(*getter)(void *);
        std::memcpy(&getter,static_cast<const char *>(table)+0x330,sizeof(getter));
        title=getter(ui.get()); // exact original GetDecorHeight virtual call
    }
    // Original void getters can return early if their real container/stage is
    // absent. Keep an explicit unfilled rectangle; do not synthesize content
    // geometry from title height or treat normal return as layout readiness.
    NativeRect app_paint={-1,-1,0,0},window_paint={-1,-1,0,0};
    if(ui) {
        original_app_paint(ui.get(),app_paint);
        original_window_paint(ui.get(),window_paint);
    }
    result=nlohmann::json{{"native_id",id},{"bundle",bundle},
        {"mode",original_mode(p.pointer)},{"rect",{rect.x,rect.y,rect.width,rect.height}},
        {"vpr_result",vpr_result},{"window_vpr",vpr},{"decor_result",decor_result},
        {"decor_vp",decor},{"ui_present",bool(ui)},{"ui_title_px",title},
        {"app_paint_rect",{app_paint.x,app_paint.y,app_paint.width,app_paint.height}},
        {"window_paint_rect",{window_paint.x,window_paint.y,window_paint.width,window_paint.height}},
        {"app_paint_populated",app_paint.width!=0 && app_paint.height!=0},
        {"window_paint_populated",window_paint.width!=0 && window_paint.height!=0},
        {"layout_fullscreen",original_layout_full(p.pointer)}}.dump();
    return result.c_str();
}

extern "C" void original_ui_dump(void *,const std::vector<std::string> &,std::vector<std::string> &)
    asm("_ZN4OHOS3Ace13UIContentImpl8DumpInfoERKNSt3__h6vectorINS2_12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEENS7_IS9_EEEERSB_");
extern "C" __attribute__((visibility("default")))
const char *l410_anco_window_tree_diagnostic(uint32_t id) {
    static std::string result;
    auto w=original_window(id);
    if(!w.pointer){result="{\"error\":\"missing native window\"}";return result.c_str();}
    auto p=original_property(w.pointer);
    if(!p.pointer||original_collaborator(p.pointer)!=1||original_type(p.pointer)!=1) {
        result="{\"error\":\"not an AnCo main window\"}";return result.c_str();
    }
    auto ui=original_ui(w.pointer);
    if(!ui){result="{\"error\":\"missing native UI\"}";return result.c_str();}
    const std::vector<std::string> params={"-element"};
    std::vector<std::string> info;
    original_ui_dump(ui.get(),params,info);
    result=nlohmann::json{{"native_id",id},{"bundle",original_real_bundle(p.pointer)},
        {"original_dump_parameters",params},{"info",info}}.dump();
    return result.c_str();
}
