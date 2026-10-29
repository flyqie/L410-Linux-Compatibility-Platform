/* L410 兼容引擎：宿主侧 broker 的通道模块（NAPI）。
 *
 * 对应 anco 的 anco_service_broker + GraphicBroker 的"宿主 <-> 容器"这一层：
 *   - 绑定引擎通道 l410-engine-<pid>（AF_UNIX 抽象套接字），接受容器侧显示/输入服务；
 *   - 只搬运整行文本：窗口创建/移除/属性/任务、输入事件、显示信息；
 *   - 每行的解析放在 ArkTS 侧（消息到达即推送，不轮询）；
 *   - 每个窗口的图形缓冲仍由该窗口的 XComponent 模块（libl410displaymulti.so）单独服务，
 *     与 anco "按 windowId 交 surface" 的分工一致。
 *
 * 约束（沿用本设备已验证的做法）：
 *   - 工作线程只用系统调用（poll/accept/recv/send/close）与 memcpy/memchr；
 *     不在工作线程调用任何 libc 格式化函数；
 *   - 不轮询渲染：通道只在有数据时唤醒 JS。
 */
#define _GNU_SOURCE
#include <node_api.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <poll.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdbool.h>

#define ENGINE_LINE_MAX 1024
#define ENGINE_CLIENTS 8

/* 一个容器进程一条连接：宿主同时承载多个应用（featherpad 和 mousepad 可以同时开着）。
   每条上行消息都带上来源进程号，JS 侧按 pid 认出是谁发的；下行可以广播（pid=0），
   各客户端只认自己那些窗口 id，别人的消息直接忽略。 */
typedef struct {
    int fd;
    long pid;
    char buffer[4096];
    size_t used;
} EngineClient;

typedef struct {
    int listen_fd;
    int stop;
    int stage;                 /* 0=未启动 1=已绑定 2=已连接容器 */
    unsigned long frames;
    unsigned long messages;
    pthread_t thread;
    int started;
    char endpoint[96];
    pthread_mutex_t lock;
    EngineClient clients[ENGINE_CLIENTS];
    int client_count;
} Engine;

static Engine engine;
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static napi_threadsafe_function listener = NULL;

/* 上行消息统一加 "<pid> " 前缀，JS 才知道是哪条连接的窗口/输入。 */
static void deliver_from(long pid, const char *line) {
    if (!listener || !line) return;
    char buffer[ENGINE_LINE_MAX + 32];
    snprintf(buffer, sizeof(buffer), "%ld %s", pid, line);
    char *copy = strdup(buffer);
    if (!copy) return;
    if (napi_call_threadsafe_function(listener, copy, napi_tsfn_nonblocking) != napi_ok) free(copy);
}

static void call_js(napi_env env, napi_value js_callback, void *context, void *data) {
    (void)context;
    char *line = data;
    if (!env || !js_callback) { free(line); return; }
    napi_value argument;
    napi_value undefined;
    if (napi_create_string_utf8(env, line, NAPI_AUTO_LENGTH, &argument) == napi_ok &&
        napi_get_undefined(env, &undefined) == napi_ok) {
        napi_call_function(env, undefined, js_callback, 1, &argument, NULL);
    }
    free(line);
}

/* 把缓冲区里的完整行逐条推给 JS；返回剩余未完整字节数。 */
/* 把某个客户端缓冲区里的完整行逐条推给 JS。 */
static size_t feed_client_lines(EngineClient *client) {
    size_t start = 0;
    for (size_t i = 0; i < client->used; i++) {
        if (client->buffer[i] != '\n') continue;
        client->buffer[i] = 0;
        if (i > start) {
            deliver_from(client->pid, client->buffer + start);
            __atomic_add_fetch(&engine.messages, 1, __ATOMIC_RELAXED);
        }
        start = i + 1;
    }
    if (start == 0) return client->used;
    size_t rest = client->used - start;
    memmove(client->buffer, client->buffer + start, rest);
    return rest;
}

static void drop_client(int index) {
    pthread_mutex_lock(&engine.lock);
    EngineClient *client = &engine.clients[index];
    long pid = client->pid;
    if (client->fd >= 0) close(client->fd);
    client->fd = -1;
    client->pid = 0;
    client->used = 0;
    for (int i = index; i + 1 < engine.client_count; i++)
        engine.clients[i] = engine.clients[i + 1];
    --engine.client_count;
    engine.clients[engine.client_count].fd = -1;
    engine.clients[engine.client_count].pid = 0;
    engine.clients[engine.client_count].used = 0;
    if (!engine.client_count) __atomic_store_n(&engine.stage, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&engine.lock);
    if (pid) {
        char note[64];
        snprintf(note, sizeof(note), "ENGINE_CLIENT_GONE %ld", pid);
        deliver_from(pid, "ENGINE_CLIENT_GONE");
        (void)note;
    }
}

