// Linux bridge-internal delivery of ORIGINAL native window properties.
// The AnCo/Binder/window interfaces and Task bounds callback remain unchanged.
// Runs at the original owning frontend's bind/viewport/destroy lifecycle.
#include "json.hpp"
#include "refbase.h"
#include <algorithm>
#include <cerrno>
#include <cfloat>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <unistd.h>

namespace L410Geometry {
struct NativeRef {
    void *pointer=nullptr;
    OHOS::RefBase *base() const {
        const char *table;std::memcpy(&table,pointer,8);
        ptrdiff_t delta;std::memcpy(&delta,table-0x18,8);
        return reinterpret_cast<OHOS::RefBase *>(static_cast<char *>(pointer)+delta);
    }
    NativeRef()=default;
    NativeRef(const NativeRef &r):pointer(r.pointer){if(pointer)base()->IncStrongRef(this);}
    NativeRef(NativeRef &&r):pointer(r.pointer){if(pointer){base()->IncStrongRef(this);base()->DecStrongRef(&r);}r.pointer=nullptr;}
    ~NativeRef(){if(pointer)base()->DecStrongRef(this);}
};
struct Rect {int32_t x,y;uint32_t width,height;};
static_assert(sizeof(NativeRef)==8 && sizeof(Rect)==16);
}
using L410Geometry::NativeRef;
using L410Geometry::Rect;
extern "C" void anco_linux_window_geometry_update(void *window);

namespace OHOS::Rosen {
enum class WindowStatus : uint32_t;
struct WindowLimits {
    uint32_t maxWidth=static_cast<uint32_t>(INT32_MAX);
    uint32_t maxHeight=static_cast<uint32_t>(INT32_MAX);
    uint32_t minWidth=1;
    uint32_t minHeight=1;
    float maxRatio=FLT_MAX;
    float minRatio=0.0f;
    float vpRatio=1.0f;
};
struct RectAnimationConfig {
    uint32_t duration=0;
    float x1=0,y1=0,x2=0,y2=0;
};
struct MoveConfiguration {
    uint64_t displayId=UINT64_MAX;
    RectAnimationConfig rectAnimationConfig;
};
class Window;
class IWindowStatusChangeListener : virtual public OHOS::RefBase {
public:
    virtual void OnWindowStatusChange(WindowStatus) {}
};
class IWindowWillCloseListener : virtual public OHOS::RefBase {
public:
    virtual void OnWindowWillClose(OHOS::sptr<Window>) {}
};
}
static_assert(sizeof(OHOS::Rosen::RectAnimationConfig)==20);
static_assert(sizeof(OHOS::Rosen::MoveConfiguration)==32);
static_assert(sizeof(OHOS::Rosen::WindowLimits)==28);

namespace OHOS::Media {
enum class PixelFormat : int32_t {
    UNKNOWN = 0,
    BGRA_8888 = 4,
};
enum class AlphaType : int32_t {
    IMAGE_ALPHA_TYPE_UNKNOWN = 0,
    IMAGE_ALPHA_TYPE_UNPREMUL = 3,
};
enum class ScaleMode : int32_t {
    FIT_TARGET_SIZE = 0,
};
enum class AllocatorType : int32_t {
    DEFAULT = 0,
    HEAP_ALLOC = 1,
};
enum class YuvConversion : int32_t {
    BT601 = 0,
};
struct Size { int32_t width=0,height=0; };
struct YUVConvertColorSpaceDetails {
    uint8_t srcRange=0,dstRange=0;
    YuvConversion srcYuvConversion=YuvConversion::BT601;
    YuvConversion dstYuvConversion=YuvConversion::BT601;
};
struct InitializationOptions {
    Size size;
    PixelFormat srcPixelFormat=PixelFormat::BGRA_8888;
    PixelFormat pixelFormat=PixelFormat::UNKNOWN;
    AlphaType alphaType=AlphaType::IMAGE_ALPHA_TYPE_UNKNOWN;
    ScaleMode scaleMode=ScaleMode::FIT_TARGET_SIZE;
    YUVConvertColorSpaceDetails convertColorSpace;
    int32_t srcRowStride=0;
    AllocatorType allocatorType=AllocatorType::DEFAULT;
    bool editable=false,useSourceIfMatch=false,useDMA=false;
};
class PixelMap {
public:
    virtual ~PixelMap();
    static std::unique_ptr<PixelMap> Create(const uint32_t *,uint32_t,
        const InitializationOptions &);
};
static_assert(sizeof(Size)==8);
static_assert(sizeof(YUVConvertColorSpaceDetails)==12);
static_assert(offsetof(InitializationOptions,convertColorSpace)==24);
static_assert(offsetof(InitializationOptions,srcRowStride)==36);
static_assert(offsetof(InitializationOptions,allocatorType)==40);
static_assert(sizeof(InitializationOptions)==48);
}

namespace OHOS::Ace {
class ContainerScope {
public:
    static int32_t CurrentId();
    static void UpdateCurrent(int32_t);
};
}
extern "C" void geometry_pixelmap_delete(OHOS::Media::PixelMap *)
    asm("_ZN4OHOS5Media8PixelMapD0Ev");

namespace {
void geometry_update_internal(void *window,bool queue_metadata);
void geometry_close_requested(void *window);
void geometry_status_changed(void *window,uint32_t status);
class GeometryStatusChangeListener final
    : public OHOS::Rosen::IWindowStatusChangeListener {
public:
    explicit GeometryStatusChangeListener(void *window):window_(window) {}
    void OnWindowStatusChange(OHOS::Rosen::WindowStatus status) override {
        std::printf("ORIGINAL_LINUX_WINDOW_STATUS_CHANGE status=%u original_listener=1\n",
            static_cast<uint32_t>(status));
        std::fflush(stdout);
        geometry_status_changed(window_,static_cast<uint32_t>(status));
    }
private:
    void *window_;
};
class GeometryWillCloseListener final
    : public OHOS::Rosen::IWindowWillCloseListener {
public:
    explicit GeometryWillCloseListener(void *window):window_(window) {}
    void OnWindowWillClose(OHOS::sptr<OHOS::Rosen::Window>) override {
        geometry_close_requested(window_);
    }
private:
    void *window_;
};
}

using StatusChangeListener = OHOS::Rosen::IWindowStatusChangeListener;
using StatusChangeListenerSptr = OHOS::sptr<StatusChangeListener>;
using WillCloseListener = OHOS::Rosen::IWindowWillCloseListener;
using WillCloseListenerSptr = OHOS::sptr<WillCloseListener>;
extern NativeRef geometry_window(uint32_t) asm("_ZN4OHOS5Rosen6Window15GetWindowWithIdEj");
extern NativeRef geometry_property(const void *) asm("_ZNK4OHOS5Rosen17WindowSessionImpl11GetPropertyEv");
extern std::string geometry_bundle(const void *) asm("_ZNK4OHOS5Rosen21WindowSessionProperty21GetAncoRealBundleNameEv");
extern const std::string &geometry_window_name(const void *) asm("_ZNK4OHOS5Rosen21WindowSessionProperty13GetWindowNameEv");
extern "C" int32_t geometry_persistent_id(const void *)
    asm("_ZNK4OHOS5Rosen21WindowSessionProperty15GetPersistentIdEv");
extern "C" int32_t geometry_collaborator(const void *) asm("_ZNK4OHOS5Rosen21WindowSessionProperty19GetCollaboratorTypeEv");
extern "C" int32_t geometry_type(const void *) asm("_ZNK4OHOS5Rosen21WindowSessionProperty13GetWindowTypeEv");
extern "C" int32_t geometry_mode(const void *) asm("_ZNK4OHOS5Rosen21WindowSessionProperty13GetWindowModeEv");
extern "C" int32_t geometry_set_modes(void *,const std::vector<int32_t> &,bool)
    asm("_ZN4OHOS5Rosen22WindowSceneSessionImpl23SetSupportedWindowModesERKNSt3__h6vectorINS_10AppExecFwk17SupportWindowModeENS2_9allocatorIS5_EEEEb");
extern "C" int32_t geometry_recover(void *)
    asm("_ZN4OHOS5Rosen22WindowSceneSessionImpl7RecoverEv");
extern "C" int32_t geometry_maximize_floating(void *)
    asm("_ZN4OHOS5Rosen22WindowSceneSessionImpl16MaximizeFloatingEv");
extern "C" int32_t geometry_minimize(void *)
    asm("_ZN4OHOS5Rosen22WindowSceneSessionImpl8MinimizeEv");
extern "C" int32_t geometry_set_fullscreen(void *,bool)
    asm("_ZN4OHOS5Rosen22WindowSceneSessionImpl13SetFullScreenEb");
extern "C" int32_t geometry_switch_free_multi_window(void *,bool)
    asm("_ZN4OHOS5Rosen22WindowSceneSessionImpl21SwitchFreeMultiWindowEb");
extern "C" int32_t geometry_get_status(void *,OHOS::Rosen::WindowStatus &)
    asm("_ZN4OHOS5Rosen22WindowSceneSessionImpl15GetWindowStatusERNS0_12WindowStatusE");
extern "C" int32_t geometry_request_focus(const void *)
    asm("_ZNK4OHOS5Rosen17WindowSessionImpl12RequestFocusEv");
