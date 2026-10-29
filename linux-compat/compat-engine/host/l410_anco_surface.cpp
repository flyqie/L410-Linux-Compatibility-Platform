/* 用卓易通自己的接口把窗口表面交给系统图形 broker（照它的做法）。
 *
 * 链路（全部是它们导出的符号与头文件里的类）：
 *   OHOS::ConsumerSurface(name)        —— 造一个缓冲队列（我们持有生产端）
 *   OHOS::ConsumerSurface::GetProducer()
 *   OHOS::Surface::CreateSurfaceAsProducer(producer)   —— 得到可提交的 Surface
 *   OHOS::ANCO::GraphicBrokerClient::GetInstance()/Init()
 *   OHOS::ANCO::GraphicBrokerClient::SetSurfaceWithWindowID(windowId, surface)
 *
 * 注意：本模块用 Debian 的 g++ 编译（libstdc++），而系统库是 libc++（命名空间 std::__h）。
 * 因此这里不引入任何 std 类型：唯一需要跨过去的是 ConsumerSurface 那个 const std::string&
 * 参数——按 libc++ 的短字符串布局手工搭一个 24 字节对象传指针即可（引用就是一个指针）。
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

/* ---- 只声明我们要用的那些，字段/布局照它们头文件里的类型（sptr 就是单指针包装） ---- */
namespace OHOS {
class RefBase {
public:
    virtual ~RefBase() = default;
};
template <typename T>
class sptr {
public:
    sptr() : ptr_(nullptr) {}
    sptr(T *other) : ptr_(other) {}
    T *GetRefPtr() const { return ptr_; }
private:
    T *ptr_ = nullptr;
};
class IBufferProducer : public RefBase {};
class Surface : public RefBase {
public:
    static sptr<Surface> CreateSurfaceAsProducer(sptr<IBufferProducer> &producer);
};
class IConsumerSurface : public RefBase {
public:
    virtual sptr<IBufferProducer> GetProducer() const;
};
class ConsumerSurface : public IConsumerSurface {
public:
    ConsumerSurface(const void *name);   /* 真实签名是 const std::string&（libc++） */
};
}  // namespace OHOS

/* libc++ 的短字符串：24 字节，0..22 是字符，第 23 字节是最短长度（高位 0 表示 short） */
struct LibcxxShortString {
    char data[23];
    unsigned char size;
};

static void build_libcxx_string(LibcxxShortString *out, const char *text) {
    memset(out, 0, sizeof(*out));
    size_t length = strlen(text);
    if (length > 22) length = 22;
    memcpy(out->data, text, length);
    out->size = (unsigned char)length;
}