static void *engine_thread(void *arg) {
    (void)arg;
    struct pollfd waiting[ENGINE_CLIENTS + 1];

    while (!__atomic_load_n(&engine.stop, __ATOMIC_ACQUIRE)) {
        int count = 0;
        waiting[count].fd = engine.listen_fd;
        waiting[count].events = POLLIN;
        waiting[count].revents = 0;
        ++count;
        pthread_mutex_lock(&engine.lock);
        int clients = engine.client_count;
        for (int i = 0; i < clients; i++) {
            waiting[count].fd = engine.clients[i].fd;
            waiting[count].events = POLLIN;
            waiting[count].revents = 0;
            ++count;
        }
        pthread_mutex_unlock(&engine.lock);

        int ready = poll(waiting, (nfds_t)count, 200);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;

        if (waiting[0].revents & POLLIN) {
            int fd = accept4(engine.listen_fd, NULL, NULL, SOCK_CLOEXEC);
            if (fd >= 0) {
                struct ucred cred;
                socklen_t size = sizeof(cred);
                if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) || cred.uid != 0) {
                    close(fd);
                } else {
                    struct timeval timeout = {2, 0};
                    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                    pthread_mutex_lock(&engine.lock);
                    if (engine.client_count < ENGINE_CLIENTS) {
                        EngineClient *client = &engine.clients[engine.client_count++];
                        client->fd = fd;
                        client->pid = (long)cred.pid;
                        client->used = 0;
                        fd = -1;
                        __atomic_store_n(&engine.stage, 2, __ATOMIC_RELEASE);
                    }
                    pthread_mutex_unlock(&engine.lock);
                    if (fd < 0)
                        deliver_from((long)cred.pid, "ENGINE_CLIENT_CONNECTED");
                    else
                        close(fd);      /* 连接数已满 */
                }
            }
        }

        for (int i = 0; i < count - 1; i++) {
            if (!(waiting[i + 1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) continue;
            pthread_mutex_lock(&engine.lock);
            if (i >= engine.client_count) {
                pthread_mutex_unlock(&engine.lock);
                continue;
            }
            EngineClient *client = &engine.clients[i];
            int fd = client->fd;
            pthread_mutex_unlock(&engine.lock);
            if (fd < 0) continue;

            ssize_t got = recv(fd, client->buffer + client->used,
                               sizeof(client->buffer) - client->used, 0);
            int dead = 0;
            if (got > 0) {
                client->used += (size_t)got;
                client->used = feed_client_lines(client);
                if (client->used >= sizeof(client->buffer)) client->used = 0;
            } else if (got == 0) {
                dead = 1;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                dead = 1;
            }
            if (dead) {
                pthread_mutex_lock(&engine.lock);
                int index = -1;
                for (int c = 0; c < engine.client_count; c++) {
                    if (engine.clients[c].fd == fd) { index = c; break; }
                }
                pthread_mutex_unlock(&engine.lock);
                if (index >= 0) drop_client(index);
            }
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ NAPI ---- */

static napi_value start(napi_env env, napi_callback_info info) {
    (void)info;
    pthread_mutex_lock(&registry_lock);
    if (!engine.started) {
        snprintf(engine.endpoint, sizeof(engine.endpoint), "l410-engine-%ld", (long)getpid());
        engine.listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        memset(engine.clients, 0, sizeof(engine.clients));
        for (int i = 0; i < 8; i++) engine.clients[i].fd = -1;
        engine.client_count = 0;
        if (engine.listen_fd >= 0) {
            struct sockaddr_un address;
            memset(&address, 0, sizeof(address));
            address.sun_family = AF_UNIX;
            size_t length = strlen(engine.endpoint);
            memcpy(address.sun_path + 1, engine.endpoint, length);
            if (!bind(engine.listen_fd, (void *)&address,
                      offsetof(struct sockaddr_un, sun_path) + 1 + length) &&
                !listen(engine.listen_fd, 1)) {
                __atomic_store_n(&engine.stop, 0, __ATOMIC_RELEASE);
                __atomic_store_n(&engine.stage, 1, __ATOMIC_RELEASE);
                if (!pthread_create(&engine.thread, NULL, engine_thread, NULL)) engine.started = 1;
            }
        }
    }
    napi_value result;
    napi_create_string_utf8(env, engine.started ? engine.endpoint : "", NAPI_AUTO_LENGTH, &result);
    pthread_mutex_unlock(&registry_lock);
    return result;
}

static napi_value set_listener(napi_env env, napi_callback_info info) {
    napi_value callback;
    size_t count = 1;
    if (napi_get_cb_info(env, info, &count, &callback, NULL, NULL) != napi_ok || count != 1) {
        napi_value no; napi_get_boolean(env, 0, &no); return no;
    }
    napi_valuetype type;
    if (napi_typeof(env, callback, &type) != napi_ok || type != napi_function) {
        napi_value no; napi_get_boolean(env, 0, &no); return no;
    }
    if (listener) {
        napi_release_threadsafe_function(listener, napi_tsfn_release);
        listener = NULL;
    }
    napi_value resource_name;
    if (napi_create_string_utf8(env, "l410engine", NAPI_AUTO_LENGTH, &resource_name) != napi_ok ||
        napi_create_threadsafe_function(env, callback, NULL, resource_name, 0, 1, NULL, NULL, NULL,
                                       call_js, &listener) != napi_ok) {
        listener = NULL;
        napi_value no; napi_get_boolean(env, 0, &no); return no;
    }
    napi_value yes; napi_get_boolean(env, 1, &yes);
    return yes;
}

/* 送一行给容器：send(line) 广播给所有应用，send(line, pid) 只给某一个。
   广播时各客户端只认自己那些窗口 id，别人的消息直接忽略。 */
static napi_value send_line(napi_env env, napi_callback_info info) {
    napi_value argv[2];
    size_t count = 2, length = 0;
    char line[ENGINE_LINE_MAX + 2];
    int ok = 0;
    long only_pid = 0;
    if (napi_get_cb_info(env, info, &count, argv, NULL, NULL) == napi_ok && count >= 1 &&
        napi_get_value_string_utf8(env, argv[0], NULL, 0, &length) == napi_ok &&
        length > 0 && length <= ENGINE_LINE_MAX &&
        napi_get_value_string_utf8(env, argv[0], line, sizeof(line) - 1, &length) == napi_ok) {
        if (count >= 2) {
            int32_t pid = 0;
            if (napi_get_value_int32(env, argv[1], &pid) == napi_ok && pid > 0) only_pid = (long)pid;
        }
        line[length] = '\n';
        const size_t total = length + 1;
        pthread_mutex_lock(&engine.lock);
        for (int i = 0; i < engine.client_count; i++) {
            EngineClient *client = &engine.clients[i];
            if (client->fd < 0) continue;
            if (only_pid && client->pid != only_pid) continue;
            size_t done = 0;
            ok = 1;
            while (done < total) {
                ssize_t written = send(client->fd, line + done, total - done, MSG_NOSIGNAL);
                if (written < 0) {
                    if (errno == EINTR) continue;
                    ok = 0;
                    break;
                }
                done += (size_t)written;
            }
        }
        pthread_mutex_unlock(&engine.lock);
    }
    napi_value result; napi_get_boolean(env, ok, &result); return result;
}

static napi_value status(napi_env env, napi_callback_info info) {
    (void)info;
    char text[256];
    pthread_mutex_lock(&engine.lock);
    snprintf(text, sizeof(text), "{\"endpoint\":\"%s\",\"stage\":%d,\"peers\":%d,\"frames\":%lu,\"messages\":%lu}",
             engine.endpoint, __atomic_load_n(&engine.stage, __ATOMIC_ACQUIRE), engine.client_count,
             __atomic_load_n(&engine.frames, __ATOMIC_RELAXED),
             __atomic_load_n(&engine.messages, __ATOMIC_RELAXED));
    pthread_mutex_unlock(&engine.lock);
    napi_value result; napi_create_string_utf8(env, text, NAPI_AUTO_LENGTH, &result); return result;
}

static napi_value note_frame(napi_env env, napi_callback_info info) {
    (void)info;
    __atomic_add_fetch(&engine.frames, 1, __ATOMIC_RELAXED);
    napi_value result; napi_get_boolean(env, 1, &result); return result;
}

static napi_value stop(napi_env env, napi_callback_info info) {
    (void)info;
    __atomic_store_n(&engine.stop, 1, __ATOMIC_RELEASE);
    pthread_mutex_lock(&registry_lock);
    if (engine.started) {
        pthread_join(engine.thread, NULL);
        engine.started = 0;
    }
    if (engine.listen_fd >= 0) { close(engine.listen_fd); engine.listen_fd = -1; }
    pthread_mutex_lock(&engine.lock);
    for (int i = 0; i < engine.client_count; i++) {
        if (engine.clients[i].fd >= 0) close(engine.clients[i].fd);
        engine.clients[i].fd = -1;
        engine.clients[i].pid = 0;
        engine.clients[i].used = 0;
    }
    engine.client_count = 0;
    pthread_mutex_unlock(&engine.lock);
    __atomic_store_n(&engine.stage, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&registry_lock);
    napi_value result; napi_get_boolean(env, 1, &result); return result;
}

/* ---------------------------------------------------------------- 应用拉起 ----
 * 宿主不只承载窗口，也负责把容器里的应用拉起来（anco 里这也是宿主/管理那一侧的活）。
 * 拉起时直接 exec 容器入口，不经过任何 shell：应用名只允许来自固定的应用表，
 * 环境变量在这里设好，路径全部是编译期常量。
 */
/* 每个应用自己的运行环境：工具包不同环境就不同（Qt 用我们的平台插件；
   GTK 用我们自己编的 GDK ohos 后端，要指向容器里那一套 GTK）。 */
static const char *engine_common_env[] = {
    "DBUS_SESSION_BUS_ADDRESS=disabled:", "GSETTINGS_BACKEND=memory",
    "NO_AT_BRIDGE=1", "HOME=/root", NULL,
};
static const char *engine_qt_env[] = {
    "QT_QPA_PLATFORM=ohos", "QT_QPA_PLATFORM_PLUGIN_PATH=/opt/l410/ohos-platforms",
    "QT_QPA_FONTDIR=/usr/share/fonts/truetype", NULL,
};
static const char *engine_gtk_env[] = {
    "GDK_BACKEND=ohos",
    "LD_LIBRARY_PATH=/home/linux/Shared/gtk-build/gtk-build/gdk:/home/linux/Shared/gtk-build/gtk-build/gtk:"
    "/home/linux/Shared/gtk-build/gtk-build/gdk-pixbuf:/home/linux/Shared/gtk-build/gtk-build/gtk/modules",
    "GDK_PIXBUF_MODULE_FILE=/usr/lib/aarch64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders.cache",
    "XDG_DATA_DIRS=/home/linux/Shared/gtk-build/gtk-build/gtk:/usr/share",
    "GDK_DPI_SCALE=1.35", NULL,
};

static const struct { const char *name; const char *path; const char *const *env; } engine_apps[] = {
    {"featherpad", "/usr/bin/featherpad", engine_qt_env},
    {"mousepad", "/usr/bin/mousepad", engine_gtk_env},
};

static napi_value launch_app(napi_env env, napi_callback_info info) {
    napi_value argv[2];
    size_t count = 2, length = 0;
    char name[64] = {0};
    char endpoint[96] = {0};
    if (napi_get_cb_info(env, info, &count, argv, NULL, NULL) != napi_ok || count < 1 ||
        napi_get_value_string_utf8(env, argv[0], name, sizeof(name) - 1, &length) != napi_ok) {
        napi_value no; napi_get_boolean(env, 0, &no); return no;
    }
    if (count >= 2)
        napi_get_value_string_utf8(env, argv[1], endpoint, sizeof(endpoint) - 1, &length);

    const char *path = NULL;
    const char *const *toolkit_env = NULL;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    for (size_t i = 0; i < sizeof(engine_apps) / sizeof(engine_apps[0]); i++) {
        if (!strcmp(engine_apps[i].name, base)) {
            path = engine_apps[i].path;
            toolkit_env = engine_apps[i].env;
            break;
        }
    }
    if (!path) {
        napi_value no; napi_get_boolean(env, 0, &no); return no;
    }

    pid_t child = fork();
    if (child < 0) {
        napi_value no; napi_get_boolean(env, 0, &no); return no;
    }
    if (child == 0) {
        setsid();
        setenv("L410_ENGINE_ENDPOINT", endpoint, 1);
        for (size_t i = 0; engine_common_env[i]; i++) putenv((char *)engine_common_env[i]);
        for (size_t i = 0; toolkit_env && toolkit_env[i]; i++) putenv((char *)toolkit_env[i]);
        const char *const args[] = {
            "/data/local/tmp/boot-tools-island/lib/ld-linux-aarch64.so.1",
            "--library-path", "/data/local/tmp/boot-tools-island/lib",
            "/data/vendor/l410-linux/bin/enter",
            "/data/vendor/l410-linux/rootfs-bookworm",
            path, NULL,
        };
        execv(args[0], (char *const *)args);
        /* exec 失败也要留下痕迹：写进应用沙箱的 launch.log，宿主日志里看不到子进程的 errno。 */
        {
            int fd = open("/data/storage/el2/base/haps/entry/files/launch.log",
                          O_WRONLY | O_CREAT | O_APPEND, 0666);
            if (fd >= 0) {
                char note[192];
                int size = snprintf(note, sizeof(note), "exec %s failed errno=%d\n", args[0], errno);
                if (size > 0) { ssize_t ignored = write(fd, note, (size_t)size); (void)ignored; }
                close(fd);
            }
        }
        _exit(127);
    }
    napi_value result; napi_create_int32(env, (int32_t)child, &result); return result;
}


/* 从应用沙箱里能看到容器入口吗？（拉起应用失败时第一件要查的事） */
static napi_value env_probe(napi_env env, napi_callback_info info) {
    (void)info;
    static const char *paths[] = {
        "/data/local/tmp/boot-tools-island/lib/ld-linux-aarch64.so.1",
        "/data/local/tmp/boot-tools-island/lib",
        "/data/vendor/l410-linux/bin/enter",
        "/data/vendor/l410-linux/rootfs-bookworm",
        "/data/vendor/l410-linux/shared",
        "/system/bin/sh",
    };
    char text[640];
    size_t used = 0;
    used += (size_t)snprintf(text + used, sizeof(text) - used, "{");
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]) && used < sizeof(text) - 96; i++) {
        used += (size_t)snprintf(text + used, sizeof(text) - used, "%s\"%s\":%d",
                                 i ? "," : "", paths[i], access(paths[i], R_OK) == 0 ? 1 : 0);
    }
    snprintf(text + used, sizeof(text) - used, "}");
    napi_value result; napi_create_string_utf8(env, text, NAPI_AUTO_LENGTH, &result); return result;
}