extern "C" int32_t geometry_register_status_change(
    void *,const StatusChangeListenerSptr &)
    asm("_ZN4OHOS5Rosen17WindowSessionImpl34RegisterWindowStatusChangeListenerERKNS_4sptrINS0_27IWindowStatusChangeListenerEEE");
extern "C" int32_t geometry_register_will_close(
    void *,const WillCloseListenerSptr &)
    asm("_ZN4OHOS5Rosen17WindowSessionImpl32RegisterWindowWillCloseListenersERKNS_4sptrINS0_24IWindowWillCloseListenerEEE");
extern "C" int32_t geometry_unregister_will_close(
    void *,const WillCloseListenerSptr &)
    asm("_ZN4OHOS5Rosen17WindowSessionImpl34UnRegisterWindowWillCloseListenersERKNS_4sptrINS0_24IWindowWillCloseListenerEEE");
extern "C" int32_t geometry_resize(void *,uint32_t,uint32_t)
    asm("_ZN4OHOS5Rosen22WindowSceneSessionImpl6ResizeEjj");
extern "C" int32_t geometry_set_limits(void *,OHOS::Rosen::WindowLimits &,bool)
    asm("_ZN4OHOS5Rosen22WindowSceneSessionImpl15SetWindowLimitsERNS0_12WindowLimitsEb");
extern "C" int32_t geometry_move_to(void *,int32_t,int32_t,bool,
    OHOS::Rosen::MoveConfiguration)
    asm("_ZN4OHOS5Rosen22WindowSceneSessionImpl6MoveToEiibNS0_17MoveConfigurationE");
extern int32_t geometry_set_label(void *,const std::string &)
    asm("_ZN4OHOS5Rosen17WindowSessionImpl17SetAPPWindowLabelERKNSt3__h12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE");
extern "C" int32_t geometry_set_decor_visible(void *,bool)
    asm("_ZN4OHOS5Rosen17WindowSessionImpl15SetDecorVisibleEb");
extern "C" int32_t geometry_set_title_button_visible(void *,bool,bool,bool,bool)
    asm("_ZN4OHOS5Rosen17WindowSessionImpl21SetTitleButtonVisibleEbbbb");
extern "C" int32_t geometry_set_title_move_enabled(void *,bool)
    asm("_ZN4OHOS5Rosen17WindowSessionImpl25SetWindowTitleMoveEnabledEb");
extern "C" void anco_window_title_position_6_1_to_6_0(
    const void *,bool,int32_t);
extern "C" __attribute__((visibility("hidden")))
void anco_window_set_title_visibility_6_1_to_6_0(const void *,bool);
extern "C" int32_t geometry_start_move_window(void *)
    asm("_ZN4OHOS5Rosen22WindowSceneSessionImpl15StartMoveWindowEv");
extern int32_t geometry_set_icon(void *,const std::shared_ptr<OHOS::Media::PixelMap> &)
    asm("_ZN4OHOS5Rosen17WindowSessionImpl16SetAPPWindowIconERKNSt3__h10shared_ptrINS_5Media8PixelMapEEE");
extern "C" Rect geometry_rect(const void *) asm("_ZNK4OHOS5Rosen17WindowSessionImpl7GetRectEv");
extern "C" int32_t geometry_vpr(void *,float &) asm("_ZN4OHOS5Rosen17WindowSessionImpl20GetVirtualPixelRatioERf");
extern std::shared_ptr<void> geometry_ui(const void *)
    asm("_ZNK4OHOS5Rosen17WindowSessionImpl21GetUIContentSharedPtrEv");
extern "C" void geometry_paint(void *,Rect &) asm("_ZN4OHOS3Ace13UIContentImpl18GetWindowPaintSizeERNS_5Rosen4RectE");
extern void geometry_execute_ui(void *,std::function<void()>,const std::string &)
    asm("_ZN4OHOS3Ace13UIContentImpl13ExecuteUITaskENSt3__h8functionIFvvEEERKNS2_12basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE");
extern "C" const void *geometry_app_info() asm("_ZN4OHOS3Ace18AceApplicationInfo11GetInstanceEv");
extern "C" Rect geometry_request_rect(const void *)
    asm("_ZNK4OHOS5Rosen14WindowProperty14GetRequestRectEv");
extern "C" Rect geometry_window_rect(const void *)
    asm("_ZNK4OHOS5Rosen14WindowProperty13GetWindowRectEv");
extern "C" int32_t geometry_request_type(const void *)
    asm("_ZNK4OHOS5Rosen14WindowProperty13GetWindowTypeEv");

