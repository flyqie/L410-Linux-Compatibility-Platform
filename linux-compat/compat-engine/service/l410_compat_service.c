/* L410 兼容引擎：容器生命周期系统服务（对应 anco 里 init 拉起的 anco_service_broker）。
 *
 * 为什么必须是系统服务：应用沙箱的挂载命名空间里看不到容器（/data/local/tmp/... 与
 * /data/vendor/l410-linux/... 在 HAP 里全部不存在），所以"把容器里的应用拉起来"这件事
 * 只能由 root、在宿主命名空间里的常驻进程做。HAP 那一侧只管窗口/合成/输入/输入法/剪贴板。
 *
 * 接口：抽象 AF_UNIX 套接字 @l410-compat-service，一行一条命令（不引入 HTTP）：
 *   APP_START name=<featherpad|mousepad> endpoint=<l410-engine-<pid>>   -> OK <pid> / ERR <why>
 *   APP_STOP  name=<...>                                                 -> OK <n>   （杀掉该应用的进程）
 *   PING                                                                 -> OK
 * 另外支持 `--once "<命令>"`：跑一条命令就退出，方便在设备上手工自检。
 *
 * 拉起应用时直接 exec 容器入口，不经过 shell：应用名来自固定表，环境按工具包分好。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define SERVICE_NAME "l410-compat-service"
#define LD_LOADER "/data/local/tmp/boot-tools-island/lib/ld-linux-aarch64.so.1"
#define LD_LIBPATH "/data/local/tmp/boot-tools-island/lib"
#define ENTER_BIN "/data/vendor/l410-linux/bin/enter"
#define ROOTFS "/data/vendor/l410-linux/rootfs-bookworm"
#define GTK_ROOT "/home/linux/Shared/gtk-build/gtk-build"

/* 容器入口 enter 会把调用方的环境丢掉，所以环境必须在容器里设：
   这个脚本由服务写好放在共享目录里，服务用"脚本 + 参数"的方式拉起应用。 */
static const char *launcher_script =
    "#!/bin/sh\n"
    "# 由 l410-compat-serviced 写好并调用：$1=引擎端点 $2=应用名（服务那边已校验过）。\n"
    "endpoint=$1\n"
    "app=$2\n"
    "export L410_ENGINE_ENDPOINT=\"$endpoint\"\n"
    "export DBUS_SESSION_BUS_ADDRESS=disabled:\n"
    "export GSETTINGS_BACKEND=memory\n"
    "export NO_AT_BRIDGE=1\n"
    "export HOME=/root\n"
    "doc=/tmp/engine-app-test.txt\n"
    "engine=/home/linux/Shared/l410-engine\n"
    "# 容器内输入服务：读 /dev/input 设备事件，应用侧平台后端连它拿输入。\n"
    "# 必须和应用在同一个容器会话里（每次 enter 是独立命名空间），所以在应用之前起。\n"
    "if [ -x $engine/l410-inputd ]; then\n"
    "  $engine/l410-inputd >/tmp/l410-inputd.log 2>&1 &\n"
    "  sleep 0.3\n"
    "fi\n"
    "case \"$app\" in\n"
    "  featherpad)\n"
    "    export QT_QPA_PLATFORM=ohos\n"
    "    export QT_QPA_PLATFORM_PLUGIN_PATH=/opt/l410/ohos-platforms\n"
    "    export QT_QPA_FONTDIR=/usr/share/fonts/truetype\n"
    "    exec /usr/bin/featherpad \"$doc\"\n"
    "    ;;\n"
    "  mousepad)\n"
    "    G=/home/linux/Shared/gtk-build/gtk-build\n"
    "    export GDK_BACKEND=ohos\n"
    "    # Render at the XComponent buffer resolution.  The former logical-size\n"
    "    # path enlarged Cairo pixels and made text jagged.\n"
    "    export L410_OHOS_PIXEL_EXACT=1\n"
    "    export LD_LIBRARY_PATH=$G/gdk:$G/gtk:$G/gdk-pixbuf:$G/gtk/modules\n"
    "    export GDK_PIXBUF_MODULE_FILE=/usr/lib/aarch64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders.cache\n"
    "    export XDG_DATA_DIRS=$G/gtk:/usr/share\n"
    "    export GDK_DPI_SCALE=1.35\n"
    "    exec /usr/bin/mousepad \"$doc\"\n"
    "    ;;\n"
    "esac\n"
    "exit 2\n";

#define LAUNCHER_HOST "/data/vendor/l410-linux/shared/compat-app-run.sh"
#define LAUNCHER_CONTAINER "/home/linux/Shared/compat-app-run.sh"

/* 应用表：只有表里的名字才能被拉起（脚本里的分支与这里一一对应）。 */
static const char *apps[] = {"featherpad", "mousepad"};

static void write_launcher(void) {
    FILE *file = fopen(LAUNCHER_HOST, "w");
    if (!file) return;
    fputs(launcher_script, file);
    fclose(file);
    chmod(LAUNCHER_HOST, 0755);
}