/* 容器生命周期归 root 系统服务（应用沙箱看不到容器，只能请它代劳）。
   连的是抽象套接字，不是路径，所以在沙箱里也通。 */
static napi_value service_send(napi_env env, napi_callback_info info) {
    napi_value value;
    size_t count = 1, length = 0;
    char line[512];
    char reply[256] = {0};
    if (napi_get_cb_info(env, info, &count, &value, NULL, NULL) != napi_ok || count != 1 ||
        napi_get_value_string_utf8(env, value, line, sizeof(line) - 4, &length) != napi_ok || !length) {
        napi_value empty; napi_create_string_utf8(env, "", 0, &empty); return empty;
    }
    line[length] = '\n';
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    const char *name = "l410-compat-service";
    memcpy(address.sun_path + 1, name, strlen(name));
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        struct timeval timeout = {3, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        if (!connect(fd, (struct sockaddr *)&address, offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name))) {
            if (send(fd, line, length + 1, MSG_NOSIGNAL) > 0) {
                ssize_t got = recv(fd, reply, sizeof(reply) - 1, 0);
                if (got > 0) {
                    reply[got] = 0;
                    char *newline = strchr(reply, '\n');
                    if (newline) *newline = 0;
                }
            }
        }
        close(fd);
    }
    napi_value result;
    napi_create_string_utf8(env, reply, NAPI_AUTO_LENGTH, &result);
    return result;
}


/* ---------------------------------------------------- 宿主侧窗口级输入（anco 做法）
 * 键盘不能靠 ArkUI 控件焦点（子窗口拿不到"激活"），所以由宿主自己向输入服务取：
 * libohinput.so 的 OH_Input_AddKeyEventMonitor/…MouseEventMonitor/…TouchEventMonitor。
 * 事件结构体是 opaque 的，字段一律用 OH_Input_Get* 读；读到就按协议推给 JS，
 * 由 JS 按"最后被点过的窗口"发给对应应用。
 */
typedef int32_t (*AddKeyMonitor)(void *callback);
typedef int32_t (*GetKeyCode)(const void *event);
typedef int32_t (*GetKeyAction)(const void *event);
typedef int32_t (*GetKeyWindowId)(const void *event);

static AddKeyMonitor g_add_key_monitor = NULL;
static GetKeyCode g_get_key_code = NULL;
static GetKeyAction g_get_key_action = NULL;
static GetKeyWindowId g_get_key_window = NULL;

static void engine_key_monitor(const void *event) {
    if (!event || !g_get_key_code || !g_get_key_action) return;
    const int code = g_get_key_code(event);
    const int action = g_get_key_action(event);
    const int window = g_get_key_window ? g_get_key_window(event) : 0;
    fprintf(stderr, "l410-input-monitor: action=%d code=%d window=%d\n", action, code, window);
    fflush(stderr);
    char line[96];
    snprintf(line, sizeof(line), "KEYEVENT action=%d code=%d window=%d", action, code, window);
    deliver_from(0, line);
}

static napi_value input_monitor(napi_env env, napi_callback_info info) {
    (void)info;
    void *input = dlopen("libohinput.so", RTLD_NOW);
    void *wm = dlopen("libnative_window_manager.so", RTLD_NOW);
    int result = -1;
    if (input) {
        g_get_key_code = (GetKeyCode)dlsym(input, "OH_Input_GetKeyEventKeyCode");
        g_get_key_action = (GetKeyAction)dlsym(input, "OH_Input_GetKeyEventAction");
        g_get_key_window = (GetKeyWindowId)dlsym(input, "OH_Input_GetKeyEventWindowId");
        g_add_key_monitor = (AddKeyMonitor)dlsym(input, "OH_Input_AddKeyEventMonitor");
        if (g_add_key_monitor) result = (int)g_add_key_monitor((void *)engine_key_monitor);
    }
    /* 窗口级过滤器先只探测符号是否存在（注册要窗口 id，等 id 拿到再上）。 */
    int have_filter = wm && dlsym(wm, "OH_NativeWindowManager_RegisterKeyEventFilter") ? 1 : 0;
    if (have_filter) {
        deliver_from(0, "INPUT_FILTER_AVAILABLE 1");
    }
    if (input) dlclose(input);
    if (wm) dlclose(wm);
    napi_value value; napi_create_int32(env, result, &value); return value;
}