namespace {
struct Entry {
    Entry(uint32_t native_id_,int32_t task_,int32_t guest_window_)
        : native_id(native_id_),task(task_),guest_window(guest_window_) {}
    uint32_t native_id;int32_t task,guest_window;std::string path,last;
    uint32_t content_width=0,content_height=0;
    bool capabilities_in_progress=false,capabilities_applied=false;
    bool recover_attempted=false,resize_in_progress=false;
    StatusChangeListenerSptr status_change_listener;
    WillCloseListenerSptr will_close_listener;
    uint64_t close_request_serial=0;
    std::string applied_title;
    bool applied_decor_known=false,applied_decor_visible=true;
    bool limits_in_progress=false,applied_limits_known=false;
    OHOS::Rosen::WindowLimits applied_limits;
    uint64_t applied_move_request_serial=0;
    uint64_t applied_window_request_serial=0;
    uint64_t applied_resize_request_serial=0;
    uint32_t window_status=0;
    bool window_status_known=false;
    std::shared_ptr<OHOS::Media::PixelMap> applied_icon;
};
struct PendingLayout {
    int32_t guest_window,activity,task;uint32_t color_mode;Rect request;
};
struct State {
    std::mutex lock;std::map<const void *,Entry> windows;
    std::map<int32_t,PendingLayout> pending_layouts;
    std::map<const void *,uint32_t> metadata_tasks;
    std::map<const void *,int32_t> ui_instances;
};
State &state(){static State *s=new State;return *s;}
uint64_t frontend_start_ticks() {
    static const uint64_t value=[]() {
        FILE *file=std::fopen("/proc/self/stat","re");
        if(!file)return uint64_t{0};
        char line[4096];const bool read=std::fgets(line,sizeof(line),file)!=nullptr;
        std::fclose(file);if(!read)return uint64_t{0};
        char *cursor=std::strrchr(line,')');
        if(!cursor || cursor[1]!=' ')return uint64_t{0};
        cursor+=2;
        for(int field=3;field<22;++field){
            cursor=std::strchr(cursor,' ');
            if(!cursor)return uint64_t{0};
            while(*cursor==' ')++cursor;
        }
        errno=0;char *end=nullptr;
        const unsigned long long ticks=std::strtoull(cursor,&end,10);
        if(errno || !end || (*end!=' ' && *end!='\n' && *end!='\0') || !ticks ||
           ticks>uint64_t(INT64_MAX))return uint64_t{0};
        return uint64_t(ticks);
    }();
    return value;
}
std::string directory() {
    // Current 6.0 inline GetDataFileDirPath: GetInstance returns singleton
    // 0x437a720; original DumpLog reads its std::string at0x437a860 (+0x140).
    // Actual original object/inline field only; never a guessed global address
    // or a replacement application context. The linked binding is host60 ABI.
    const auto *app=static_cast<const char *>(geometry_app_info());
    if(!app)return {};
    const auto &path=*reinterpret_cast<const std::string *>(app+0x140);
    if(path.empty() || path[0]!='/')return {};
    return path+"/linux-compat-window-properties";
}
std::string title_path(int32_t task) {
    const auto root=directory();
    if(root.empty() || task<0)return {};
    return root+"/linux-title/title-"+std::to_string(getpid())+"-"+
        std::to_string(task)+".json";
}
bool valid_linux_bundle(const std::string &bundle);
struct LinuxMetadata {
    std::string bundle,title;
    std::vector<int32_t> modes;
    bool decor_visible=true;
    uint64_t move_request_serial=0;
    uint64_t window_request_serial=0;
    uint32_t window_request=0;
    uint64_t resize_request_serial=0;
    Rect resize_request={0,0,0,0};
    uint32_t min_width=0,min_height=0,max_width=0,max_height=0;
};
bool read_linux_metadata_file(const std::string &path,int32_t task,
    int32_t frontend_pid,LinuxMetadata &metadata) {
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return false;
    std::string text;char bytes[2048];
    for(;;){
        const ssize_t n=read(fd,bytes,sizeof(bytes));
        if(n<0&&errno==EINTR)continue;
        if(n<=0)break;
        if(text.size()+size_t(n)>4096){text.clear();break;}
        text.append(bytes,size_t(n));
    }
    close(fd);if(text.empty())return false;
    const auto value=nlohmann::json::parse(text,nullptr,false);
    if(value.is_discarded() || !value.is_object() ||
       value.value("frontendPid",-1)!=frontend_pid ||
       value.value("taskId",-1)!=task ||
       !value.contains("supportWindowModes") ||
       !value["supportWindowModes"].is_array())return false;
    if(!value.contains("decorVisible") || !value["decorVisible"].is_boolean())return false;
    if(!value.contains("contentSizeLimits") ||
       !value["contentSizeLimits"].is_object())return false;
    const auto &limits=value["contentSizeLimits"];
    for(const char *name:{"minWidth","minHeight","maxWidth","maxHeight"})
        if(!limits.contains(name) || !limits[name].is_number_unsigned())return false;
    const uint64_t min_width=limits["minWidth"].get<uint64_t>();
    const uint64_t min_height=limits["minHeight"].get<uint64_t>();
    const uint64_t max_width=limits["maxWidth"].get<uint64_t>();
    const uint64_t max_height=limits["maxHeight"].get<uint64_t>();
    if(min_width>16384 || min_height>16384 || max_width>16384 ||
       max_height>16384 || (max_width && min_width>max_width) ||
       (max_height && min_height>max_height))return false;
    if(!value.contains("moveRequestSerial") ||
       (!value["moveRequestSerial"].is_number_integer() &&
        !value["moveRequestSerial"].is_number_unsigned()))return false;
    uint64_t move_request_serial=0;
    if(value["moveRequestSerial"].is_number_unsigned()) {
        move_request_serial=value["moveRequestSerial"].get<uint64_t>();
    } else {
        const int64_t serial=value["moveRequestSerial"].get<int64_t>();
        if(serial<0)return false;
        move_request_serial=static_cast<uint64_t>(serial);
    }
    if(move_request_serial>uint64_t(INT64_MAX))return false;
    if(!value.contains("windowRequestSerial") ||
       (!value["windowRequestSerial"].is_number_integer() &&
        !value["windowRequestSerial"].is_number_unsigned()) ||
       !value.contains("windowRequest") || !value["windowRequest"].is_number_integer())return false;
    uint64_t window_request_serial=0;
    if(value["windowRequestSerial"].is_number_unsigned()) {
        window_request_serial=value["windowRequestSerial"].get<uint64_t>();
    } else {
        const int64_t serial=value["windowRequestSerial"].get<int64_t>();
        if(serial<0)return false;
        window_request_serial=static_cast<uint64_t>(serial);
    }
    const int window_request=value["windowRequest"].get<int>();
    if(window_request_serial>uint64_t(INT64_MAX) || window_request<0 || window_request>6 ||
       ((!window_request_serial)!=(window_request==0)))return false;
    if(!value.contains("resizeRequestSerial") ||
       (!value["resizeRequestSerial"].is_number_integer() &&
        !value["resizeRequestSerial"].is_number_unsigned()) ||
       !value.contains("resizeRequest") || !value["resizeRequest"].is_object())return false;
    uint64_t resize_request_serial=0;
    if(value["resizeRequestSerial"].is_number_unsigned()) {
        resize_request_serial=value["resizeRequestSerial"].get<uint64_t>();
    } else {
        const int64_t serial=value["resizeRequestSerial"].get<int64_t>();
        if(serial<0)return false;
        resize_request_serial=static_cast<uint64_t>(serial);
    }
    const auto &resize=value["resizeRequest"];
    if(!resize.contains("left") || !resize["left"].is_number_integer() ||
       !resize.contains("top") || !resize["top"].is_number_integer() ||
       !resize.contains("width") || !resize["width"].is_number_unsigned() ||
       !resize.contains("height") || !resize["height"].is_number_unsigned())return false;
    const int64_t resize_left=resize["left"].get<int64_t>();
    const int64_t resize_top=resize["top"].get<int64_t>();
    const uint64_t resize_width=resize["width"].get<uint64_t>();
    const uint64_t resize_height=resize["height"].get<uint64_t>();
    if(resize_request_serial>uint64_t(INT64_MAX) || resize_left<INT32_MIN ||
       resize_left>INT32_MAX || resize_top<INT32_MIN || resize_top>INT32_MAX ||
       resize_width>16384 || resize_height>16384 ||
       ((!resize_request_serial)!=(resize_width==0 && resize_height==0)) ||
       ((resize_width==0)!=(resize_height==0)))return false;
    LinuxMetadata parsed{value.value("bundleName",std::string()),
        value.value("title",std::string()),{},value["decorVisible"].get<bool>(),
        move_request_serial,window_request_serial,static_cast<uint32_t>(window_request),
        resize_request_serial,{static_cast<int32_t>(resize_left),
        static_cast<int32_t>(resize_top),static_cast<uint32_t>(resize_width),
        static_cast<uint32_t>(resize_height)},static_cast<uint32_t>(min_width),
        static_cast<uint32_t>(min_height),static_cast<uint32_t>(max_width),
        static_cast<uint32_t>(max_height)};
    if(!valid_linux_bundle(parsed.bundle) || parsed.title.size()>1024 ||
       value["supportWindowModes"].empty() ||
       value["supportWindowModes"].size()>4)return false;
    uint32_t seen=0;
    for(const auto &item:value["supportWindowModes"]) {
        if(!item.is_number_integer())return false;
        const int mode=item.get<int>();
        if(mode<0 || mode>3 || (seen&(1u<<mode)))return false;
        seen|=1u<<mode;parsed.modes.push_back(mode);
    }
    metadata=std::move(parsed);return true;
}
bool same_linux_metadata(const LinuxMetadata &a,const LinuxMetadata &b) {
    return a.bundle==b.bundle && a.title==b.title && a.modes==b.modes &&
        a.decor_visible==b.decor_visible &&
        a.move_request_serial==b.move_request_serial &&
        a.window_request_serial==b.window_request_serial &&
        a.window_request==b.window_request &&
        a.resize_request_serial==b.resize_request_serial &&
        !std::memcmp(&a.resize_request,&b.resize_request,sizeof(Rect)) &&
        a.min_width==b.min_width && a.min_height==b.min_height &&
        a.max_width==b.max_width && a.max_height==b.max_height;
}
bool parse_metadata_filename(const std::string &name,int32_t task,
    int32_t &frontend_pid) {
    static constexpr char prefix[]="title-";
    const auto suffix="-"+std::to_string(task)+".json";
    if(name.size()<=sizeof(prefix)-1+suffix.size() ||
       name.compare(0,sizeof(prefix)-1,prefix) ||
       name.compare(name.size()-suffix.size(),suffix.size(),suffix))return false;
    const size_t begin=sizeof(prefix)-1;
    const size_t end=name.size()-suffix.size();
    int64_t value=0;
    for(size_t i=begin;i<end;++i) {
        const unsigned char c=name[i];
        if(c<'0'||c>'9')return false;
        value=value*10+(c-'0');
        if(value>INT32_MAX)return false;
    }
    if(value<=0)return false;
    frontend_pid=static_cast<int32_t>(value);return true;
}
bool read_linux_metadata(int32_t task,LinuxMetadata &metadata) {
    const auto exact=title_path(task);if(exact.empty())return false;
    if(read_linux_metadata_file(exact,task,getpid(),metadata))return true;

    /* The original launcher creates each later Task in a fresh frontend
     * process.  The Linux Task exists first, so its trusted metadata is
     * published under the already-running launcher PID before the new
     * frontend has produced its original Window property.  Consume that
     * same-task record at bind time; once the frontend-specific record exists,
     * the exact path above remains authoritative.  The per-run directory is
     * created empty and is writable only by the Linux runtime UID. */
    const auto slash=exact.rfind('/');if(slash==std::string::npos)return false;
    const auto directory_path=exact.substr(0,slash);
    DIR *directory=opendir(directory_path.c_str());if(!directory)return false;
    bool found=false,ambiguous=false;int32_t source_pid=-1;
    LinuxMetadata selected;
    while(const auto *item=readdir(directory)) {
        const std::string name=item->d_name;int32_t candidate_pid=-1;
        if(!parse_metadata_filename(name,task,candidate_pid) ||
           candidate_pid==getpid())continue;
        LinuxMetadata candidate;
        if(!read_linux_metadata_file(directory_path+"/"+name,task,
            candidate_pid,candidate))continue;
        if(!found) {
            selected=std::move(candidate);source_pid=candidate_pid;found=true;
        } else if(!same_linux_metadata(selected,candidate)) {
            ambiguous=true;break;
        }
    }
    closedir(directory);
    if(!found || ambiguous)return false;
    metadata=std::move(selected);
    std::printf("ORIGINAL_LINUX_WINDOW_METADATA_SOURCE task=%d source_pid=%d frontend_pid=%d same_task_fallback=1\n",
        task,source_pid,getpid());
    std::fflush(stdout);
    return true;
}
void apply_original_title(void *window,int32_t task) {
    LinuxMetadata metadata;if(!read_linux_metadata(task,metadata) ||
       metadata.title.empty())return;
    auto property=geometry_property(window);
    if(!property.pointer || geometry_bundle(property.pointer)!=metadata.bundle)return;
    {
        std::lock_guard<std::mutex> guard(state().lock);
        auto it=state().windows.find(window);
        if(it==state().windows.end() || it->second.task!=task ||
           it->second.applied_title==metadata.title)return;
    }
    const int result=geometry_set_label(window,metadata.title);
    if(result==0) {
        std::lock_guard<std::mutex> guard(state().lock);
        auto it=state().windows.find(window);
        if(it!=state().windows.end() && it->second.task==task)
            it->second.applied_title=metadata.title;
    }
    std::printf("ORIGINAL_LINUX_WINDOW_TITLE task=%d title=%s result=%d original_set_app_window_label=1\n",
        task,metadata.title.c_str(),result);
    std::fflush(stdout);
}

void apply_original_decoration(void *window,int32_t task) {
    LinuxMetadata metadata;if(!read_linux_metadata(task,metadata))return;
    auto property=geometry_property(window);
    if(!property.pointer || geometry_bundle(property.pointer)!=metadata.bundle)return;
    {
        std::lock_guard<std::mutex> guard(state().lock);
        auto it=state().windows.find(window);
        if(it==state().windows.end() || it->second.task!=task ||
           (it->second.applied_decor_known &&
            it->second.applied_decor_visible==metadata.decor_visible))return;
    }
    /* Match QOhosPlatformWindow/QOhosView's native frameless sequence.  Qt
     * clears maximize/minimize/close flags first; its three-argument JS API
     * maps maximize to the native split button as well.  The existing Rosen
     * WindowSession then owns visibility, painting and hit testing for all
     * four container controls. */
    const bool buttons_visible=metadata.decor_visible;
    anco_window_set_title_visibility_6_1_to_6_0(window,metadata.decor_visible);
    const int buttons=geometry_set_title_button_visible(window,buttons_visible,
        buttons_visible,buttons_visible,buttons_visible);
    const int decor=geometry_set_decor_visible(window,metadata.decor_visible);
    const int move=geometry_set_title_move_enabled(window,metadata.decor_visible);
    /* SP68 publishes every title-position visibility transition through
     * WindowSessionImpl::NotifyTitleChange(bool,int).  SetDecorVisible exists
     * on the 6.0 host but predates that AnCo producer, so complete the same
     * notification here after the original decoration switch succeeds.  The
     * title bridge derives the current native hot areas and therefore emits an
     * empty exclusion vector for a client-decorated Linux window. */
    if(buttons==0 || decor==0 || move==0)
        anco_window_title_position_6_1_to_6_0(window,metadata.decor_visible,0);
    if(buttons==0 && decor==0 && move==0) {
        std::lock_guard<std::mutex> guard(state().lock);
        auto it=state().windows.find(window);
        if(it!=state().windows.end() && it->second.task==task) {
            it->second.applied_decor_known=true;
            it->second.applied_decor_visible=metadata.decor_visible;
        }
    }
    std::printf("ORIGINAL_LINUX_WINDOW_DECORATION task=%d visible=%d buttons_result=%d decor_result=%d title_move_result=%d original_window_session=1\n",
        task,metadata.decor_visible,buttons,decor,move);
    std::fflush(stdout);
}

void apply_original_move_request(void *window,int32_t task) {
    LinuxMetadata metadata;if(!read_linux_metadata(task,metadata) ||
       !metadata.move_request_serial)return;
    auto property=geometry_property(window);
    if(!property.pointer || geometry_bundle(property.pointer)!=metadata.bundle)return;
    {
        std::lock_guard<std::mutex> guard(state().lock);
        auto it=state().windows.find(window);
        if(it==state().windows.end() || it->second.task!=task ||
           metadata.move_request_serial<=it->second.applied_move_request_serial)return;
    }
    const int result=geometry_start_move_window(window);
    if(result==0) {
        std::lock_guard<std::mutex> guard(state().lock);
        auto it=state().windows.find(window);
        if(it!=state().windows.end() && it->second.task==task)
            it->second.applied_move_request_serial=metadata.move_request_serial;
    }
    std::printf("ORIGINAL_LINUX_WINDOW_START_MOVE task=%d serial=%llu result=%d original_window_scene_session=1\n",
        task,static_cast<unsigned long long>(metadata.move_request_serial),result);
    std::fflush(stdout);
}

void apply_original_window_request(void *window,int32_t task) {
    LinuxMetadata metadata;if(!read_linux_metadata(task,metadata) ||
       !metadata.window_request_serial || !metadata.window_request)return;
    auto property=geometry_property(window);
    if(!property.pointer || geometry_bundle(property.pointer)!=metadata.bundle)return;
    {
        std::lock_guard<std::mutex> guard(state().lock);
        auto it=state().windows.find(window);
        if(it==state().windows.end() || it->second.task!=task ||
           metadata.window_request_serial<=it->second.applied_window_request_serial)return;
    }
    int result=0,secondary=INT32_MIN;
    switch(metadata.window_request) {
    case 1: result=geometry_maximize_floating(window);break;
    case 2: result=geometry_recover(window);break;
    case 3: result=geometry_minimize(window);break;
    case 4: result=geometry_set_fullscreen(window,true);break;
    case 5:
        result=geometry_set_fullscreen(window,false);
        if(result==0)secondary=geometry_recover(window);
        break;
    case 6: result=geometry_request_focus(window);break;
    default:return;
    }
    if(result==0 && (secondary==INT32_MIN || secondary==0)) {
        std::lock_guard<std::mutex> guard(state().lock);
        auto it=state().windows.find(window);
        if(it!=state().windows.end() && it->second.task==task)
            it->second.applied_window_request_serial=metadata.window_request_serial;
    }
    std::printf("ORIGINAL_LINUX_WINDOW_REQUEST task=%d serial=%llu request=%u result=%d secondary=%d original_window_scene_session=1\n",
        task,static_cast<unsigned long long>(metadata.window_request_serial),
        metadata.window_request,result,secondary);
    std::fflush(stdout);
}

void apply_original_resize_request(void *window,int32_t task) {
    LinuxMetadata metadata;if(!read_linux_metadata(task,metadata) ||
       !metadata.resize_request_serial || !metadata.resize_request.width ||
       !metadata.resize_request.height)return;
    auto property=geometry_property(window);
    if(!property.pointer || geometry_bundle(property.pointer)!=metadata.bundle)return;
    {
        std::lock_guard<std::mutex> guard(state().lock);
        auto it=state().windows.find(window);
        if(it==state().windows.end() || it->second.task!=task ||
           metadata.resize_request_serial<=it->second.applied_resize_request_serial)return;
    }
    const Rect current=geometry_rect(window);
    int resize_result=0,move_result=0;
    if(current.width!=metadata.resize_request.width ||
       current.height!=metadata.resize_request.height)
        resize_result=geometry_resize(window,metadata.resize_request.width,
            metadata.resize_request.height);
    if(resize_result==0 && (current.x!=metadata.resize_request.x ||
       current.y!=metadata.resize_request.y))
        move_result=geometry_move_to(window,metadata.resize_request.x,
            metadata.resize_request.y,false,OHOS::Rosen::MoveConfiguration{});
    if(resize_result==0 && move_result==0) {
        std::lock_guard<std::mutex> guard(state().lock);
        auto it=state().windows.find(window);
        if(it!=state().windows.end() && it->second.task==task)
            it->second.applied_resize_request_serial=metadata.resize_request_serial;
    }
    std::printf("ORIGINAL_LINUX_WINDOW_RESIZE task=%d serial=%llu target=%d,%d,%u,%u resize_result=%d move_result=%d original_window_scene_session=1\n",
        task,static_cast<unsigned long long>(metadata.resize_request_serial),
        metadata.resize_request.x,metadata.resize_request.y,
        metadata.resize_request.width,metadata.resize_request.height,
        resize_result,move_result);
    std::fflush(stdout);
}

bool valid_linux_bundle(const std::string &bundle) {
    static constexpr char prefix[]="org.l410.linux.";
    if(bundle.size()<=sizeof(prefix)-1 || bundle.size()>160 ||
       bundle.compare(0,sizeof(prefix)-1,prefix))return false;
    for(const unsigned char c:bundle) {
        if((c>='a'&&c<='z') || (c>='A'&&c<='Z') ||
           (c>='0'&&c<='9') || c=='.' || c=='_' || c=='-')continue;
        return false;
    }
    return true;
}
std::string icon_path(const std::string &bundle) {
    const auto root=directory();
    if(root.empty() || !valid_linux_bundle(bundle))return {};
    return root+"/linux-title/icons/"+bundle+".bgra";
}
void apply_original_icon(void *window,int32_t task) {
    auto property=geometry_property(window);
    if(!property.pointer)return;
    const auto bundle=geometry_bundle(property.pointer);
    const auto path=icon_path(bundle);if(path.empty())return;
    {
        std::lock_guard<std::mutex> guard(state().lock);
        auto it=state().windows.find(window);
        if(it==state().windows.end() || it->second.task!=task ||
           it->second.applied_icon)return;
    }
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return;
    struct stat info={};
    if(fstat(fd,&info)!=0 || !S_ISREG(info.st_mode) || info.st_size!=64*64*4) {
        close(fd);return;
    }
    std::vector<uint32_t> colors(64*64);
    size_t done=0;auto *bytes=reinterpret_cast<uint8_t *>(colors.data());
    while(done<colors.size()*sizeof(uint32_t)) {
        const ssize_t n=read(fd,bytes+done,colors.size()*sizeof(uint32_t)-done);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)break;
        done+=size_t(n);
    }
    uint8_t extra=0;
    const ssize_t trailing=done==colors.size()*sizeof(uint32_t)?read(fd,&extra,1):-1;
    close(fd);
    if(done!=colors.size()*sizeof(uint32_t) || trailing!=0)return;
    OHOS::Media::InitializationOptions options;
    options.size={64,64};
    options.srcPixelFormat=OHOS::Media::PixelFormat::BGRA_8888;
    options.pixelFormat=OHOS::Media::PixelFormat::BGRA_8888;
    options.alphaType=OHOS::Media::AlphaType::IMAGE_ALPHA_TYPE_UNPREMUL;
    options.srcRowStride=64*4;
    options.allocatorType=OHOS::Media::AllocatorType::HEAP_ALLOC;
    auto unique=OHOS::Media::PixelMap::Create(colors.data(),
        uint32_t(colors.size()*sizeof(uint32_t)),options);
    if(!unique)return;
    // The object is allocated by the original image framework. Keep its exact
    // deleting destructor as the shared_ptr deleter instead of compiling a
    // replacement PixelMap class or invoking a cross-DSO virtual delete.
    std::shared_ptr<OHOS::Media::PixelMap> icon(
        unique.release(),geometry_pixelmap_delete);
    const int result=geometry_set_icon(window,icon);
    if(result==0) {
        std::lock_guard<std::mutex> guard(state().lock);
        auto it=state().windows.find(window);
        if(it!=state().windows.end() && it->second.task==task)
            it->second.applied_icon=icon;
    }
    std::printf("ORIGINAL_LINUX_WINDOW_ICON task=%d bundle=%s result=%d original_set_app_window_icon=1 pixelmap_create=1\n",
        task,bundle.c_str(),result);
    std::fflush(stdout);
}