/* 我们自己的 operator new/delete：不引入 libstdc++ */
void *operator new(size_t size) { return malloc(size ? size : 1); }
void *operator new[](size_t size) { return malloc(size ? size : 1); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

/* OHOS 的 sptr<T> 只有 8 字节，但它是**非平凡类型**：返回它时编译器用 sret（x8 传结果地址）。
 * 我们必须用同样"非平凡"的声明，ABI 才一致（否则它会往我们没给的地址写 → 崩）。 */
struct OhosSptr {
    void *ptr;
    OhosSptr() : ptr(nullptr) {}
    OhosSptr(const OhosSptr &other) : ptr(other.ptr) {}
    ~OhosSptr() {}
};

static char g_anco_error[256];

static void anco_report(const char *fmt, long a, long b, long c) {
    snprintf(g_anco_error, sizeof(g_anco_error), fmt, a, b, c);
    fprintf(stderr, "l410-anco: %s\n", g_anco_error);
    fflush(stderr);
}

extern "C" {

const char *l410_anco_last_error(void) { return g_anco_error; }


/* 造一个 producer Surface，返回它的裸指针（失败返回 NULL）。 */
void *l410_anco_create_surface(const char *name) {
    LibcxxShortString label;
    build_libcxx_string(&label, name ? name : "l410-anco");
    /* ConsumerSurface 的对象大小没有公开，按足够大分配（它的成员是若干 sptr/mutex）。 */
    void *storage = calloc(1, 4096);
    if (!storage) return nullptr;
    typedef void (*Ctor)(void *, const void *);
    Ctor ctor = (Ctor)dlsym(RTLD_DEFAULT,
        "_ZN4OHOS15ConsumerSurfaceC1ERKNSt3__h12basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE");
    if (!ctor) {
        void *lib = dlopen("libsurface.z.so", RTLD_NOW);
        void *surface_lib = dlopen("libsurface.z.so", RTLD_NOW);
        if (!surface_lib) surface_lib = dlopen("/system/lib64/chipset-sdk-sp/libsurface.z.so", RTLD_NOW);
        if (!surface_lib) surface_lib = dlopen("/system/lib64/chipset-pub-sdk/libsurface.z.so", RTLD_NOW);
        if (surface_lib) {
            ctor = (Ctor)dlsym(surface_lib,
                "_ZN4OHOS15ConsumerSurfaceC1ERKNSt3__h12basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE");
            anco_report("surface_lib=1 ctor=%ld", (long)(ctor ? 1 : 0), 0, 0);
        } else {
            anco_report("dlopen libsurface failed a=%ld b=%ld", (long)1, (long)0, 0);
        }
    }
    typedef OhosSptr (*GetProducerFn)(const void *);
    typedef OhosSptr (*CreateProducerSurfaceFn)(OhosSptr &);
    /* 这些符号都在 libsurface.z.so 里：必须先 dlopen 拿到句柄再查（RTLD_DEFAULT 只能查到已加载的库）。 */
    void *probe_lib = dlopen("libsurface.z.so", RTLD_NOW);
    if (!probe_lib) probe_lib = dlopen("/system/lib64/chipset-sdk-sp/libsurface.z.so", RTLD_NOW);
    if (!probe_lib) probe_lib = dlopen("/system/lib64/chipset-pub-sdk/libsurface.z.so", RTLD_NOW);
    GetProducerFn get_producer = probe_lib
        ? (GetProducerFn)dlsym(probe_lib, "_ZNK4OHOS15ConsumerSurface11GetProducerEv") : NULL;
    if (!get_producer) get_producer = (GetProducerFn)dlsym(RTLD_DEFAULT, "_ZNK4OHOS15ConsumerSurface11GetProducerEv");
    CreateProducerSurfaceFn create_producer_surface = probe_lib
        ? (CreateProducerSurfaceFn)dlsym(probe_lib,
            "_ZN4OHOS7Surface23CreateSurfaceAsProducerERNS_4sptrINS_15IBufferProducerEEE") : NULL;
    if (!create_producer_surface) create_producer_surface = (CreateProducerSurfaceFn)dlsym(RTLD_DEFAULT,
        "_ZN4OHOS7Surface23CreateSurfaceAsProducerERNS_4sptrINS_15IBufferProducerEEE");
    fprintf(stderr, "l410-anco: ctor=%p getProducer=%p createSurface=%p\n",
            (void *)ctor, (void *)get_producer, (void *)create_producer_surface);
    fflush(stderr);
    if (!ctor || !get_producer || !create_producer_surface) {
        anco_report("missing symbols ctor=%ld gp=%ld cs=%ld", (long)(ctor ? 1 : 0),
                    (long)(get_producer ? 1 : 0), (long)(create_producer_surface ? 1 : 0));
        return nullptr;
    }
    ctor(storage, &label);
    /* 构造之后必须先 Init：不 Init 直接 GetProducer 会 SIGSEGV（崩溃栈已定位到这一步）。 */
    typedef int32_t (*InitFn)(void *);
    InitFn consumer_init = probe_lib ? (InitFn)dlsym(probe_lib, "_ZN4OHOS15ConsumerSurface4InitEv") : NULL;
    if (!consumer_init) consumer_init = (InitFn)dlsym(RTLD_DEFAULT, "_ZN4OHOS15ConsumerSurface4InitEv");
    if (consumer_init) {
        const int32_t init_result = consumer_init(storage);
        anco_report("consumer_init=%ld", (long)init_result, 0, 0);
        if (init_result != 0) return nullptr;
    } else {
        anco_report("consumer_init symbol missing=%ld", 0, 0, 0);
    }
    OhosSptr producer = get_producer(storage);       /* 返回 sptr<IBufferProducer>（非平凡 → sret） */
    if (!producer.ptr) {
        anco_report("GetProducer null a=%ld b=%ld", 0, 0, 0);
        return nullptr;
    }
    OhosSptr surface = create_producer_surface(producer);   /* 返回 sptr<Surface> */
    anco_report("producer=%ld surface=%ld", (long)(producer.ptr != NULL), (long)(surface.ptr != NULL), 0);
    return surface.ptr;
}

/* 把这个 Surface 交给 anco 的图形 broker（等价于卓易通 guest 的做法）。 */
int32_t l410_anco_set_surface(void *surface, int32_t window_id) {
    if (!surface) return -1;
    void *lib = dlopen("libgraphic_broker_client.z.so", RTLD_NOW);
    if (!lib) lib = dlopen("/system/lib64/libgraphic_broker_client.z.so", RTLD_NOW);
    if (!lib) return -2;
    void *(*get_instance)(void) = (void *(*)(void))dlsym(lib,
        "_ZN4OHOS4ANCO19GraphicBrokerClient11GetInstanceEv");
    int32_t (*init)(void *) = (int32_t (*)(void *))dlsym(lib,
        "_ZN4OHOS4ANCO19GraphicBrokerClient4InitEv");
    int32_t (*set_surface)(void *, long, void *) = (int32_t (*)(void *, long, void *))dlsym(lib,
        "_ZN4OHOS4ANCO19GraphicBrokerClient22SetSurfaceWithWindowIDElRNS_4sptrINS_7SurfaceEEE");
    if (!get_instance || !set_surface) return -3;
    void *client = get_instance();
    if (!client) return -4;
    if (!init) return -5;
    const int32_t initialized = init(client);
    /* This vendor client returns false/0 when SA 66829 is absent.  Calling
     * SetSurfaceWithWindowID after that is a no-op which also returns 0, so it
     * must not be reported as a successful broker hand-off. */
    if (initialized == 0) {
        anco_report("GraphicBroker SA unavailable init=%ld", (long)initialized, 0, 0);
        return -6;
    }
    /* sptr<Surface>& 就是指向那 8 字节指针的引用 */
    void *slot[1] = { surface };
    int32_t result = set_surface(client, (long)window_id, slot);
    fprintf(stderr, "l410-anco: SetSurfaceWithWindowID(%d) -> %d\n", window_id, result);
    fflush(stderr);
    return result;
}

}  /* extern "C" */

/* ---- 异步探测：分阶段上报，挂住也能看清卡在哪一步 ---- */
#include <pthread.h>

static volatile int g_anco_stage = 0;
static volatile int g_anco_result = -999;
static volatile int g_anco_pid_arg = 0;

static void *anco_probe_thread(void *arg) {
    long window_id = (long)arg;
    g_anco_stage = 1;
    void *surface = l410_anco_create_surface("l410-anco");
    g_anco_stage = (surface ? 5 : 4);
    if (surface) {
        g_anco_result = l410_anco_set_surface(surface, (int32_t)window_id);
    } else {
        g_anco_result = -1;
    }
    g_anco_stage = 9;
    return NULL;
}

extern "C" {

int32_t l410_anco_probe_async(int32_t window_id) {
    pthread_t thread;
    g_anco_stage = 0;
    g_anco_result = -999;
    if (pthread_create(&thread, NULL, anco_probe_thread, (void *)(long)window_id)) return -1;
    pthread_detach(thread);
    return 0;
}

int32_t l410_anco_probe_stage(void) { return g_anco_stage; }
int32_t l410_anco_probe_result(void) { return g_anco_result; }
const char *l410_anco_probe_error(void) { return g_anco_error; }
}