/* ------------------------------- 窗口级按键过滤器（NDK，公开头已确认签名）
 * WindowManager_ErrorCode OH_NativeWindowManager_RegisterKeyEventFilter(int32_t windowId,
 *     bool (*)(Input_KeyEvent*));
 * 先探"什么 windowId 能注册成功"，回调里带 stderr 痕迹，这样注册与派发能分开看。 */
typedef int32_t (*RegisterKeyFilter)(int32_t windowId, void *callback);
typedef int32_t (*UnregisterKeyFilter)(int32_t windowId);

static RegisterKeyFilter g_register_key_filter = NULL;
static UnregisterKeyFilter g_unregister_key_filter = NULL;
static int g_filter_window = -1;
static int g_filter_windows[8];
static int g_filter_count = 0;

static bool engine_key_filter(void *event) {
    int owner = -1;
    if (event && g_get_key_window) owner = g_get_key_window(event);
    fprintf(stderr, "l410-key-filter: owner=%d registered=%d event=%p\n", owner, g_filter_count, event);
    fflush(stderr);
    if (!event || !g_get_key_code || !g_get_key_action) return false;
    const int code = g_get_key_code(event);
    const int action = g_get_key_action(event);
    char line[96];
    snprintf(line, sizeof(line), "KEYEVENT action=%d code=%d window=%d", action, code,
             owner > 0 ? owner : g_filter_window);
    deliver_from(0, line);
    return false;       /* 不消费：应用那边照旧能收到 */
}

static napi_value key_filter_probe(napi_env env, napi_callback_info info) {
    napi_value argv[1];
    size_t count = 1;
    int32_t window = 0;
    if (napi_get_cb_info(env, info, &count, argv, NULL, NULL) != napi_ok || count < 1 ||
        napi_get_value_int32(env, argv[0], &window) != napi_ok) {
        napi_value no; napi_create_int32(env, -999, &no); return no;
    }
    void *input = dlopen("libohinput.so", RTLD_NOW);
    if (input) {
        g_get_key_code = (GetKeyCode)dlsym(input, "OH_Input_GetKeyEventKeyCode");
        g_get_key_action = (GetKeyAction)dlsym(input, "OH_Input_GetKeyEventAction");
    }
    if (!g_register_key_filter) {
        void *wm = dlopen("libnative_window_manager.so", RTLD_NOW);
        if (wm) {
            g_register_key_filter = (RegisterKeyFilter)dlsym(wm, "OH_NativeWindowManager_RegisterKeyEventFilter");
            g_unregister_key_filter = (UnregisterKeyFilter)dlsym(wm, "OH_NativeWindowManager_UnregisterKeyEventFilter");
        }
    }
    if (!g_register_key_filter) {
        napi_value no; napi_create_int32(env, -998, &no); return no;
    }
    /* 允许多个窗口同时注册：我们要看键盘到底被投给哪个窗口。 */
    for (int i = 0; i < g_filter_count; i++) {
        if (g_filter_windows[i] == window) {
            napi_value again; napi_create_int32(env, 0, &again); return again;
        }
    }
    const int32_t result = g_register_key_filter(window, (void *)engine_key_filter);
    if (result == 0 && g_filter_count < 8) g_filter_windows[g_filter_count++] = window;
    napi_value value; napi_create_int32(env, result, &value); return value;
}


/* --------------------------- UI 侧输入桥（跨能力/跨 VM）
 * 应用窗口将来由独立的能力实例承载（各自一个 JS VM），那边的输入层拿不到宿主能力里的引擎通道。
 * 这里在宿主侧多开一个抽象 socket，窗口侧加载同一个模块、连上来发事件；宿主收到后照常投给 JS。 */
static int ui_listen_fd = -1;
/* The HAP loads this .so in several JS VMs, but ELF static storage is shared
 * process-wide.  Keep the accepted server endpoint and the sender endpoint
 * separate.  Reusing one variable made uiConnect() store the client socket,
 * then accept() overwrite it with the server socket; uiSend() consequently
 * wrote in the wrong direction and every ArkUI pointer/key event vanished. */
static int ui_sender_fd = -1;
static char ui_endpoint[96];
static pthread_t ui_thread;
static _Atomic int ui_stop = 0;

static void *ui_bridge_thread(void *arg) {
    (void)arg;
    while (!__atomic_load_n(&ui_stop, __ATOMIC_ACQUIRE)) {
        struct pollfd waiting = {ui_listen_fd, POLLIN, 0};
        if (poll(&waiting, 1, 200) <= 0) continue;
        int client = accept4(ui_listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) continue;
        struct timeval timeout = {1, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        char buffer[1024];
        size_t used = 0;
        while (!__atomic_load_n(&ui_stop, __ATOMIC_ACQUIRE)) {
            ssize_t got = recv(client, buffer + used, sizeof(buffer) - used - 1, 0);
            if (got <= 0) {
                if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
                break;
            }
            used += (size_t)got;
            size_t start = 0;
            for (size_t i = 0; i < used; i++) {
                if (buffer[i] != '\n') continue;
                buffer[i] = '\0';
                if (i > start) {
                    char line[1200];
                    snprintf(line, sizeof(line), "UI %s", buffer + start);
                    deliver_from(0, line);
                }
                start = i + 1;
            }
            if (start) {
                memmove(buffer, buffer + start, used - start);
                used -= start;
            }
            if (used >= sizeof(buffer) - 1) used = 0;
        }
        close(client);
    }
    return NULL;
}

static napi_value ui_endpoint_fn(napi_env env, napi_callback_info info) {
    (void)info;
    pthread_mutex_lock(&registry_lock);
    if (ui_listen_fd < 0) {
        snprintf(ui_endpoint, sizeof(ui_endpoint), "l410-engine-ui-%ld", (long)getpid());
        ui_listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (ui_listen_fd >= 0) {
            struct sockaddr_un address;
            memset(&address, 0, sizeof(address));
            address.sun_family = AF_UNIX;
            size_t length = strlen(ui_endpoint);
            memcpy(address.sun_path + 1, ui_endpoint, length);
            if (bind(ui_listen_fd, (void *)&address,
                     offsetof(struct sockaddr_un, sun_path) + 1 + length) ||
                listen(ui_listen_fd, 4)) {
                close(ui_listen_fd);
                ui_listen_fd = -1;
                ui_endpoint[0] = '\0';
            } else {
                __atomic_store_n(&ui_stop, 0, __ATOMIC_RELEASE);
                if (pthread_create(&ui_thread, NULL, ui_bridge_thread, NULL)) {
                    close(ui_listen_fd);
                    ui_listen_fd = -1;
                    ui_endpoint[0] = '\0';
                }
            }
        }
    }
    napi_value result;
    napi_create_string_utf8(env, ui_endpoint, NAPI_AUTO_LENGTH, &result);
    pthread_mutex_unlock(&registry_lock);
    return result;
}

/* 窗口能力侧：连到宿主能力的桥，然后把输入行发过去。 */
static napi_value ui_connect(napi_env env, napi_callback_info info) {
    napi_value argv[1];
    size_t count = 1;
    char endpoint[96] = {0};
    size_t length = 0;
    if (napi_get_cb_info(env, info, &count, argv, NULL, NULL) != napi_ok || count < 1 ||
        napi_get_value_string_utf8(env, argv[0], endpoint, sizeof(endpoint), &length) != napi_ok || !length) {
        napi_value no; napi_get_boolean(env, 0, &no); return no;
    }
    int ok = 0;
    /* All window VMs belong to this one process and send to the same host
     * endpoint.  A single process-wide connection also lets the single reader
     * thread serve any number of Linux top-level windows. */
    int current = __atomic_load_n(&ui_sender_fd, __ATOMIC_ACQUIRE);
    if (current >= 0) {
        napi_value result;
        napi_get_boolean(env, 1, &result);
        return result;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        struct sockaddr_un address;
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path + 1, endpoint, length);
        if (!connect(fd, (void *)&address, offsetof(struct sockaddr_un, sun_path) + 1 + length)) {
            __atomic_store_n(&ui_sender_fd, fd, __ATOMIC_RELEASE);
            ok = 1;
        } else {
            close(fd);
        }
    }
    napi_value result;
    napi_get_boolean(env, ok, &result);
    return result;
}