void apply_original_capabilities(void *window,int32_t task);
void apply_original_limits(void *window,int32_t task);
void apply_pending_layout(int32_t task);
void apply_original_title_for_task(int32_t task) {
    void *window=nullptr;
    bool content_stable=false;
    bool ambiguous=false;
    {
        std::lock_guard<std::mutex> guard(state().lock);
        for(const auto &[candidate,entry]:state().windows) {
            if(entry.task!=task)continue;
            auto property=geometry_property(candidate);
            if(!property.pointer || geometry_collaborator(property.pointer)!=1 ||
               geometry_type(property.pointer)!=1)continue;
            const auto bundle=geometry_bundle(property.pointer);
            static constexpr char prefix[]="org.l410.linux.";
            if(bundle.compare(0,sizeof(prefix)-1,prefix))continue;
            if(window){ambiguous=true;break;}
            window=const_cast<void *>(candidate);
            /* Title/icon updates synchronously re-enter Ace app-bar layout.
             * Keep their proven post-Resize gate.  Decoration state is a
             * WindowSession property and must be applied as soon as the sole
             * real root is known, otherwise a self-decorated X11 client is
             * displayed underneath the native caption bar during that wait. */
            content_stable=entry.content_width && entry.content_height &&
                !entry.resize_in_progress;
        }
    }
    if(window && !ambiguous) {
        apply_original_decoration(window,task);
        apply_original_limits(window,task);
        apply_original_move_request(window,task);
        apply_original_window_request(window,task);
        apply_original_resize_request(window,task);
        if(content_stable) {
            apply_original_title(window,task);
            apply_original_icon(window,task);
        }
    }
}

