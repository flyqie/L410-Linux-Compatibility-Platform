#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>

#include "l410ohos.h"

/* Stable C boundary implemented by l410_anco_surface_typed.cpp. */
struct L410NativeBufferInfo {
    int32_t fd, width, stride, height, size, format;
    uint64_t usage;
};
extern void *l410_anco_surface_create_typed(const char *, int32_t, int32_t);
extern int32_t l410_anco_surface_publish_typed(void *, int32_t);
extern int32_t l410_anco_surface_destroy_typed(void *);
extern int32_t l410_anco_surface_id_typed(void *, uint64_t *);
extern int32_t l410_anco_surface_configure_typed(void *, int32_t, int32_t);
extern int32_t l410_anco_surface_request_buffer_typed(
    void *, void **, int32_t *, struct L410NativeBufferInfo *);
extern int32_t l410_anco_surface_flush_buffer_typed(void *, void *, int32_t);
extern int32_t l410_anco_surface_abort_buffer_typed(void *, void *);

typedef struct BrokerWindow {
    int32_t window_id;
    int32_t width;
    int32_t height;
    uint64_t surface_id;
    void *surface;
    int server_fd;
    int client_fd;
    _Atomic int stop;
    _Atomic int stage;
    _Atomic int last_error;
    _Atomic unsigned frames;
    pthread_t thread;
    char endpoint[96];
    struct BrokerWindow *next;
} BrokerWindow;

static BrokerWindow *g_windows;
static pthread_mutex_t g_windows_lock = PTHREAD_MUTEX_INITIALIZER;

int l410_broker_window_destroy(int32_t window_id);