static napi_value ui_send(napi_env env, napi_callback_info info) {
    napi_value argv[1];
    size_t count = 1;
    char line[1024] = {0};
    size_t length = 0;
    if (napi_get_cb_info(env, info, &count, argv, NULL, NULL) != napi_ok || count < 1 ||
        napi_get_value_string_utf8(env, argv[0], line, sizeof(line) - 2, &length) != napi_ok || !length) {
        napi_value no; napi_get_boolean(env, 0, &no); return no;
    }
    line[length] = '\n';
    int fd = __atomic_load_n(&ui_sender_fd, __ATOMIC_ACQUIRE);
    int ok = 0;
    if (fd >= 0) {
        size_t done = 0;
        ok = 1;
        while (done < length + 1) {
            ssize_t written = send(fd, line + done, length + 1 - done, MSG_NOSIGNAL);
            if (written <= 0) {
                if (errno == EINTR) continue;
                ok = 0;
                if (__atomic_compare_exchange_n(&ui_sender_fd, &fd, -1, 0,
                                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) close(fd);
                break;
            }
            done += (size_t)written;
        }
    }
    napi_value result;
    napi_get_boolean(env, ok, &result);
    return result;
}


/* ------------------- 窗口级输入过滤器（键/鼠标/触摸三件套）
 * 真实应用窗口由窗口管理器按窗口派发输入；注册过滤器后，回调里就能直接拿到事件，
 * 完全不经过 ArkUI 控件（这正是卓易通容器把输入按窗口接进去的做法）。 */
typedef int32_t (*RegisterMouseFilter)(int32_t windowId, void *callback);
typedef int32_t (*RegisterTouchFilter)(int32_t windowId, void *callback);
typedef int32_t (*GetMouseAction)(const void *event);
typedef int32_t (*GetMouseDisplayX)(const void *event);
typedef int32_t (*GetMouseDisplayY)(const void *event);
typedef int32_t (*GetMouseButton)(const void *event);
typedef int32_t (*GetMouseWindowId)(const void *event);
typedef int32_t (*GetTouchAction)(const void *event);
typedef int32_t (*GetTouchDisplayX)(const void *event);
typedef int32_t (*GetTouchDisplayY)(const void *event);
typedef int32_t (*GetTouchWindowId)(const void *event);

static RegisterMouseFilter g_register_mouse_filter = NULL;
static RegisterTouchFilter g_register_touch_filter = NULL;
static GetMouseAction g_get_mouse_action = NULL;
static GetMouseDisplayX g_get_mouse_x = NULL;
static GetMouseDisplayY g_get_mouse_y = NULL;
static GetMouseButton g_get_mouse_button = NULL;
static GetMouseWindowId g_get_mouse_window = NULL;
static GetTouchAction g_get_touch_action = NULL;
static GetTouchDisplayX g_get_touch_x = NULL;
static GetTouchDisplayY g_get_touch_y = NULL;
static GetTouchWindowId g_get_touch_window = NULL;

static bool engine_mouse_filter(void *event) {
    if (!event || !g_get_mouse_action) return false;
    const int action = g_get_mouse_action(event);
    const int x = g_get_mouse_x ? g_get_mouse_x(event) : 0;
    const int y = g_get_mouse_y ? g_get_mouse_y(event) : 0;
    const int button = g_get_mouse_button ? g_get_mouse_button(event) : 0;
    const int owner = g_get_mouse_window ? g_get_mouse_window(event) : -1;
    char line[128];
    snprintf(line, sizeof(line), "MOUSEEVENT action=%d x=%d y=%d button=%d window=%d",
             action, x, y, button, owner);
    deliver_from(0, line);
    return false;
}

static bool engine_touch_filter(void *event) {
    if (!event || !g_get_touch_action) return false;
    const int action = g_get_touch_action(event);
    const int x = g_get_touch_x ? g_get_touch_x(event) : 0;
    const int y = g_get_touch_y ? g_get_touch_y(event) : 0;
    const int owner = g_get_touch_window ? g_get_touch_window(event) : -1;
    char line[128];
    snprintf(line, sizeof(line), "TOUCHEVENT action=%d x=%d y=%d window=%d", action, x, y, owner);
    deliver_from(0, line);
    return false;
}

static napi_value window_filters(napi_env env, napi_callback_info info) {
    napi_value argv[1];
    size_t count = 1;
    int32_t window = 0;
    if (napi_get_cb_info(env, info, &count, argv, NULL, NULL) != napi_ok || count < 1 ||
        napi_get_value_int32(env, argv[0], &window) != napi_ok || window <= 0) {
        napi_value no; napi_create_int32(env, -1, &no); return no;
    }
    void *input = dlopen("libohinput.so", RTLD_NOW);
    if (input) {
        if (!g_get_key_code) g_get_key_code = (GetKeyCode)dlsym(input, "OH_Input_GetKeyEventKeyCode");
        if (!g_get_key_action) g_get_key_action = (GetKeyAction)dlsym(input, "OH_Input_GetKeyEventAction");
        if (!g_get_key_window) g_get_key_window = (GetKeyWindowId)dlsym(input, "OH_Input_GetKeyEventWindowId");
        g_get_mouse_action = (GetMouseAction)dlsym(input, "OH_Input_GetMouseEventAction");
        g_get_mouse_x = (GetMouseDisplayX)dlsym(input, "OH_Input_GetMouseEventDisplayX");
        g_get_mouse_y = (GetMouseDisplayY)dlsym(input, "OH_Input_GetMouseEventDisplayY");
        g_get_mouse_button = (GetMouseButton)dlsym(input, "OH_Input_GetMouseEventButton");
        g_get_mouse_window = (GetMouseWindowId)dlsym(input, "OH_Input_GetMouseEventWindowId");
        g_get_touch_action = (GetTouchAction)dlsym(input, "OH_Input_GetTouchEventAction");
        g_get_touch_x = (GetTouchDisplayX)dlsym(input, "OH_Input_GetTouchEventDisplayX");
        g_get_touch_y = (GetTouchDisplayY)dlsym(input, "OH_Input_GetTouchEventDisplayY");
        g_get_touch_window = (GetTouchWindowId)dlsym(input, "OH_Input_GetTouchEventWindowId");
    }
    void *wm = dlopen("libnative_window_manager.so", RTLD_NOW);
    if (wm) {
        if (!g_register_key_filter)
            g_register_key_filter = (RegisterKeyFilter)dlsym(wm, "OH_NativeWindowManager_RegisterKeyEventFilter");
        g_register_mouse_filter = (RegisterMouseFilter)dlsym(wm, "OH_NativeWindowManager_RegisterMouseEventFilter");
        g_register_touch_filter = (RegisterTouchFilter)dlsym(wm, "OH_NativeWindowManager_RegisterTouchEventFilter");
    }
    int key_result = -1, mouse_result = -1, touch_result = -1;
    if (g_register_key_filter) {
        g_filter_window = window;
        key_result = g_register_key_filter(window, (void *)engine_key_filter);
    }
    if (g_register_mouse_filter) mouse_result = g_register_mouse_filter(window, (void *)engine_mouse_filter);
    if (g_register_touch_filter) touch_result = g_register_touch_filter(window, (void *)engine_touch_filter);
    fprintf(stderr, "l410-window-filters: window=%d key=%d mouse=%d touch=%d\n",
            window, key_result, mouse_result, touch_result);
    fflush(stderr);
    if (input) dlclose(input);
    if (wm) dlclose(wm);
    /* 按位上报：1=键过滤器成功 2=鼠标 4=触摸（都不成返回 -1）。 */
    int mask = (key_result == 0 ? 1 : 0) | (mouse_result == 0 ? 2 : 0) | (touch_result == 0 ? 4 : 0);
    napi_value value;
    napi_create_int32(env, mask ? mask : -1, &value);
    return value;
}


/* 探针：anco 的 GraphicBrokerClient 能不能拿到并初始化。
 * 卓易通就是靠它把容器的 Surface 变成系统窗口（libgraphic_broker_client.z.so）。 */
static napi_value anco_probe(napi_env env, napi_callback_info info) {
    (void)info;
    void *handle = dlopen("libgraphic_broker_client.z.so", RTLD_NOW);
    if (!handle) handle = dlopen("/system/lib64/libgraphic_broker_client.z.so", RTLD_NOW);
    char line[256];
    if (!handle) {
        snprintf(line, sizeof(line), "ANCO_PROBE load_failed %s", dlerror() ? dlerror() : "?");
        deliver_from(0, line);
        napi_value no; napi_create_int32(env, -1, &no); return no;
    }
    void *(*get_instance)(void) = dlsym(handle, "_ZN4OHOS4ANCO19GraphicBrokerClient11GetInstanceEv");
    int (*init)(void *) = dlsym(handle, "_ZN4OHOS4ANCO19GraphicBrokerClient4InitEv");
    int (*set_status)(void *, int) = dlsym(handle, "_ZN4OHOS4ANCO19GraphicBrokerClient13SetAncoStatusEi");
    int (*set_surface)(void *, long, void *) = dlsym(handle,
        "_ZN4OHOS4ANCO19GraphicBrokerClient22SetSurfaceWithWindowIDElRNS_4sptrINS_7SurfaceEEE");
    snprintf(line, sizeof(line), "ANCO_PROBE symbols instance=%d init=%d status=%d surface=%d",
             get_instance ? 1 : 0, init ? 1 : 0, set_status ? 1 : 0, set_surface ? 1 : 0);
    deliver_from(0, line);
    int result = -99;
    if (get_instance) {
        void *client = get_instance();
        snprintf(line, sizeof(line), "ANCO_PROBE instance=%p", client);
        deliver_from(0, line);
        if (client && init) {
            result = init(client);
            snprintf(line, sizeof(line), "ANCO_PROBE init_result=%d", result);
            deliver_from(0, line);
        }
        if (client && set_status && result != 0) {
            int status = set_status(client, 1);
            snprintf(line, sizeof(line), "ANCO_PROBE set_status_result=%d", status);
            deliver_from(0, line);
        } else if (client && result == 0) {
            deliver_from(0, "ANCO_PROBE broker_unavailable sa=66829");
        }
    }
    napi_value value; napi_create_int32(env, result, &value); return value;
}


/* Strongly typed OHOS C++ adapter, loaded from the same HAP native-library
 * directory.  Keeping this boundary in C lets the N-API module stay plain C
 * while the adapter uses the platform's real libc++/sptr implementation. */
static int32_t anco_typed_stage = 0;
static int32_t anco_typed_result = -999;
static void *anco_probe_surface = NULL;
extern void *l410_anco_surface_create_typed(const char *name, int32_t width, int32_t height);
extern int32_t l410_anco_surface_publish_typed(void *handle, int32_t window_id);
extern void *l410_anco_surface_producer_typed(void *handle);
extern const void *l410_anco_surface_queue_ref_typed(void *handle);
extern int32_t l410_anco_surface_publish_raw_typed(const void *queue_ref, int32_t window_id);
extern int32_t l410_anco_surface_destroy_typed(void *handle);
extern int32_t l410_anco_surface_last_create_stage(void);
extern void *l410_anco_surface_native_window_typed(void *handle);
extern int32_t l410_anco_surface_id_typed(void *handle, uint64_t *surface_id);
struct L410NativeBufferInfo {
    int32_t fd, width, stride, height, size, format;
    uint64_t usage;
};
extern int32_t l410_anco_surface_configure_typed(void *handle, int32_t width, int32_t height);
extern int32_t l410_anco_surface_request_buffer_typed(void *handle, void **buffer,
    int32_t *fence_fd, struct L410NativeBufferInfo *info);
extern int32_t l410_anco_surface_flush_buffer_typed(void *handle, void *buffer, int32_t fence_fd);
extern int32_t l410_anco_surface_abort_buffer_typed(void *handle, void *buffer);
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
extern int32_t l410_anco_lifecycle_start(
    void (*callback)(const struct L410AncoLifecycleEvent *, void *), void *context);
extern int32_t l410_anco_lifecycle_stop(void);
extern int32_t l410_anco_surface_node_create_typed(
    uint64_t display_id, int32_t window_id, const char *name);
extern int32_t l410_anco_surface_node_create_delegated_typed(
    uint64_t display_id, int32_t window_id, const char *name, void *remote_object);
extern void *l410_anco_surface_delegator_remote_typed(void *handle);
extern int32_t l410_anco_surface_delegator_register_result_typed(void *handle);
extern int32_t l410_anco_surface_node_remove_typed(
    uint64_t display_id, int32_t window_id);
extern int l410_broker_window_create(int32_t window_id, int32_t width, int32_t height,
                                     char *endpoint, size_t endpoint_size, uint64_t *surface_id);
extern int l410_broker_window_destroy(int32_t window_id);
extern int l410_broker_window_status(int32_t window_id, char *text, size_t size);
extern int l410_broker_window_probe_queue(const char *endpoint, int32_t width, int32_t height);

static void anco_lifecycle_event(
    const struct L410AncoLifecycleEvent *event, void *context) {
    (void)context;
    if (!event) return;
    char line[384];
    snprintf(line, sizeof(line),
             "ANCO_LIFECYCLE type=%d display=%llu window=%d task=%d x=%d y=%d w=%u h=%u op=%d relative=%llu value=%d name=%s",
             event->type, (unsigned long long)event->displayId, event->windowId,
             event->taskId, event->x, event->y, event->width, event->height,
             event->operation, (unsigned long long)event->relativeId,
             event->value, event->name);
    deliver_from(0, line);
}

static napi_value anco_lifecycle_start(napi_env env, napi_callback_info info) {
    (void)info;
    int32_t result = l410_anco_lifecycle_start(anco_lifecycle_event, NULL);
    char line[80];
    snprintf(line, sizeof(line), "ANCO_LIFECYCLE_REGISTER result=%d", result);
    deliver_from(0, line);
    napi_value value; napi_create_int32(env, result, &value); return value;
}

static napi_value anco_lifecycle_stop(napi_env env, napi_callback_info info) {
    (void)info;
    int32_t result = l410_anco_lifecycle_stop();
    napi_value value; napi_create_int32(env, result, &value); return value;
}

static napi_value anco_surface_node_create(napi_env env, napi_callback_info info) {
    napi_value argv[2];
    size_t count = 2;
    int32_t window_id = 0;
    int64_t surface_view_id = 0;
    if (napi_get_cb_info(env, info, &count, argv, NULL, NULL) != napi_ok || count < 1 ||
        napi_get_value_int32(env, argv[0], &window_id) != napi_ok) window_id = 0;
    if (count >= 2) napi_get_value_int64(env, argv[1], &surface_view_id);
    int32_t result = window_id > 0 ? l410_anco_surface_node_create_typed(
        (uint64_t)surface_view_id, window_id, "l410-surface-node-probe") : -1;
    char line[160];
    snprintf(line, sizeof(line), "ANCO_SURFACE_NODE_CREATE window=%d surfaceView=%lld result=%d",
        window_id, (long long)surface_view_id, result);
    deliver_from(0, line);
    napi_value value; napi_create_int32(env, result, &value); return value;
}

static napi_value anco_surface_node_remove(napi_env env, napi_callback_info info) {
    napi_value argv[1];
    size_t count = 1;
    int32_t window_id = 0;
    if (napi_get_cb_info(env, info, &count, argv, NULL, NULL) != napi_ok || count != 1 ||
        napi_get_value_int32(env, argv[0], &window_id) != napi_ok) window_id = 0;
    int32_t result = window_id > 0 ? l410_anco_surface_node_remove_typed(0, window_id) : -1;
    char line[112];
    snprintf(line, sizeof(line), "ANCO_SURFACE_NODE_REMOVE window=%d result=%d", window_id, result);
    deliver_from(0, line);
    napi_value value; napi_create_int32(env, result, &value); return value;
}

static napi_value broker_window_create(napi_env env, napi_callback_info info) {
    napi_value argv[3];
    size_t count = 3;
    int32_t window_id = 0, width = 0, height = 0;
    if (napi_get_cb_info(env, info, &count, argv, NULL, NULL) != napi_ok || count != 3 ||
        napi_get_value_int32(env, argv[0], &window_id) != napi_ok ||
        napi_get_value_int32(env, argv[1], &width) != napi_ok ||
        napi_get_value_int32(env, argv[2], &height) != napi_ok) {
        napi_value bad; napi_create_string_utf8(env, "{\"result\":-1}", NAPI_AUTO_LENGTH, &bad); return bad;
    }
    char endpoint[96] = {0};
    uint64_t surface_id = 0;
    int result = l410_broker_window_create(window_id, width, height,
                                            endpoint, sizeof(endpoint), &surface_id);
    char json[256];
    snprintf(json, sizeof(json),
             "{\"result\":%d,\"endpoint\":\"%s\",\"surfaceId\":\"%llu\"}",
             result, endpoint, (unsigned long long)surface_id);
    char line[320];
    snprintf(line, sizeof(line), "BROKER_WINDOW_CREATE window=%d result=%d sid=%llu endpoint=%s",
             window_id, result, (unsigned long long)surface_id, endpoint);
    deliver_from(0, line);
    napi_value value; napi_create_string_utf8(env, json, NAPI_AUTO_LENGTH, &value); return value;
}

/* Bounded end-to-end queue probe for the AnCo delegator ABI.  The device's
 * OpenHarmony 6.0 libsurface delegator methods are placeholders, so broker
 * success alone proves object publication, not a CLIENT/buffer transaction. */
static napi_value anco_direct_queue_probe(napi_env env, napi_callback_info info) {
    (void)info;
    const int32_t window_id = 42002;
    uint64_t surface_id = 0;
    void *surface = l410_anco_surface_create_typed(
        "l410-direct-delegated-probe", 320, 240);
    int32_t stage = l410_anco_surface_last_create_stage();
    int32_t surface_id_rc = surface ?
        l410_anco_surface_id_typed(surface, &surface_id) : -90;
    int32_t node = surface ? l410_anco_surface_node_create_delegated_typed(
        0, window_id, "l410-direct-delegated-probe",
        l410_anco_surface_delegator_remote_typed(surface)) : -90;
    int32_t raw_publish = node == 0 ?
        l410_anco_surface_publish_raw_typed(
            l410_anco_surface_queue_ref_typed(surface), window_id) : -90;
    int32_t published = raw_publish == 0 ? 0 : -3;
    int32_t configured = published == 0 ?
        l410_anco_surface_configure_typed(surface, 320, 240) : -90;
    void *buffer = NULL;
    int32_t fence_fd = -1;
    struct L410NativeBufferInfo buffer_info = {0};
    int32_t requested = configured == 0 ?
        l410_anco_surface_request_buffer_typed(
            surface, &buffer, &fence_fd, &buffer_info) : -90;
    int32_t registered = surface ?
        l410_anco_surface_delegator_register_result_typed(surface) : INT32_MIN;
    if (fence_fd >= 0) close(fence_fd);
    int32_t aborted = requested == 0 ?
        l410_anco_surface_abort_buffer_typed(surface, buffer) : -90;
    int32_t destroyed = surface ? l410_anco_surface_destroy_typed(surface) : 0;
    int32_t removed = node == 0 ? l410_anco_surface_node_remove_typed(0, window_id) : 0;
    char line[512];
    snprintf(line, sizeof(line),
        "ANCO_DIRECT_QUEUE_PROBE stage=%d sidrc=%d node=%d publish=%d raw_publish=%d configure=%d register=%d request=%d abort=%d destroyed=%d removed=%d sid=%llu extent=%dx%d stride=%d format=%d",
        stage, surface_id_rc, node, published, raw_publish, configured, registered, requested, aborted,
        destroyed, removed, (unsigned long long)surface_id, buffer_info.width,
        buffer_info.height, buffer_info.stride, buffer_info.format);
    deliver_from(0, line);
    napi_value value; napi_create_string_utf8(env, line, NAPI_AUTO_LENGTH, &value); return value;
}

static napi_value broker_window_destroy(napi_env env, napi_callback_info info) {
    napi_value argv[1];
    size_t count = 1;
    int32_t window_id = 0;
    if (napi_get_cb_info(env, info, &count, argv, NULL, NULL) != napi_ok || count != 1 ||
        napi_get_value_int32(env, argv[0], &window_id) != napi_ok) window_id = 0;
    int result = window_id > 0 ? l410_broker_window_destroy(window_id) : -1;
    char line[96];
    snprintf(line, sizeof(line), "BROKER_WINDOW_DESTROY window=%d result=%d", window_id, result);
    deliver_from(0, line);
    napi_value value; napi_create_int32(env, result, &value); return value;
}

static napi_value broker_window_status(napi_env env, napi_callback_info info) {
    napi_value argv[1];
    size_t count = 1;
    int32_t window_id = 0;
    if (napi_get_cb_info(env, info, &count, argv, NULL, NULL) != napi_ok || count != 1 ||
        napi_get_value_int32(env, argv[0], &window_id) != napi_ok) window_id = 0;
    char json[320];
    l410_broker_window_status(window_id, json, sizeof(json));
    napi_value value; napi_create_string_utf8(env, json, NAPI_AUTO_LENGTH, &value); return value;
}

static napi_value anco_surface_probe(napi_env env, napi_callback_info info) {
    napi_value argv[1];
    size_t count = 1;
    int32_t window_id = 1001;
    if (napi_get_cb_info(env, info, &count, argv, NULL, NULL) == napi_ok && count >= 1)
        napi_get_value_int32(env, argv[0], &window_id);
    char line[384];
    void *surface = anco_probe_surface;
    if (!surface) surface = l410_anco_surface_create_typed("l410-engine-probe", 800, 600);
    anco_typed_stage = l410_anco_surface_last_create_stage();
    uint64_t surface_id = 0;
    int32_t surface_id_rc = surface ? l410_anco_surface_id_typed(surface, &surface_id) : -1;
    int32_t native_window = surface && l410_anco_surface_native_window_typed(surface) ? 1 : 0;
    anco_typed_result = surface ? l410_anco_surface_publish_typed(surface, window_id) : -11;
    int32_t configured = anco_typed_result == 0 ?
        l410_anco_surface_configure_typed(surface, 800, 600) : -90;
    void *buffer = NULL;
    int32_t fence_fd = -1;
    struct L410NativeBufferInfo buffer_info = {0};
    int32_t requested = configured == 0 ? l410_anco_surface_request_buffer_typed(
        surface, &buffer, &fence_fd, &buffer_info) : -90;
    int32_t flushed = -90;
    if (requested == 0) {
        if (fence_fd >= 0) {
            struct pollfd wait_fence = {fence_fd, POLLIN, 0};
            poll(&wait_fence, 1, 1000);
            close(fence_fd);
            fence_fd = -1;
        }
        void *mapped = mmap(NULL, (size_t)buffer_info.size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, buffer_info.fd, 0);
        if (mapped != MAP_FAILED) {
            uint32_t *pixels = mapped;
            size_t pitch = (size_t)buffer_info.stride / sizeof(uint32_t);
            for (int32_t y = 0; y < buffer_info.height; y++) {
                for (int32_t x = 0; x < buffer_info.width; x++) {
                    uint32_t red = 32u + (uint32_t)x * 192u / (uint32_t)buffer_info.width;
                    uint32_t green = 32u + (uint32_t)y * 192u / (uint32_t)buffer_info.height;
                    pixels[(size_t)y * pitch + (size_t)x] =
                        0xff000040u | (red << 16) | (green << 8);
                }
            }
            msync(mapped, (size_t)buffer_info.size, MS_SYNC);
            munmap(mapped, (size_t)buffer_info.size);
            flushed = l410_anco_surface_flush_buffer_typed(surface, buffer, -1);
        } else {
            flushed = -91;
            l410_anco_surface_abort_buffer_typed(surface, buffer);
        }
    }
    int32_t retained = surface && anco_typed_result == 0 ? 1 : 0;
    if (retained) anco_probe_surface = surface;
    else if (surface) l410_anco_surface_destroy_typed(surface);
    snprintf(line, sizeof(line),
             "ANCO_TYPED stage=%d native=%d sid_rc=%d sid=%llu publish=%d configure=%d request=%d flush=%d extent=%dx%d stride=%d format=%d retained=%d window=%d",
             anco_typed_stage, native_window, surface_id_rc, (unsigned long long)surface_id,
             anco_typed_result, configured, requested, flushed, buffer_info.width,
             buffer_info.height, buffer_info.stride, buffer_info.format, retained, window_id);
    deliver_from(0, line);
    napi_value value; napi_create_string_utf8(env, line, NAPI_AUTO_LENGTH, &value); return value;
}

/* 阶段查询：1=构造 ConsumerSurface 2/3=GetProducer/Create 4=create 失败 5=已拿到 surface 9=完成 */
static napi_value anco_surface_stage(napi_env env, napi_callback_info info) {
    (void)info;
    char line[256];
    snprintf(line, sizeof(line), "ANCO_TYPED stage=%d result=%d",
             anco_typed_stage, anco_typed_result);
    deliver_from(0, line);
    napi_value value; napi_create_int32(env, anco_typed_stage, &value); return value;
}

static napi_value init(napi_env env, napi_value exports) {
    /* 同一个进程里，第二个 JS VM（例如窗口能力页面）再加载这个 .so 时会**再次**调用 Init，
       而 .so 的静态数据是共享的：以前这里无条件把 listen_fd 和所有客户端 fd 清成 -1，
       结果容器应用那条已经建立的连接被从表里抹掉（日志表现为 peers 1 但 send 全部失败）。
       所以只初始化一次，之后不动任何运行期状态。 */
    static int module_initialised = 0;
    if (!module_initialised) {
        module_initialised = 1;
        pthread_mutex_init(&engine.lock, NULL);
        engine.listen_fd = -1;
        for (int i = 0; i < 8; i++) engine.clients[i].fd = -1;
    }
    napi_value fn;
    napi_create_function(env, "start", NAPI_AUTO_LENGTH, start, NULL, &fn);
    napi_set_named_property(env, exports, "start", fn);
    napi_create_function(env, "setListener", NAPI_AUTO_LENGTH, set_listener, NULL, &fn);
    napi_set_named_property(env, exports, "setListener", fn);
    napi_create_function(env, "send", NAPI_AUTO_LENGTH, send_line, NULL, &fn);
    napi_set_named_property(env, exports, "send", fn);
    napi_create_function(env, "status", NAPI_AUTO_LENGTH, status, NULL, &fn);
    napi_set_named_property(env, exports, "status", fn);
    napi_create_function(env, "noteFrame", NAPI_AUTO_LENGTH, note_frame, NULL, &fn);
    napi_set_named_property(env, exports, "noteFrame", fn);
    napi_create_function(env, "launch", NAPI_AUTO_LENGTH, launch_app, NULL, &fn);
    napi_set_named_property(env, exports, "launch", fn);
    napi_create_function(env, "envProbe", NAPI_AUTO_LENGTH, env_probe, NULL, &fn);
    napi_set_named_property(env, exports, "envProbe", fn);
    napi_create_function(env, "serviceSend", NAPI_AUTO_LENGTH, service_send, NULL, &fn);
    napi_set_named_property(env, exports, "serviceSend", fn);
    napi_create_function(env, "inputMonitor", NAPI_AUTO_LENGTH, input_monitor, NULL, &fn);
    napi_set_named_property(env, exports, "inputMonitor", fn);
    napi_create_function(env, "keyFilterProbe", NAPI_AUTO_LENGTH, key_filter_probe, NULL, &fn);
    napi_set_named_property(env, exports, "keyFilterProbe", fn);
    napi_create_function(env, "ancoSurfaceProbe", NAPI_AUTO_LENGTH, anco_surface_probe, NULL, &fn);
    napi_set_named_property(env, exports, "ancoSurfaceProbe", fn);
    napi_create_function(env, "ancoSurfaceStage", NAPI_AUTO_LENGTH, anco_surface_stage, NULL, &fn);
    napi_set_named_property(env, exports, "ancoSurfaceStage", fn);
    napi_create_function(env, "brokerWindowCreate", NAPI_AUTO_LENGTH, broker_window_create, NULL, &fn);
    napi_set_named_property(env, exports, "brokerWindowCreate", fn);
    napi_create_function(env, "brokerWindowDestroy", NAPI_AUTO_LENGTH, broker_window_destroy, NULL, &fn);
    napi_set_named_property(env, exports, "brokerWindowDestroy", fn);
    napi_create_function(env, "brokerWindowStatus", NAPI_AUTO_LENGTH, broker_window_status, NULL, &fn);
    napi_set_named_property(env, exports, "brokerWindowStatus", fn);
    napi_create_function(env, "ancoDirectQueueProbe", NAPI_AUTO_LENGTH, anco_direct_queue_probe, NULL, &fn);
    napi_set_named_property(env, exports, "ancoDirectQueueProbe", fn);
    napi_create_function(env, "ancoLifecycleStart", NAPI_AUTO_LENGTH, anco_lifecycle_start, NULL, &fn);
    napi_set_named_property(env, exports, "ancoLifecycleStart", fn);
    napi_create_function(env, "ancoLifecycleStop", NAPI_AUTO_LENGTH, anco_lifecycle_stop, NULL, &fn);
    napi_set_named_property(env, exports, "ancoLifecycleStop", fn);
    napi_create_function(env, "ancoSurfaceNodeCreate", NAPI_AUTO_LENGTH, anco_surface_node_create, NULL, &fn);
    napi_set_named_property(env, exports, "ancoSurfaceNodeCreate", fn);
    napi_create_function(env, "ancoSurfaceNodeRemove", NAPI_AUTO_LENGTH, anco_surface_node_remove, NULL, &fn);
    napi_set_named_property(env, exports, "ancoSurfaceNodeRemove", fn);
    napi_create_function(env, "ancoProbe", NAPI_AUTO_LENGTH, anco_probe, NULL, &fn);
    napi_set_named_property(env, exports, "ancoProbe", fn);
    napi_create_function(env, "windowFilters", NAPI_AUTO_LENGTH, window_filters, NULL, &fn);
    napi_set_named_property(env, exports, "windowFilters", fn);
    napi_create_function(env, "uiEndpoint", NAPI_AUTO_LENGTH, ui_endpoint_fn, NULL, &fn);
    napi_set_named_property(env, exports, "uiEndpoint", fn);
    napi_create_function(env, "uiConnect", NAPI_AUTO_LENGTH, ui_connect, NULL, &fn);
    napi_set_named_property(env, exports, "uiConnect", fn);
    napi_create_function(env, "uiSend", NAPI_AUTO_LENGTH, ui_send, NULL, &fn);
    napi_set_named_property(env, exports, "uiSend", fn);
    napi_create_function(env, "stop", NAPI_AUTO_LENGTH, stop, NULL, &fn);
    napi_set_named_property(env, exports, "stop", fn);
    return exports;
}

static napi_module engine_module = {1, 0, NULL, init, "l410engine", NULL, {0}};
__attribute__((constructor)) static void register_module(void) { napi_module_register(&engine_module); }