bool same_window_limits(const OHOS::Rosen::WindowLimits &a,
    const OHOS::Rosen::WindowLimits &b) {
    return a.maxWidth==b.maxWidth && a.maxHeight==b.maxHeight &&
        a.minWidth==b.minWidth && a.minHeight==b.minHeight &&
        a.maxRatio==b.maxRatio && a.minRatio==b.minRatio &&
        a.vpRatio==b.vpRatio;
}

void apply_original_limits(void *window,int32_t task) {
    LinuxMetadata metadata;if(!read_linux_metadata(task,metadata))return;
    auto property=geometry_property(window);
    if(!property.pointer || geometry_bundle(property.pointer)!=metadata.bundle)return;
    const Rect outer=geometry_rect(window);Rect paint={-1,-1,0,0};
    auto ui=geometry_ui(window);if(!ui)return;
    geometry_paint(ui.get(),paint);
    float vpr=0.0f;
    if(geometry_vpr(window,vpr)!=0 || !std::isfinite(vpr) || vpr<=0 ||
       paint.x<0 || paint.y<0 || !paint.width || !paint.height ||
       uint64_t(paint.x)+paint.width>outer.width ||
       uint64_t(paint.y)+paint.height>outer.height)return;
    const uint64_t frame_width=uint64_t(outer.width)-paint.width;
    const uint64_t frame_height=uint64_t(outer.height)-paint.height;
    auto project=[vpr](uint32_t logical,uint64_t frame,bool maximum)->uint32_t {
        if(maximum && !logical)return static_cast<uint32_t>(INT32_MAX);
        const double content=double(logical?logical:1)*vpr;
        if(!std::isfinite(content) || content<0)return 0;
        const uint64_t rounded=uint64_t(std::lround(content));
        const uint64_t value=rounded+frame;
        return value>uint64_t(INT32_MAX)?0:static_cast<uint32_t>(value);
    };
    OHOS::Rosen::WindowLimits desired;
    desired.minWidth=project(metadata.min_width,frame_width,false);
    desired.minHeight=project(metadata.min_height,frame_height,false);
    desired.maxWidth=project(metadata.max_width,frame_width,true);
    desired.maxHeight=project(metadata.max_height,frame_height,true);
    if(!desired.minWidth || !desired.minHeight || !desired.maxWidth ||
       !desired.maxHeight || desired.minWidth>desired.maxWidth ||
       desired.minHeight>desired.maxHeight)return;
    {
        std::lock_guard<std::mutex> guard(state().lock);
        auto it=state().windows.find(window);
        if(it==state().windows.end() || it->second.task!=task ||
           it->second.limits_in_progress ||
           (it->second.applied_limits_known &&
            same_window_limits(it->second.applied_limits,desired)))return;
        it->second.limits_in_progress=true;
    }
    const auto requested=desired;
    const int result=geometry_set_limits(window,desired,true);
    {
        std::lock_guard<std::mutex> guard(state().lock);
        auto it=state().windows.find(window);
        if(it!=state().windows.end() && it->second.task==task){
            it->second.limits_in_progress=false;
            if(result==0){
                it->second.applied_limits_known=true;
                it->second.applied_limits=requested;
            }
        }
    }
    std::printf("ORIGINAL_LINUX_WINDOW_LIMITS task=%d logical=%u,%u,%u,%u frame=%llu,%llu requested=%u,%u,%u,%u effective=%u,%u,%u,%u vpr=%.9g result=%d original_set_window_limits=1 forcible=1\n",
        task,metadata.min_width,metadata.min_height,metadata.max_width,
        metadata.max_height,static_cast<unsigned long long>(frame_width),
        static_cast<unsigned long long>(frame_height),requested.minWidth,
        requested.minHeight,requested.maxWidth,requested.maxHeight,
        desired.minWidth,desired.minHeight,desired.maxWidth,desired.maxHeight,
        vpr,result);
    std::fflush(stdout);
}
struct OriginalRoot {
    void *window=nullptr;
    uint32_t native_id=0;
};
OriginalRoot find_original_root_for_task(int32_t task) {
    OriginalRoot root;
    std::lock_guard<std::mutex> guard(state().lock);
    for(const auto &[candidate,entry]:state().windows) {
        if(entry.task!=task)continue;
        auto property=geometry_property(candidate);
        if(!property.pointer || geometry_collaborator(property.pointer)!=1 ||
           geometry_type(property.pointer)!=1)continue;
        const auto bundle=geometry_bundle(property.pointer);
        static constexpr char prefix[]="org.l410.linux.";
        if(bundle.compare(0,sizeof(prefix)-1,prefix))continue;
        if(root.window)return {}; // Multiple native roots are ambiguous.
        root.window=const_cast<void *>(candidate);
        root.native_id=entry.native_id;
    }
    return root;
}
void apply_original_capabilities_for_task(int32_t task) {
    const auto root=find_original_root_for_task(task);
    if(!root.window || !root.native_id)return;
    auto original=geometry_window(root.native_id);
    if(original.pointer!=root.window)return;
    apply_original_capabilities(original.pointer,task);
    apply_pending_layout(task);
}
void queue_original_title_for_task(void *window,int32_t task) {
    auto ui=geometry_ui(window);if(!ui)return;
    int32_t resolved=-1;uint32_t native_id=0;
    {
        std::lock_guard<std::mutex> guard(state().lock);
        const auto entry=state().windows.find(window);
        if(entry==state().windows.end() || entry->second.task!=task)return;
        const auto instance=state().ui_instances.find(window);
        if(instance==state().ui_instances.end() || instance->second<0)return;
        resolved=instance->second;
        native_id=entry->second.native_id;
        if(state().metadata_tasks.count(window))return;
        state().metadata_tasks.emplace(window,native_id);
    }
    const auto previous=OHOS::Ace::ContainerScope::CurrentId();
    OHOS::Ace::ContainerScope::UpdateCurrent(resolved);
    geometry_execute_ui(ui.get(),[window,native_id,task]() {
        bool active=false;
        {
            std::lock_guard<std::mutex> guard(state().lock);
            const auto entry=state().windows.find(window);
            const auto pending=state().metadata_tasks.find(window);
            active=entry!=state().windows.end() && entry->second.task==task &&
                entry->second.native_id==native_id &&
                pending!=state().metadata_tasks.end() && pending->second==native_id;
        }
        if(active) {
            apply_original_capabilities_for_task(task);
            apply_pending_layout(task);
            apply_original_title_for_task(task);
        }
        {
            std::lock_guard<std::mutex> guard(state().lock);
            const auto pending=state().metadata_tasks.find(window);
            if(pending!=state().metadata_tasks.end() && pending->second==native_id)
                state().metadata_tasks.erase(pending);
        }
    },
        "L410AncoApplyLinuxWindowMetadata");
    OHOS::Ace::ContainerScope::UpdateCurrent(previous);
    std::printf("ORIGINAL_LINUX_WINDOW_METADATA_UI_TASK task=%d previous_instance=%d resolved_instance=%d original_ace_execute_ui_task=1\n",
        task,previous,resolved);
    std::fflush(stdout);
}
void queue_original_title_for_task_id(int32_t task) {
    const auto root=find_original_root_for_task(task);
    if(!root.window || !root.native_id)return;
    /* Hold the original WindowSession strongly while obtaining UIContent and
     * queuing its unchanged Ace task.  The queued task resolves the live root
     * again before applying metadata. */
    auto original=geometry_window(root.native_id);
    if(original.pointer!=root.window)return;
    queue_original_title_for_task(original.pointer,task);
}
void ensure_linux_metadata_watch() {
    static std::once_flag once;
    const auto root=directory();
    if(root.empty())return;
    const auto watched=root+"/linux-title";
    std::call_once(once,[watched]() {
        std::thread([watched]() {
            for(;;) {
                const int fd=inotify_init1(IN_CLOEXEC|IN_NONBLOCK);
                if(fd<0) {
                    usleep(100000);continue;
                }
                const int watch=inotify_add_watch(fd,watched.c_str(),
                    IN_MOVED_TO|IN_CLOSE_WRITE|IN_DELETE_SELF|IN_MOVE_SELF);
                if(watch<0) {
                    close(fd);usleep(100000);continue;
                }
                std::printf("ORIGINAL_LINUX_WINDOW_METADATA_WATCH directory=%s original_ui_queue=1\n",
                    watched.c_str());
                std::fflush(stdout);
                bool restart=false;
                while(!restart) {
                    pollfd descriptor{fd,POLLIN,0};
                    const int ready=poll(&descriptor,1,-1);
                    if(ready<0) {
                        if(errno==EINTR)continue;
                        break;
                    }
                    if(!(descriptor.revents&POLLIN))break;
                    alignas(inotify_event) char bytes[4096];
                    const ssize_t count=read(fd,bytes,sizeof(bytes));
                    if(count<0) {
                        if(errno==EINTR||errno==EAGAIN)continue;
                        break;
                    }
                    bool metadata_changed=false;
                    for(ssize_t offset=0;offset<count;) {
                        const auto *event=reinterpret_cast<const inotify_event *>(bytes+offset);
                        if(event->mask&(IN_DELETE_SELF|IN_MOVE_SELF|IN_IGNORED))restart=true;
                        if(event->len && (event->mask&(IN_MOVED_TO|IN_CLOSE_WRITE))) {
                            const std::string name(event->name);
                            if(name.size()>5 && name.compare(name.size()-5,5,".json")==0)
                                metadata_changed=true;
                        }
                        offset+=sizeof(inotify_event)+event->len;
                    }
                    if(!metadata_changed)continue;
                    std::set<int32_t> tasks;
                    {
                        std::lock_guard<std::mutex> guard(state().lock);
                        for(const auto &[window,entry]:state().windows)
                            if(entry.task>=0)tasks.insert(entry.task);
                    }
                    for(const int32_t task:tasks)queue_original_title_for_task_id(task);
                }
                inotify_rm_watch(fd,watch);
                close(fd);
                usleep(100000);
            }
        }).detach();
    });
}
nlohmann::json rect_json(const Rect &r) {
    return {{"left",r.x},{"top",r.y},{"width",r.width},{"height",r.height}};
}
int32_t original_system_guest_window(const void *property) {
    if(!property)return -1;
    const auto name=geometry_window_name(property);
    static constexpr char prefix[]="shell_sys_win_";
    if(name.compare(0,sizeof(prefix)-1,prefix))return -1;
    const auto digits=name.substr(sizeof(prefix)-1);
    if(digits.empty() || (digits.size()>1 && digits[0]=='0'))return -1;
    int64_t value=0;
    for(const unsigned char c:digits) {
        if(c<'0'||c>'9')return -1;
        value=value*10+(c-'0');
        if(value>INT32_MAX)return -1;
    }
    return value>0?int32_t(value):-1;
}
void publish_locked(void *window,Entry &entry) {
    auto property=geometry_property(window);
    if(!property.pointer)return;
    const uint64_t start_ticks=frontend_start_ticks();
    if(!start_ticks || entry.close_request_serial>uint64_t(INT64_MAX))return;
    std::string text;
    if(entry.guest_window>0) {
        if(entry.task!=-1 || original_system_guest_window(property.pointer)!=entry.guest_window)
            return;
        text=nlohmann::json{{"taskId",-1},{"guestWindowId",entry.guest_window},
            {"frontendPid",getpid()},{"frontendStartTicks",start_ticks},
            {"closeRequestSerial",entry.close_request_serial},
            {"windowProperties",{{"id",entry.native_id}}}}.dump()+"\n";
    } else {
        if(geometry_collaborator(property.pointer)!=1 ||
           geometry_type(property.pointer)!=1)return;
        const int mode=geometry_mode(property.pointer);
        if(mode!=1 && mode!=102)return;
        const auto bundle=geometry_bundle(property.pointer);
        if(bundle.empty())return;
        auto ui=geometry_ui(window);if(!ui)return;
        const auto outer=geometry_rect(window);
        Rect paint={-1,-1,0,0};float vpr=0;
        geometry_paint(ui.get(),paint);
        if(geometry_vpr(window,vpr)!=0 || !std::isfinite(vpr) || vpr<=0 ||
           paint.x<0 || paint.y<0 || !paint.width || !paint.height ||
           uint64_t(paint.x)+paint.width>outer.width ||
           uint64_t(paint.y)+paint.height>outer.height)return;
        if(entry.resize_in_progress && entry.content_width==paint.width &&
           entry.content_height==paint.height)entry.resize_in_progress=false;
        if(!entry.window_status_known)return;
        text=nlohmann::json{{"taskId",entry.task},{"frontendPid",getpid()},
            {"frontendStartTicks",start_ticks},
            {"closeRequestSerial",entry.close_request_serial},
            {"bundleName",bundle},{"windowMode",mode},
            {"windowStatus",entry.window_status},{"vpr",vpr},
            {"windowProperties",{{"id",entry.native_id},{"windowRect",rect_json(outer)},
              {"drawableRect",rect_json(paint)}}}}.dump()+"\n";
    }
    if(text==entry.last)return;
    const auto dir=directory();if(dir.empty())return;
    if(mkdir(dir.c_str(),0755)!=0 && errno!=EEXIST)return;
    if(entry.path.empty())entry.path=dir+"/window-"+std::to_string(getpid())+"-"+std::to_string(entry.native_id)+".json";
    const auto temp=entry.path+".tmp";
    int fd=open(temp.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0644);
    if(fd<0)return;
    size_t done=0;
    while(done<text.size()) {
        const ssize_t n=write(fd,text.data()+done,text.size()-done);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)break;
        done+=size_t(n);
    }
    const int closed=close(fd);
    if(done!=text.size() || closed!=0 || rename(temp.c_str(),entry.path.c_str())!=0) {
        unlink(temp.c_str());return;
    }
    entry.last=text;
}
void geometry_status_changed(void *window,uint32_t status) {
    {
        std::lock_guard<std::mutex> guard(state().lock);
        auto it=state().windows.find(window);
        if(it!=state().windows.end() && status>=1 && status<=5) {
            it->second.window_status=status;
            it->second.window_status_known=true;
        }
    }
    geometry_update_internal(window,true);
}
void geometry_close_requested(void *window) {
    std::lock_guard<std::mutex> guard(state().lock);
    auto it=state().windows.find(window);
    if(it==state().windows.end() ||
       it->second.close_request_serial>=uint64_t(INT64_MAX))return;
    ++it->second.close_request_serial;
    publish_locked(window,it->second);
    std::printf("ORIGINAL_LINUX_WINDOW_CLOSE_REQUEST task=%d guest=%d native=%u serial=%llu original_listener=1 direct_close=0\n",
        it->second.task,it->second.guest_window,it->second.native_id,
        static_cast<unsigned long long>(it->second.close_request_serial));
    std::fflush(stdout);
}
void apply_original_capabilities(void *window,int32_t task) {
    LinuxMetadata metadata;if(!read_linux_metadata(task,metadata))return;
    auto property=geometry_property(window);
    if(!property.pointer || geometry_collaborator(property.pointer)!=1 ||
       geometry_type(property.pointer)!=1 ||
       geometry_bundle(property.pointer)!=metadata.bundle)return;
    {
        std::lock_guard<std::mutex> guard(state().lock);
        auto it=state().windows.find(window);
        if(it==state().windows.end() || it->second.task!=task ||
           it->second.capabilities_applied ||
           it->second.capabilities_in_progress)return;
        it->second.capabilities_in_progress=true;
    }
    const int supported=geometry_set_modes(window,metadata.modes,false);
    const bool floating=std::find(metadata.modes.begin(),metadata.modes.end(),2)
        !=metadata.modes.end();
    /* SceneBoard pre-creates every Linux AnCo root in floating mode.  A warm
     * ShellAssistant process can still construct the fresh client window with
     * its mode-1 fallback after that server-side transition has already been
     * delivered.  Re-enter the unchanged Rosen Recover path once, after the
     * original support modes are accepted, so WMS replays the existing
     * floating session state to this new client window. */
    const bool should_recover=!supported && floating &&
        geometry_mode(property.pointer)==1;
    const int recovered=should_recover?geometry_recover(window):INT32_MIN;
    {
        std::lock_guard<std::mutex> guard(state().lock);
        auto it=state().windows.find(window);
        if(it!=state().windows.end() && it->second.task==task) {
            it->second.capabilities_in_progress=false;
            it->second.capabilities_applied=!supported;
            it->second.recover_attempted=should_recover;
        }
    }
    std::printf("ORIGINAL_LINUX_WINDOW_CAPABILITIES task=%d modes=%s native_result_modes=%d native_result_recover=%d requested_mode=102 hardcoded_modes=0 original_set_supported_modes=1 original_recover=%d\n",
        task,nlohmann::json(metadata.modes).dump().c_str(),supported,recovered,
        should_recover?1:0);
    std::fflush(stdout);
}
bool same_pending(const PendingLayout &a,const PendingLayout &b) {
    return a.guest_window==b.guest_window && a.activity==b.activity &&
        a.task==b.task && a.color_mode==b.color_mode &&
        a.request.x==b.request.x && a.request.y==b.request.y &&
        a.request.width==b.request.width && a.request.height==b.request.height;
}
void apply_pending_layout(int32_t task) {
    PendingLayout pending={};void *window=nullptr;uint32_t native_id=0;
    {
        std::lock_guard<std::mutex> guard(state().lock);
        auto pending_it=state().pending_layouts.find(task);
        if(pending_it==state().pending_layouts.end())return;
        pending=pending_it->second;
        for(auto &[candidate,entry]:state().windows) {
            if(entry.task!=task)continue;
            auto property=geometry_property(candidate);
            if(!property.pointer || geometry_collaborator(property.pointer)!=1 ||
               geometry_type(property.pointer)!=1)continue;
            const int mode=geometry_mode(property.pointer);
            if(mode!=102)continue;
            const auto bundle=geometry_bundle(property.pointer);
            static constexpr char prefix[]="org.l410.linux.";
            if(bundle.compare(0,sizeof(prefix)-1,prefix))continue;
            if(window){window=nullptr;break;} // Multiple native roots are ambiguous.
            window=const_cast<void *>(candidate);
            native_id=entry.native_id;
        }
        if(!window)return;
        auto entry_it=state().windows.find(window);
        if(entry_it==state().windows.end() || entry_it->second.resize_in_progress)return;
        if(entry_it->second.content_width==pending.request.width &&
           entry_it->second.content_height==pending.request.height) {
            state().pending_layouts.erase(pending_it);return;
        }
        entry_it->second.resize_in_progress=true;
    }
    apply_original_limits(window,task);
    const Rect outer=geometry_rect(window);Rect paint={-1,-1,0,0};
    auto ui=geometry_ui(window);if(ui)geometry_paint(ui.get(),paint);
    int result=-EINVAL;uint32_t target_width=0,target_height=0;
    bool resize_requested=false;
    if(ui && paint.x>=0 && paint.y>=0 && paint.width && paint.height &&
       uint64_t(paint.x)+paint.width<=outer.width &&
       uint64_t(paint.y)+paint.height<=outer.height) {
        const uint64_t width=uint64_t(pending.request.width)+outer.width-paint.width;
        const uint64_t height=uint64_t(pending.request.height)+outer.height-paint.height;
        if(width>0 && height>0 && width<=16384 && height<=16384) {
            target_width=uint32_t(width);target_height=uint32_t(height);
            if(paint.width==pending.request.width &&
               paint.height==pending.request.height)result=0;
            else {
                resize_requested=true;
                result=geometry_resize(window,target_width,target_height);
            }
        }
    }
    {
        std::lock_guard<std::mutex> guard(state().lock);
        auto entry_it=state().windows.find(window);
        if(entry_it!=state().windows.end() && entry_it->second.task==task &&
           entry_it->second.native_id==native_id) {
            entry_it->second.resize_in_progress=!result&&resize_requested;
            if(!result) {
                entry_it->second.content_width=pending.request.width;
                entry_it->second.content_height=pending.request.height;
            }
        }
        auto pending_it=state().pending_layouts.find(task);
        if(!result && pending_it!=state().pending_layouts.end() &&
           same_pending(pending_it->second,pending))state().pending_layouts.erase(pending_it);
    }
    std::printf("ORIGINAL_LINUX_WINDOW_LAYOUT guest=%d activity=%d task=%d native=%u color=%u request=%d,%d,%u,%u outer=%d,%d,%u,%u paint=%d,%d,%u,%u resize=%u,%u result=%d original_wms14=1 pending_replay=1\n",
        pending.guest_window,pending.activity,pending.task,native_id,pending.color_mode,
        pending.request.x,pending.request.y,pending.request.width,pending.request.height,
        outer.x,outer.y,outer.width,outer.height,paint.x,paint.y,paint.width,paint.height,
        target_width,target_height,result);
    std::fflush(stdout);
}
void geometry_update_internal(void *window,bool queue_metadata) {
    if(!window)return;
    int32_t task=-1;
    bool register_close=false;
    WillCloseListenerSptr close_listener;
    {
        std::lock_guard<std::mutex> guard(state().lock);
        auto it=state().windows.find(window);
        if(it==state().windows.end()) {
            /* Original AnCo system windows do not traverse the main-window
             * registerRectListener callback.  Their unchanged libwm identity
             * and viewport observations reach this bridge after
             * AncoWindowManager names them shell_sys_win_<guestId>. */
            auto property=geometry_property(window);
            const int32_t guest_window=property.pointer?
                original_system_guest_window(property.pointer):-1;
            const int32_t native_id=property.pointer?
                geometry_persistent_id(property.pointer):-1;
            if(guest_window>0 && native_id>0) {
                auto inserted=state().windows.try_emplace(window,
                    Entry{static_cast<uint32_t>(native_id),-1,guest_window});
                it=inserted.first;
                std::printf("ORIGINAL_LINUX_SYSTEM_WINDOW guest=%d native=%d original_create_callback=1\n",
                    guest_window,native_id);
                std::fflush(stdout);
            }
        }
        if(it!=state().windows.end()){
            if(!it->second.will_close_listener) {
                close_listener=new GeometryWillCloseListener(window);
                it->second.will_close_listener=close_listener;
                register_close=true;
            }
            publish_locked(window,it->second);
            task=it->second.task;
        }
    }
    if(register_close) {
        const int result=geometry_register_will_close(window,close_listener);
        std::printf("ORIGINAL_LINUX_WINDOW_CLOSE_LISTENER result=%d original_listener=1\n",result);
        std::fflush(stdout);
        if(result!=0) {
            std::lock_guard<std::mutex> guard(state().lock);
            auto it=state().windows.find(window);
            if(it!=state().windows.end() && it->second.will_close_listener.GetRefPtr()==
               close_listener.GetRefPtr())it->second.will_close_listener=WillCloseListenerSptr();
        }
    }
    /* Linux publishes supportWindowModes after the original root has already
     * bound.  Apply those capabilities as soon as a later original geometry
     * callback can see the metadata; otherwise the first caption-bar maximize
     * still observes the package's fullscreen-only fallback and bypasses the
     * stock maximizeFloating route.  SetSupportedWindowModes is already
     * guarded against re-entry by capabilities_in_progress and remains the
     * unchanged original WindowSession operation.  Title/icon work stays on
     * the original Ace UI executor below. */
    if(queue_metadata && task>=0) {
        apply_original_capabilities(window,task);
        queue_original_title_for_task(window,task);
    }
}
}