static int write_all(int fd, const void *data, size_t size)
{
    const char *at = data;
    while (size) {
        ssize_t written = send(fd, at, size, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (written == 0) return -1;
        at += written;
        size -= (size_t)written;
    }
    return 0;
}

static int read_all(int fd, void *data, size_t size)
{
    char *at = data;
    while (size) {
        ssize_t received = recv(fd, at, size, 0);
        if (received < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (received == 0) return -1;
        at += received;
        size -= (size_t)received;
    }
    return 0;
}

int l410_broker_window_probe_queue(const char *endpoint, int32_t width, int32_t height)
{
    if (!endpoint || width < 16 || height < 16) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -2;
    struct timeval timeout = {1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    size_t length = strlen(endpoint);
    if (length >= sizeof(address.sun_path) - 1) { close(fd); return -3; }
    memcpy(address.sun_path + 1, endpoint, length);
    if (connect(fd, (struct sockaddr *)&address,
        offsetof(struct sockaddr_un, sun_path) + 1 + length)) { close(fd); return -4; }
    uint32_t hello[3] = {L410_OHOS_WIRE_QUEUE, (uint32_t)width, (uint32_t)height};
    struct l410_ohos_want want = {L410_OHOS_WANT, 1};
    if (write_all(fd, hello, sizeof(hello)) || write_all(fd, &want, sizeof(want))) {
        close(fd); return -5;
    }
    struct l410_ohos_slot slot;
    struct iovec iov = {&slot, sizeof(slot)};
    char control[CMSG_SPACE(sizeof(int))];
    memset(&slot, 0, sizeof(slot));
    memset(control, 0, sizeof(control));
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    ssize_t received = recvmsg(fd, &message, 0);
    int buffer_fd = -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
        memcpy(&buffer_fd, CMSG_DATA(cmsg), sizeof(buffer_fd));
    if (received != (ssize_t)sizeof(slot) || slot.magic != L410_OHOS_SLOT || buffer_fd < 0) {
        if (buffer_fd >= 0) close(buffer_fd);
        close(fd); return -6;
    }
    close(buffer_fd);
    struct l410_ohos_frame frame;
    int result = read_all(fd, &frame, sizeof(frame)) == 0 &&
        frame.magic == L410_OHOS_FRAME ? 0 : -7;
    close(fd);
    return result;
}

static int wait_fence(BrokerWindow *window, int fd, int milliseconds)
{
    while (milliseconds > 0 && !__atomic_load_n(&window->stop, __ATOMIC_ACQUIRE)) {
        struct pollfd item = {fd, POLLIN, 0};
        int slice = milliseconds > 100 ? 100 : milliseconds;
        int result = poll(&item, 1, slice);
        if (result > 0) return (item.revents & (POLLIN | POLLHUP)) != 0;
        if (result < 0 && errno != EINTR) return 0;
        milliseconds -= slice;
    }
    return 0;
}

static int send_slot(int client, uint32_t index, const struct L410NativeBufferInfo *native)
{
    struct l410_ohos_slot slot;
    memset(&slot, 0, sizeof(slot));
    slot.magic = L410_OHOS_SLOT;
    slot.index = index;
    slot.width = native->width;
    slot.height = native->height;
    slot.stride = native->stride;
    slot.size = native->size;
    slot.format = native->format;
    struct iovec iov = {&slot, sizeof(slot)};
    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &native->fd, sizeof(int));
    return sendmsg(client, &message, MSG_NOSIGNAL) == (ssize_t)sizeof(slot) ? 0 : -1;
}

static void frame_loop(BrokerWindow *window, int client)
{
    int slot_fd[L410_OHOS_SLOTS_MAX];
    void *slot_buffer[L410_OHOS_SLOTS_MAX];
    int slot_busy[L410_OHOS_SLOTS_MAX];
    int slot_count = 0;
    uint32_t serial = 0;
    int wanted = 0;
    for (int i = 0; i < L410_OHOS_SLOTS_MAX; i++) {
        slot_fd[i] = -1;
        slot_buffer[i] = NULL;
        slot_busy[i] = 0;
    }
    __atomic_store_n(&window->stage, 3, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&window->stop, __ATOMIC_ACQUIRE)) {
        struct pollfd incoming = {client, POLLIN, 0};
        int ready = poll(&incoming, 1, wanted ? 0 : 200);
        if (ready < 0 && errno != EINTR) break;
        if (ready > 0 && (incoming.revents & (POLLHUP | POLLERR | POLLNVAL))) break;
        if (ready > 0 && (incoming.revents & POLLIN)) {
            uint32_t magic = 0;
            if (recv(client, &magic, sizeof(magic), MSG_PEEK) != (ssize_t)sizeof(magic)) break;
            if (magic == L410_OHOS_WANT) {
                struct l410_ohos_want request;
                if (read_all(client, &request, sizeof(request))) break;
                if (wanted < L410_OHOS_SLOTS_MAX) ++wanted;
            } else if (magic == L410_OHOS_PRESENT) {
                struct l410_ohos_present present;
                if (read_all(client, &present, sizeof(present))) break;
                if (present.index < L410_OHOS_SLOTS_MAX && slot_busy[present.index]) {
                    int result = l410_anco_surface_flush_buffer_typed(
                        window->surface, slot_buffer[present.index], -1);
                    slot_busy[present.index] = 0;
                    slot_buffer[present.index] = NULL;
                    if (result != 0) {
                        __atomic_store_n(&window->last_error, result, __ATOMIC_RELEASE);
                        break;
                    }
                    __atomic_add_fetch(&window->frames, 1, __ATOMIC_RELAXED);
                }
            } else {
                break;
            }
        }

        while (wanted > 0 && !__atomic_load_n(&window->stop, __ATOMIC_ACQUIRE)) {
            void *buffer = NULL;
            int32_t fence = -1;
            struct L410NativeBufferInfo native;
            memset(&native, 0, sizeof(native));
            int result = l410_anco_surface_request_buffer_typed(
                window->surface, &buffer, &fence, &native);
            if (result != 0 || buffer == NULL) {
                __atomic_store_n(&window->last_error, result ? result : -20, __ATOMIC_RELEASE);
                /* A WANT is edge-triggered work, not a permanent demand bit.
                 * If the GraphicBroker has not connected a RenderService
                 * consumer yet, RequestBuffer fails immediately.  Keeping
                 * wanted non-zero turned that state into a tight retry loop
                 * and flooded hilog.  Drop the request; the client can ask
                 * again after the lifecycle handshake completes. */
                wanted = 0;
                usleep(20000);
                break;
            }
            if (fence >= 0) {
                int signalled = wait_fence(window, fence, 200);
                close(fence);
                if (!signalled) {
                    l410_anco_surface_abort_buffer_typed(window->surface, buffer);
                    break;
                }
            }
            int index = -1;
            for (int i = 0; i < slot_count; i++) {
                if (slot_fd[i] == native.fd) index = i;
            }
            if (index < 0) {
                if (slot_count >= L410_OHOS_SLOTS_MAX) {
                    l410_anco_surface_abort_buffer_typed(window->surface, buffer);
                    __atomic_store_n(&window->last_error, -21, __ATOMIC_RELEASE);
                    wanted = 0;
                    break;
                }
                index = slot_count++;
                slot_fd[index] = native.fd;
                if (send_slot(client, (uint32_t)index, &native)) {
                    l410_anco_surface_abort_buffer_typed(window->surface, buffer);
                    return;
                }
            }
            if (slot_busy[index]) {
                l410_anco_surface_abort_buffer_typed(window->surface, buffer);
                wanted = 0;
                break;
            }
            struct l410_ohos_frame frame;
            memset(&frame, 0, sizeof(frame));
            frame.magic = L410_OHOS_FRAME;
            frame.index = (uint32_t)index;
            frame.serial = ++serial;
            if (write_all(client, &frame, sizeof(frame))) {
                l410_anco_surface_abort_buffer_typed(window->surface, buffer);
                return;
            }
            slot_buffer[index] = buffer;
            slot_busy[index] = 1;
            --wanted;
        }
    }
    for (int i = 0; i < L410_OHOS_SLOTS_MAX; i++) {
        if (slot_busy[i] && slot_buffer[i])
            l410_anco_surface_abort_buffer_typed(window->surface, slot_buffer[i]);
    }
}

static void *window_thread(void *argument)
{
    BrokerWindow *window = argument;
    int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    window->server_fd = server;
    if (server < 0) goto failed;
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    size_t length = strlen(window->endpoint);
    memcpy(address.sun_path + 1, window->endpoint, length);
    if (bind(server, (struct sockaddr *)&address,
             offsetof(struct sockaddr_un, sun_path) + 1 + length) || listen(server, 1)) goto failed;
    __atomic_store_n(&window->stage, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&window->stop, __ATOMIC_ACQUIRE)) {
        struct pollfd waiting = {server, POLLIN, 0};
        int ready = poll(&waiting, 1, 200);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;
        int client = accept4(server, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) continue;
        window->client_fd = client;
        struct ucred cred;
        socklen_t cred_size = sizeof(cred);
        uint32_t hello[3];
        if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &cred_size) ||
            (cred.uid != 0 && cred.uid != getuid()) ||
            read_all(client, hello, sizeof(hello)) || hello[0] != L410_OHOS_WIRE_QUEUE ||
            hello[1] < 16 || hello[2] < 16 || hello[1] > 3840 || hello[2] > 2160) {
            close(client);
            window->client_fd = -1;
            continue;
        }
        __atomic_store_n(&window->stage, 2, __ATOMIC_RELEASE);
        frame_loop(window, client);
        close(client);
        window->client_fd = -1;
        if (!__atomic_load_n(&window->stop, __ATOMIC_ACQUIRE))
            __atomic_store_n(&window->stage, 1, __ATOMIC_RELEASE);
    }
    if (server >= 0) close(server);
    window->server_fd = -1;
    __atomic_store_n(&window->stage, 0, __ATOMIC_RELEASE);
    return NULL;