/* 取 key= 后面的一个词。调用方各给各的缓冲区：共用一块静态区会因为求值顺序互相覆盖。 */
static void field(const char *line, const char *key, char *out, size_t size) {
    out[0] = 0;
    size_t length = strlen(key);
    const char *at = line;
    while ((at = strstr(at, key)) != NULL) {
        if (at == line || at[-1] == ' ') break;
        at += length;
    }
    if (!at) return;
    at += length;
    size_t i = 0;
    while (at[i] && at[i] != ' ' && i + 1 < size) { out[i] = at[i]; ++i; }
    out[i] = 0;
}

static int start_app(const char *name, const char *endpoint, char *reply, size_t reply_size) {
    const char *known = NULL;
    const char *base = name ? strrchr(name, '/') : NULL;
    base = base ? base + 1 : name;
    if (!base) {
        snprintf(reply, reply_size, "ERR missing-name");
        return -1;
    }
    for (size_t i = 0; i < sizeof(apps) / sizeof(apps[0]); i++) {
        if (!strcmp(apps[i], base)) { known = apps[i]; break; }
    }
    if (!known) {
        snprintf(reply, reply_size, "ERR unknown-app %s", base);
        return -1;
    }
    if (!endpoint || !*endpoint) {
        snprintf(reply, reply_size, "ERR missing-endpoint");
        return -1;
    }
    write_launcher();

    pid_t child = fork();
    if (child < 0) {
        snprintf(reply, reply_size, "ERR fork %d", errno);
        return -1;
    }
    if (child == 0) {
        setsid();
        /* 环境由容器里的脚本设置（enter 会丢掉调用方的环境），端点与应用名作为参数传进去。 */
        const char *const args[] = {LD_LOADER, "--library-path", LD_LIBPATH, ENTER_BIN, ROOTFS,
                                "--gpu",
                                    "/bin/sh", LAUNCHER_CONTAINER, (char *)endpoint, (char *)known, NULL};
        execv(args[0], (char *const *)args);
        _exit(127);
    }
    snprintf(reply, reply_size, "OK %d", (int)child);
    return 0;
}

static int stop_app(const char *name, char *reply, size_t reply_size) {
    if (!name || !*name) {
        snprintf(reply, reply_size, "ERR missing-name");
        return -1;
    }
    /* 只按绝对路径匹配，绝不误杀别的进程。 */
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "/usr/bin/%s", name);
    int killed = 0;
    for (int pid = 2; pid < 65536; pid++) {
        char path[64], cmdline[512];
        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        FILE *file = fopen(path, "r");
        if (!file) continue;
        size_t got = fread(cmdline, 1, sizeof(cmdline) - 1, file);
        fclose(file);
        cmdline[got] = 0;
        if (got && !strcmp(cmdline, pattern)) {
            if (kill(pid, SIGTERM) == 0) ++killed;
        }
    }
    snprintf(reply, reply_size, "OK %d", killed);
    return 0;
}

static void handle_line(char *line, char *reply, size_t reply_size) {
    while (*line == ' ') ++line;
    char name[128], endpoint[128];
    if (!strncmp(line, "APP_START", 9)) {
        field(line, "name=", name, sizeof(name));
        field(line, "endpoint=", endpoint, sizeof(endpoint));
        start_app(name, endpoint, reply, reply_size);
    } else if (!strncmp(line, "APP_STOP", 8)) {
        field(line, "name=", name, sizeof(name));
        stop_app(name, reply, reply_size);
    } else if (!strncmp(line, "PING", 4)) {
        snprintf(reply, reply_size, "OK");
    } else {
        snprintf(reply, reply_size, "ERR unknown-command");
    }
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "--once")) {
        char reply[256] = {0};
        handle_line(argv[2], reply, sizeof(reply));
        printf("%s\n", reply);
        return 0;
    }

    int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server < 0) { perror("socket"); return 1; }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    size_t length = strlen(SERVICE_NAME);
    memcpy(address.sun_path + 1, SERVICE_NAME, length);
    if (bind(server, (struct sockaddr *)&address, offsetof(struct sockaddr_un, sun_path) + 1 + length) ||
        listen(server, 4)) {
        perror("bind/listen");
        return 1;
    }
    /* 让应用侧连得上：抽象套接字没有文件权限问题，但目录权限会影响非 root 客户端。 */
    chmod("/data/local/tmp/boot-tools-island", 0755);
    fprintf(stderr, "l410-compat-serviced: listening on @%s\n", SERVICE_NAME);
    fflush(stderr);

    for (;;) {
        int client = accept4(server, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) {
            if (errno == EINTR) continue;
            continue;
        }
        char line[512];
        size_t used = 0;
        char reply[256] = {0};
        for (;;) {
            ssize_t got = recv(client, line + used, sizeof(line) - used - 1, 0);
            if (got <= 0) break;
            used += (size_t)got;
            line[used] = 0;
            char *newline = strchr(line, '\n');
            if (!newline) {
                if (used >= sizeof(line) - 1) break;
                continue;
            }
            *newline = 0;
            handle_line(line, reply, sizeof(reply));
            size_t done = 0, total = strlen(reply);
            while (done < total) {
                ssize_t written = send(client, reply + done, total - done, MSG_NOSIGNAL);
                if (written <= 0) break;
                done += (size_t)written;
            }
            send(client, "\n", 1, MSG_NOSIGNAL);
            break;
        }
        close(client);
    }
    return 0;
}