#define ENTRY extern "C" __attribute__((visibility("default")))
extern "C" __attribute__((visibility("hidden")))
int32_t anco_linux_window_decor_visible(uint32_t native_id,bool *visible) {
    if(!native_id || !visible)return 0;
    int32_t task=-1;
    {
        std::lock_guard<std::mutex> guard(state().lock);
        for(const auto &[window,entry]:state().windows) {
            (void)window;
            if(entry.native_id==native_id) { task=entry.task;break; }
        }
    }
    LinuxMetadata metadata;
    if(task<0 || !read_linux_metadata(task,metadata))return 0;
    *visible=metadata.decor_visible;
    return 1;
}
ENTRY void anco_linux_window_geometry_bind(void *window,uint32_t native_id,int32_t task) {
    ensure_linux_metadata_watch();
    if(!window || !native_id || task<-1)return;
    // Arguments are the REAL original registerRectListener lookup and Task.
    // Root ownership uses its original Task; task -1 is admitted only when the
    // original Window name itself encodes shell_sys_win_<guest Window ID>.
    auto original=geometry_window(native_id);
    if(original.pointer!=window)return;
    auto property=geometry_property(window);if(!property.pointer)return;
    const int32_t guest_window=task==-1?original_system_guest_window(property.pointer):0;
    if(task==-1 && guest_window<=0)return;
    const int32_t current_instance=task>=0?OHOS::Ace::ContainerScope::CurrentId():-1;
    if(task>=0 && current_instance<0)return;
    /* SP68 enables free-window state on the real main WindowSceneSessionImpl
     * before its title/hot-area notifications are consumed.  Linux main
     * windows enter through the same original Window object but have no
     * Ability-side call that performs this step, so connect that existing
     * method here.  System/sub-windows (task -1) keep their inherited state. */
    if(task>=0) {
        const int free_window_result=geometry_switch_free_multi_window(window,true);
        std::printf("ORIGINAL_LINUX_WINDOW_FREE_MODE task=%d result=%d original_switch=1\n",
            task,free_window_result);
        std::fflush(stdout);
    }
    /* GetWindowStatus can synchronously traverse the original status path.
     * Read it before taking the bridge state lock so an original callback can
     * never re-enter geometry_status_changed() while publish_locked() owns the
     * same non-recursive mutex.  The listener remains the authority for every
     * later transition. */
    OHOS::Rosen::WindowStatus initial_status;
    const int initial_status_result=task>=0?
        geometry_get_status(window,initial_status):INT32_MIN;
    const uint32_t initial_status_value=initial_status_result==0?
        static_cast<uint32_t>(initial_status):0;
    bool register_status=false;
    bool register_close=false;
    StatusChangeListenerSptr status_listener;
    WillCloseListenerSptr close_listener;
    {
        std::lock_guard<std::mutex> guard(state().lock);
        auto [it,inserted]=state().windows.try_emplace(window,
            Entry{native_id,task,guest_window});
        if(!inserted && (it->second.native_id!=native_id || it->second.task!=task ||
           it->second.guest_window!=guest_window))return;
        if(task>=0) {
            auto [instance,instance_inserted]=state().ui_instances.try_emplace(window,current_instance);
            if(!instance_inserted && instance->second!=current_instance)return;
            if(!it->second.window_status_known && initial_status_value>=1 &&
               initial_status_value<=5) {
                it->second.window_status=initial_status_value;
                it->second.window_status_known=true;
            }
        }
        if(!it->second.status_change_listener) {
            status_listener=new GeometryStatusChangeListener(window);
            it->second.status_change_listener=status_listener;
            register_status=true;
        }
        if(!it->second.will_close_listener) {
            close_listener=new GeometryWillCloseListener(window);
            it->second.will_close_listener=close_listener;
            register_close=true;
        }
        publish_locked(window,it->second);
    }
    if(register_status) {
        const int result=geometry_register_status_change(window,status_listener);
        std::printf("ORIGINAL_LINUX_WINDOW_STATUS_LISTENER result=%d original_listener=1\n",result);
        std::fflush(stdout);
        if(result!=0) {
            std::lock_guard<std::mutex> guard(state().lock);
            auto it=state().windows.find(window);
            if(it!=state().windows.end() &&
               it->second.status_change_listener.GetRefPtr()==status_listener.GetRefPtr()) {
                it->second.status_change_listener=StatusChangeListenerSptr();
            }
        }
    }
    if(register_close) {
        const int result=geometry_register_will_close(window,close_listener);
        std::printf("ORIGINAL_LINUX_WINDOW_CLOSE_LISTENER result=%d original_listener=1\n",result);
        std::fflush(stdout);
        if(result!=0) {
            std::lock_guard<std::mutex> guard(state().lock);
            auto it=state().windows.find(window);
            if(it!=state().windows.end() && it->second.will_close_listener.GetRefPtr()==
               close_listener.GetRefPtr())it->second.will_close_listener=WillCloseListenerSptr();
        }
    }
    if(task>=0) {
        apply_original_capabilities(window,task);
        apply_pending_layout(task);
    /* Keep the proven original Recover -> pending Resize path contiguous.
     * SetAPPWindowLabel/Icon synchronously invalidates Ace titlebar layout on
     * this build, so metadata must not interrupt the first native geometry
     * replay.  These are still the unchanged original WindowSession calls. */
        queue_original_title_for_task(window,task);
    }
}
ENTRY void anco_linux_window_geometry_update(void *window) {
    geometry_update_internal(window,true);
}
extern "C" __attribute__((visibility("hidden")))
void anco_linux_window_defer_title_event(uint32_t native_id,bool shown) {
    if(!native_id)return;
    /* The registration call is made on ShellAssistant's UI thread while its
     * window table is locked.  Resolve the original Window strongly from a
     * worker, then use the already established UIContent executor so the
     * unchanged title callback runs only after registration has returned. */
    std::thread([native_id,shown]() {
        auto original=geometry_window(native_id);
        if(!original.pointer)return;
        void *window=original.pointer;
        auto ui=geometry_ui(window);if(!ui)return;
        int32_t task=-1,resolved=-1;
        {
            std::lock_guard<std::mutex> guard(state().lock);
            const auto entry=state().windows.find(window);
            const auto instance=state().ui_instances.find(window);
            if(entry==state().windows.end() || entry->second.native_id!=native_id ||
               entry->second.task<0 || instance==state().ui_instances.end() ||
               instance->second<0)return;
            task=entry->second.task;
            resolved=instance->second;
        }
        const auto previous=OHOS::Ace::ContainerScope::CurrentId();
        OHOS::Ace::ContainerScope::UpdateCurrent(resolved);
        geometry_execute_ui(ui.get(),[native_id,task,shown]() {
            auto live=geometry_window(native_id);
            if(!live.pointer)return;
            {
                std::lock_guard<std::mutex> guard(state().lock);
                const auto entry=state().windows.find(live.pointer);
                if(entry==state().windows.end() || entry->second.native_id!=native_id ||
                   entry->second.task!=task)return;
            }
            anco_window_title_position_6_1_to_6_0(live.pointer,shown,0);
            std::printf("ORIGINAL_LINUX_WINDOW_TITLE_EVENT_DEFERRED task=%d native=%u shown=%d original_ui_executor=1\n",
                task,native_id,shown);
            std::fflush(stdout);
        },"L410AncoDeferredLinuxTitleEvent");
        OHOS::Ace::ContainerScope::UpdateCurrent(previous);
    }).detach();
}
ENTRY void anco_linux_window_layout_update(int32_t guest_window,int32_t activity,
    const void *request_property,int32_t task,uint32_t color_mode) {
    if(!request_property || guest_window<=0 || activity<=0 || task<0)return;
    const int request_type=geometry_request_type(request_property);
    // The original WMS broker has already converted the guest type 2 into the
    // Rosen main-window type 1 before invoking this lifecycle callback.
    if(request_type!=1)return;
    const Rect request=geometry_request_rect(request_property);
    const Rect window_rect=geometry_window_rect(request_property);
    std::printf("ORIGINAL_LINUX_WINDOW_LAYOUT_PROPERTY guest=%d activity=%d task=%d request=%d,%d,%u,%u window=%d,%d,%u,%u original_wms14=1\n",
        guest_window,activity,task,request.x,request.y,request.width,request.height,
        window_rect.x,window_rect.y,window_rect.width,window_rect.height);
    std::fflush(stdout);
    /* GetRequestRect is the unchanged original WMS14 projection.  Each
     * xdg_toplevel now owns an independent native Task root, so TYPE_APPLICATION
     * plus that Task mapping identifies the resize owner.  The request origin
     * may be negative when WMS projects xdg decoration insets; native Resize
     * consumes the full projected width and height rather than that origin. */
    if(!request.width || !request.height ||
       request.width>16384 || request.height>16384)return;
    {
        std::lock_guard<std::mutex> guard(state().lock);
        for(const auto &[window,entry]:state().windows) {
            (void)window;
            if(entry.task==task && entry.resize_in_progress) {
                return;
            }
            if(entry.task==task && entry.content_width && entry.content_height) {
                return;
            }
        }
        state().pending_layouts[task]=PendingLayout{
            guest_window,activity,task,color_mode,request};
    }
    apply_pending_layout(task);
    /* Metadata follows the original WMS14 layout replay.  Calling it before
     * recording/replaying the request can synchronously re-enter Ace layout
     * and leave the initial freeform Resize without a stable paint rect. */
    queue_original_title_for_task_id(task);
}
ENTRY void anco_linux_window_geometry_forget_id(int32_t id) {
    std::vector<std::pair<void *,WillCloseListenerSptr>> listeners;
    {
        std::lock_guard<std::mutex> guard(state().lock);
        for(auto it=state().windows.begin();it!=state().windows.end();) {
            if(it->second.native_id!=uint32_t(id)){++it;continue;}
            state().pending_layouts.erase(it->second.task);
            state().metadata_tasks.erase(it->first);
            state().ui_instances.erase(it->first);
            if(!it->second.path.empty())unlink(it->second.path.c_str());
            if(it->second.will_close_listener)
                listeners.emplace_back(const_cast<void *>(it->first),
                    it->second.will_close_listener);
            it=state().windows.erase(it);
        }
    }
    for(const auto &[window,listener]:listeners)
        (void)geometry_unregister_will_close(window,listener);
}
ENTRY void anco_linux_window_geometry_forget(void *window) {
    WillCloseListenerSptr listener;
    {
        std::lock_guard<std::mutex> guard(state().lock);
        auto it=state().windows.find(window);if(it==state().windows.end())return;
        state().pending_layouts.erase(it->second.task);
        state().metadata_tasks.erase(window);
        state().ui_instances.erase(window);
        if(!it->second.path.empty())unlink(it->second.path.c_str());
        listener=it->second.will_close_listener;
        state().windows.erase(it);
    }
    if(listener)(void)geometry_unregister_will_close(window,listener);
}