failed:
    __atomic_store_n(&window->last_error, -10 - errno, __ATOMIC_RELEASE);
    if (server >= 0) close(server);
    window->server_fd = -1;
    __atomic_store_n(&window->stage, -1, __ATOMIC_RELEASE);
    return NULL;
}

int l410_broker_window_create(int32_t window_id, int32_t width, int32_t height,
                              char *endpoint, size_t endpoint_size, uint64_t *surface_id)
{
    if (window_id <= 0 || width < 16 || height < 16 || !endpoint || endpoint_size < 32) return -1;
    pthread_mutex_lock(&g_windows_lock);
    for (BrokerWindow *at = g_windows; at; at = at->next) {
        if (at->window_id == window_id) {
            snprintf(endpoint, endpoint_size, "%s", at->endpoint);
            if (surface_id) *surface_id = at->surface_id;
            pthread_mutex_unlock(&g_windows_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_windows_lock);

    BrokerWindow *window = calloc(1, sizeof(*window));
    if (!window) return -2;
    window->window_id = window_id;
    window->width = width;
    window->height = height;
    window->server_fd = -1;
    window->client_fd = -1;
    snprintf(window->endpoint, sizeof(window->endpoint),
             "l410-broker-surface-%ld-%d", (long)getpid(), window_id);
    char name[96];
    snprintf(name, sizeof(name), "l410-linux-window-%d", window_id);
    window->surface = l410_anco_surface_create_typed(name, width, height);
    if (!window->surface) { free(window); return -3; }
    int result = l410_anco_surface_configure_typed(window->surface, width, height);
    if (result == 0) result = l410_anco_surface_id_typed(window->surface, &window->surface_id);
    if (result == 0) result = l410_anco_surface_publish_typed(window->surface, window_id);
    if (result != 0) {
        l410_anco_surface_destroy_typed(window->surface);
        free(window);
        return result;
    }
    pthread_mutex_lock(&g_windows_lock);
    window->next = g_windows;
    g_windows = window;
    pthread_mutex_unlock(&g_windows_lock);
    if (pthread_create(&window->thread, NULL, window_thread, window)) {
        l410_broker_window_destroy(window_id);
        return -4;
    }
    for (int wait = 0; wait < 50 && __atomic_load_n(&window->stage, __ATOMIC_ACQUIRE) == 0; wait++)
        usleep(10000);
    if (__atomic_load_n(&window->stage, __ATOMIC_ACQUIRE) < 0) {
        l410_broker_window_destroy(window_id);
        return -5;
    }
    snprintf(endpoint, endpoint_size, "%s", window->endpoint);
    if (surface_id) *surface_id = window->surface_id;
    return 0;
}

int l410_broker_window_destroy(int32_t window_id)
{
    pthread_mutex_lock(&g_windows_lock);
    BrokerWindow **link = &g_windows;
    while (*link && (*link)->window_id != window_id) link = &(*link)->next;
    BrokerWindow *window = *link;
    if (window) *link = window->next;
    pthread_mutex_unlock(&g_windows_lock);
    if (!window) return -1;
    __atomic_store_n(&window->stop, 1, __ATOMIC_RELEASE);
    if (window->client_fd >= 0) shutdown(window->client_fd, SHUT_RDWR);
    if (window->server_fd >= 0) shutdown(window->server_fd, SHUT_RDWR);
    pthread_join(window->thread, NULL);
    int result = l410_anco_surface_destroy_typed(window->surface);
    free(window);
    return result;
}

int l410_broker_window_status(int32_t window_id, char *text, size_t size)
{
    if (!text || size < 64) return -1;
    pthread_mutex_lock(&g_windows_lock);
    BrokerWindow *window = g_windows;
    while (window && window->window_id != window_id) window = window->next;
    if (!window) {
        pthread_mutex_unlock(&g_windows_lock);
        snprintf(text, size, "{\"present\":false}");
        return -2;
    }
    snprintf(text, size,
             "{\"present\":true,\"windowId\":%d,\"surfaceId\":%llu,\"stage\":%d,\"frames\":%u,\"error\":%d,\"endpoint\":\"%s\"}",
             window->window_id, (unsigned long long)window->surface_id,
             __atomic_load_n(&window->stage, __ATOMIC_ACQUIRE),
             __atomic_load_n(&window->frames, __ATOMIC_RELAXED),
             __atomic_load_n(&window->last_error, __ATOMIC_ACQUIRE), window->endpoint);
    pthread_mutex_unlock(&g_windows_lock);
    return 0;
}
