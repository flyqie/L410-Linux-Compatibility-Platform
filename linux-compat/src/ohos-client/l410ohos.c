#define _GNU_SOURCE
#include "l410ohos.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

struct L410Ohos {
    int fd;
    int event_fd;
    int buffer_fd;
    void *pixels;
    size_t mapped;
    struct l410_ohos_buffer buffer;
    uint32_t serial;
    uint32_t epoch;
    int acquired;
    int connected;
    char incoming[4096];
    size_t used;
    char line[1024];
    char error[192];
    /* 帧回调队列 */
    struct {
        int fd;
        void *pixels;
        size_t mapped;
        int width, height, stride, format;
    } slots[L410_OHOS_SLOTS_MAX];
    int queue_width;
    int queue_height;
};

static void set_error(L410Ohos *surface, const char *text) {
    if (!surface) return;
    snprintf(surface->error, sizeof(surface->error), "%s", text);
}

static int connect_abstract(const char *name, int *out_fd) {
    if (!name || !*name || strlen(name) > 100) return -EINVAL;
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    size_t length = strlen(name);
    memcpy(address.sun_path + 1, name, length);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;
    struct timeval timeout = {3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, (struct sockaddr *)&address, offsetof(struct sockaddr_un, sun_path) + 1 + length)) {
        int error = -errno;
        close(fd);
        return error;
    }
    *out_fd = fd;
    return 0;
}

static int send_all(int fd, const void *data, size_t count) {
    const char *at = (const char *)data;
    while (count) {
        ssize_t written = send(fd, at, count, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (written == 0) return -EPIPE;
        at += written;
        count -= (size_t)written;
    }
    return 0;
}

static int read_all(int fd, void *data, size_t count) {
    char *at = (char *)data;
    while (count) {
        ssize_t got = recv(fd, at, count, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (got == 0) return -EPIPE;
        at += got;
        count -= (size_t)got;
    }
    return 0;
}

static L410Ohos *open_with_hello(const char *endpoint, int width, int height, uint32_t hello_magic);

L410Ohos *l410_ohos_open(const char *endpoint, int width, int height) {
    return open_with_hello(endpoint, width, height, L410_OHOS_WIRE_HELLO);
}

L410Ohos *l410_ohos_open_queue(const char *endpoint, int width, int height) {
    return open_with_hello(endpoint, width, height, L410_OHOS_WIRE_QUEUE);
}

static L410Ohos *open_with_hello(const char *endpoint, int width, int height, uint32_t hello_magic) {
    L410Ohos *surface = calloc(1, sizeof(*surface));
    if (!surface) return NULL;
    surface->fd = surface->event_fd = surface->buffer_fd = -1;
    for (int i = 0; i < L410_OHOS_SLOTS_MAX; i++) surface->slots[i].fd = -1;
    if (width < 16 || width > 3840) width = 640;
    if (height < 16 || height > 2160) height = 400;
    int error = connect_abstract(endpoint, &surface->fd);
    if (error) {
        set_error(surface, "window host not reachable");
        l410_ohos_close(surface);
        return NULL;
    }
    /* The window host answers this hello with the request driven buffer loop. */
    uint32_t hello[3] = {hello_magic, (uint32_t)width, (uint32_t)height};
    if (send_all(surface->fd, hello, sizeof(hello))) {
        set_error(surface, "window host handshake failed");
        l410_ohos_close(surface);
        return NULL;
    }
    surface->serial = 0;
    surface->epoch = 1;
    surface->connected = 1;
    set_error(surface, "ok");
    return surface;
}

static void release_buffer(L410Ohos *surface) {
    if (surface->pixels && surface->mapped) munmap(surface->pixels, surface->mapped);
    surface->pixels = NULL;
    surface->mapped = 0;
    if (surface->buffer_fd >= 0) close(surface->buffer_fd);
    surface->buffer_fd = -1;
    surface->acquired = 0;
}

static void release_queue_slots(L410Ohos *surface) {
    if (!surface) return;
    for (int i = 0; i < L410_OHOS_SLOTS_MAX; i++) {
        if (surface->slots[i].pixels && surface->slots[i].mapped)
            munmap(surface->slots[i].pixels, surface->slots[i].mapped);
        if (surface->slots[i].fd >= 0) close(surface->slots[i].fd);
        memset(&surface->slots[i], 0, sizeof(surface->slots[i]));
        surface->slots[i].fd = -1;
    }
}

void l410_ohos_close(L410Ohos *surface) {
    if (!surface) return;
    release_buffer(surface);
    release_queue_slots(surface);
    if (surface->event_fd >= 0) close(surface->event_fd);
    if (surface->fd >= 0) close(surface->fd);
    free(surface);
}

/* ------------------------------------------------------------ 帧回调队列 */

int l410_ohos_surface_fd(const L410Ohos *surface) { return surface ? surface->fd : -1; }
int l410_ohos_connected(const L410Ohos *surface) { return surface ? surface->connected : 0; }
uint32_t l410_ohos_peek_magic(const L410Ohos *surface) {
    uint32_t magic = 0;
    if (!surface || !surface->connected) return 0;
    ssize_t got = recv(surface->fd, &magic, sizeof(magic), MSG_PEEK | MSG_DONTWAIT);
    if (got < (ssize_t)sizeof(magic)) return 0;
    return magic;
}

int l410_ohos_want(L410Ohos *surface) {
    if (!surface || !surface->connected) return -EINVAL;
    struct l410_ohos_want request;
    memset(&request, 0, sizeof(request));
    request.magic = L410_OHOS_WANT;
    request.serial = ++surface->serial;
    int error = send_all(surface->fd, &request, sizeof(request));
    if (error) {
        set_error(surface, "window host vanished on frame request");
        surface->connected = 0;
    }
    return error;
}

/* 把宿主推来的消息处理掉：SLOT 收 fd 并映射，FRAME 返回给调用方去画。
 * 记录都是小结构、一次 sendmsg 发出：先 MSG_PEEK 看魔数，再按类型整条读。 */
int l410_ohos_poll_frame(L410Ohos *surface, int *slot, uint32_t *serial) {
    if (!surface || !surface->connected) return -1;
    for (int guard = 0; guard < 8; guard++) {
        uint32_t magic = 0;
        ssize_t peeked = recv(surface->fd, &magic, sizeof(magic), MSG_PEEK | MSG_DONTWAIT);
        if (peeked < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
            surface->connected = 0;
            return -1;
        }
        if (peeked == 0) {
            surface->connected = 0;
            return -1;
        }
        if (peeked < (ssize_t)sizeof(magic)) return 0;

        if (magic == L410_OHOS_FRAME) {
            struct l410_ohos_frame frame;
            if (read_all(surface->fd, &frame, sizeof(frame))) return -1;
            if (frame.index >= L410_OHOS_SLOTS_MAX || !surface->slots[frame.index].pixels) continue;
            surface->queue_width = surface->slots[frame.index].width;
            surface->queue_height = surface->slots[frame.index].height;
            if (slot) *slot = (int)frame.index;
            if (serial) *serial = frame.serial;
            return 1;
        }

        if (magic == L410_OHOS_SLOT) {
            struct l410_ohos_slot info;
            memset(&info, 0, sizeof(info));
            int fd = -1;
            struct iovec iov = {&info, sizeof(info)};
            char control[CMSG_SPACE(sizeof(int))];
            memset(control, 0, sizeof(control));
            struct msghdr message;
            memset(&message, 0, sizeof(message));
            message.msg_iov = &iov;
            message.msg_iovlen = 1;
            message.msg_control = control;
            message.msg_controllen = sizeof(control);
            ssize_t got = recvmsg(surface->fd, &message, MSG_WAITALL | MSG_CMSG_CLOEXEC);
            if (got != (ssize_t)sizeof(info)) return -1;
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
                    memcpy(&fd, CMSG_DATA(c), sizeof(int));
            }
            if (fd < 0 || info.index >= L410_OHOS_SLOTS_MAX || info.size <= 0) {
                if (fd >= 0) close(fd);
                continue;
            }
            if (surface->slots[info.index].pixels)
                munmap(surface->slots[info.index].pixels, surface->slots[info.index].mapped);
            if (surface->slots[info.index].fd >= 0) close(surface->slots[info.index].fd);
            void *pixels = mmap(NULL, (size_t)info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (pixels == MAP_FAILED) {
                close(fd);
                set_error(surface, "queue slot is not mappable");
                return -1;
            }
            surface->slots[info.index].fd = fd;
            surface->slots[info.index].pixels = pixels;
            surface->slots[info.index].mapped = (size_t)info.size;
            surface->slots[info.index].width = info.width;
            surface->slots[info.index].height = info.height;
            surface->slots[info.index].stride = info.stride;
            surface->slots[info.index].format = info.format;
            continue;
        }

        if (magic == L410_OHOS_RESET) {
            struct l410_ohos_reset reset;
            if (read_all(surface->fd, &reset, sizeof(reset))) return -1;
            release_queue_slots(surface);
            surface->queue_width = reset.width;
            surface->queue_height = reset.height;
            continue;
        }

        /* 旧协议的记录（RESULT/BUFFER）在本模式下只可能是回声，按大小吞掉。 */
        if (magic == L410_OHOS_RESULT) {
            struct l410_ohos_result drain;
            if (read_all(surface->fd, &drain, sizeof(drain))) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

void *l410_ohos_slot_pixels(L410Ohos *surface, int slot) {
    if (!surface || slot < 0 || slot >= L410_OHOS_SLOTS_MAX) return NULL;
    return surface->slots[slot].pixels;
}

int l410_ohos_slot_stride(const L410Ohos *surface, int slot) {
    if (!surface || slot < 0 || slot >= L410_OHOS_SLOTS_MAX) return 0;
    return surface->slots[slot].stride;
}

int l410_ohos_present(L410Ohos *surface, int slot, uint32_t serial) {
    if (!surface || !surface->connected) return -EINVAL;
    struct l410_ohos_present record;
    memset(&record, 0, sizeof(record));
    record.magic = L410_OHOS_PRESENT;
    record.index = (uint32_t)slot;
    record.serial = serial;
    int error = send_all(surface->fd, &record, sizeof(record));
    if (error) {
        set_error(surface, "window host vanished on present");
        surface->connected = 0;
    }
    return error;
}

int l410_ohos_cancel(L410Ohos *surface, int slot, uint32_t serial) {
    if (!surface || !surface->connected) return -EINVAL;
    struct l410_ohos_present record;
    memset(&record, 0, sizeof(record));
    record.magic = L410_OHOS_CANCEL;
    record.index = (uint32_t)slot;
    record.serial = serial;
    int error = send_all(surface->fd, &record, sizeof(record));
    if (error) {
        set_error(surface, "window host vanished on cancel");
        surface->connected = 0;
    }
    return error;
}

int l410_ohos_queue_width(const L410Ohos *surface) { return surface ? surface->queue_width : 0; }
int l410_ohos_queue_height(const L410Ohos *surface) { return surface ? surface->queue_height : 0; }

int l410_ohos_acquire(L410Ohos *surface, int width, int height) {
    if (!surface || !surface->connected) return -EINVAL;
    if (width < 16 || width > 3840 || height < 16 || height > 2160) return -EINVAL;
    if (surface->acquired) return 0;
    for (int attempt = 0; attempt < 40; attempt++) {
        if (surface->serial == UINT32_MAX) return -EOVERFLOW;
        struct l410_ohos_acquire request;
        memset(&request, 0, sizeof(request));
        request.magic = L410_OHOS_ACQUIRE;
        request.serial = ++surface->serial;
        request.epoch = surface->epoch;
        request.width = (uint32_t)width;
        request.height = (uint32_t)height;
        if (send_all(surface->fd, &request, sizeof(request))) {
            set_error(surface, "window host stopped answering");
            surface->connected = 0;
            return -EPIPE;
        }
        uint32_t header[4] = {0, 0, 0, 0};
        struct iovec iov = {header, sizeof(header)};
        char control[CMSG_SPACE(sizeof(int))];
        memset(control, 0, sizeof(control));
        struct msghdr message;
        memset(&message, 0, sizeof(message));
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        ssize_t got = recvmsg(surface->fd, &message, MSG_WAITALL | MSG_CMSG_CLOEXEC);
        int fd = -1;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            int *values = (int *)CMSG_DATA(c);
            for (size_t i = 0; i < count; i++) {
                if (fd < 0) fd = values[i];
                else close(values[i]);
            }
        }
        if (got != (ssize_t)sizeof(header) || (message.msg_flags & MSG_CTRUNC)) {
            if (fd >= 0) close(fd);
            set_error(surface, "malformed buffer answer");
            surface->connected = 0;
            return -EPROTO;
        }
        if (header[0] == L410_OHOS_RESULT) {
            int32_t status = (int32_t)header[3];
            if (fd >= 0) close(fd);
            if (status == -EAGAIN && attempt + 1 < 40) {
                usleep(2000); /* geometry turnover: the host asks again */
                continue;
            }
            set_error(surface, "window host refused the frame request");
            return status ? status : -EPROTO;
        }
        if (header[0] != L410_OHOS_BUFFER || fd < 0) {
            if (fd >= 0) close(fd);
            set_error(surface, "unexpected frame answer");
            surface->connected = 0;
            return -EPROTO;
        }
        struct l410_ohos_buffer buffer;
        memset(&buffer, 0, sizeof(buffer));
        memcpy(&buffer, header, sizeof(header));
        if (read_all(surface->fd, (char *)&buffer + sizeof(header), sizeof(buffer) - sizeof(header))) {
            close(fd);
            set_error(surface, "truncated buffer record");
            surface->connected = 0;
            return -EPROTO;
        }
        if (buffer.serial != request.serial || buffer.epoch != request.epoch ||
            buffer.width != width || buffer.height != height ||
            buffer.stride < width * 4 || buffer.size < (int64_t)buffer.stride * height) {
            close(fd);
            set_error(surface, "buffer geometry does not match the request");
            surface->connected = 0;
            return -EPROTO;
        }
        void *pixels = mmap(NULL, (size_t)buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (pixels == MAP_FAILED) {
            close(fd);
            set_error(surface, "graphic buffer is not mappable");
            return -EIO;
        }
        surface->buffer_fd = fd;
        surface->pixels = pixels;
        surface->mapped = (size_t)buffer.size;
        surface->buffer = buffer;
        surface->acquired = 1;
        set_error(surface, "ok");
        return 0;
    }
    set_error(surface, "no buffer after retries");
    return -EAGAIN;
}

void *l410_ohos_pixels(const L410Ohos *surface) { return surface ? surface->pixels : NULL; }
int l410_ohos_stride(const L410Ohos *surface) { return surface ? surface->buffer.stride : 0; }
int l410_ohos_format(const L410Ohos *surface) { return surface ? surface->buffer.format : -1; }
int l410_ohos_width(const L410Ohos *surface) { return surface ? surface->buffer.width : 0; }
int l410_ohos_height(const L410Ohos *surface) { return surface ? surface->buffer.height : 0; }

int l410_ohos_finish(L410Ohos *surface, int submit) {
    if (!surface || !surface->acquired) return -EINVAL;
    uint32_t serial = surface->buffer.serial;
    uint32_t epoch = surface->buffer.epoch;
    release_buffer(surface);
    struct l410_ohos_finish record;
    memset(&record, 0, sizeof(record));
    record.magic = L410_OHOS_FINISH;
    record.serial = serial;
    record.submit = submit ? 1u : 0u;
    if (send_all(surface->fd, &record, sizeof(record))) {
        set_error(surface, "window host vanished on present");
        surface->connected = 0;
        return -EPIPE;
    }
    struct l410_ohos_result result;
    memset(&result, 0, sizeof(result));
    if (read_all(surface->fd, &result, sizeof(result)) ||
        result.magic != L410_OHOS_RESULT || result.serial != serial || result.epoch != epoch) {
        set_error(surface, "present answer mismatch");
        surface->connected = 0;
        return -EPROTO;
    }
    if (result.status) {
        set_error(surface, result.status == -EAGAIN ? "buffer became obsolete" : "present failed");
        return result.status;
    }
    set_error(surface, "ok");
    return 0;
}

int l410_ohos_open_events(L410Ohos *surface, const char *event_endpoint) {
    if (!surface || !event_endpoint || !*event_endpoint) return -EINVAL;
    int fd = -1;
    int error = connect_abstract(event_endpoint, &fd);
    if (error) return error;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    struct timeval timeout = {0, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    surface->event_fd = fd;
    return 0;
}

/* Event channel only: the toolbar of a toolkit needs focus, clipboard and IME
 * without owning a graphic buffer itself. */
L410Ohos *l410_ohos_open_event_channel(const char *event_endpoint) {
    L410Ohos *surface = calloc(1, sizeof(*surface));
    if (!surface) return NULL;
    surface->fd = surface->event_fd = surface->buffer_fd = -1;
    for (int i = 0; i < L410_OHOS_SLOTS_MAX; i++) surface->slots[i].fd = -1;
    if (l410_ohos_open_events(surface, event_endpoint)) {
        set_error(surface, "event channel not reachable");
        l410_ohos_close(surface);
        return NULL;
    }
    set_error(surface, "ok");
    return surface;
}

int l410_ohos_event_fd(const L410Ohos *surface) { return surface ? surface->event_fd : -1; }

int l410_ohos_event_read(L410Ohos *surface, char *line, size_t size) {
    if (!surface || surface->event_fd < 0) return -1;
    for (;;) {
        ssize_t got = recv(surface->event_fd, surface->incoming + surface->used,
                           sizeof(surface->incoming) - surface->used, 0);
        if (got > 0) {
            surface->used += (size_t)got;
        } else if (got == 0) {
            return -1;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return -1;
        }
        for (;;) {
            char *newline = memchr(surface->incoming, '\n', surface->used);
            if (!newline) break;
            size_t length = (size_t)(newline - surface->incoming);
            size_t copy = length < size - 1 ? length : size - 1;
            memcpy(line, surface->incoming, copy);
            line[copy] = 0;
            size_t rest = surface->used - length - 1;
            memmove(surface->incoming, newline + 1, rest);
            surface->used = rest;
            return (int)copy;
        }
        if (got <= 0) {
            if (surface->used >= sizeof(surface->incoming)) surface->used = 0;
            return 0;
        }
    }
}

int l410_ohos_event_send(L410Ohos *surface, const char *line) {
    if (!surface || surface->event_fd < 0 || !line) return -1;
    size_t length = strlen(line);
    if (length + 2 > sizeof(surface->line)) return -1;
    memcpy(surface->line, line, length);
    surface->line[length] = '\n';
    return send_all(surface->event_fd, surface->line, length + 1);
}

const char *l410_ohos_error(const L410Ohos *surface) {
    return surface ? surface->error : "no surface";
}

/* ------------------------------------------------------------- engine channel */

#define L410_ENGINE_QUEUE 48
#define L410_ENGINE_LINE 1024

struct L410Engine {
    int fd;
    int connected;
    int error;
    char incoming[4096];
    size_t used;
    char queued[L410_ENGINE_QUEUE][L410_ENGINE_LINE];
    int queue_head;
    int queue_count;
};

L410Engine *l410_engine_open(const char *endpoint) {
    if (!endpoint || !*endpoint) return NULL;
    L410Engine *engine = calloc(1, sizeof(*engine));
    if (!engine) return NULL;
    engine->fd = -1;
    int error = connect_abstract(endpoint, &engine->fd);
    if (error) {
        engine->error = error;
        l410_engine_close(engine);
        return NULL;
    }
    /* Requests are answered in hundreds of microseconds; the read side is
     * non-blocking so the toolkit's own event loop stays in charge. */
    int flags = fcntl(engine->fd, F_GETFL, 0);
    if (flags >= 0) fcntl(engine->fd, F_SETFL, flags | O_NONBLOCK);
    struct timeval timeout = {0, 200000};
    setsockopt(engine->fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    engine->connected = 1;
    return engine;
}

void l410_engine_close(L410Engine *engine) {
    if (!engine) return;
    if (engine->fd >= 0) close(engine->fd);
    free(engine);
}

int l410_engine_fd(const L410Engine *engine) { return engine ? engine->fd : -1; }
int l410_engine_connected(const L410Engine *engine) { return engine && engine->connected; }

int l410_engine_send(L410Engine *engine, const char *line) {
    if (!engine || engine->fd < 0 || !line) return -1;
    size_t length = strlen(line);
    if (!length || length > L410_ENGINE_LINE - 2) return -1;
    static __thread char outgoing[L410_ENGINE_LINE];
    memcpy(outgoing, line, length);
    outgoing[length] = '\n';
    int error = send_all(engine->fd, outgoing, length + 1);
    if (error) engine->connected = 0;
    return error;
}

static void queue_push(L410Engine *engine, const char *line) {
    if (engine->queue_count >= L410_ENGINE_QUEUE) {
        /* Drop the oldest line rather than the newest: input events age badly. */
        engine->queue_head = (engine->queue_head + 1) % L410_ENGINE_QUEUE;
        --engine->queue_count;
    }
    const int slot = (engine->queue_head + engine->queue_count) % L410_ENGINE_QUEUE;
    snprintf(engine->queued[slot], L410_ENGINE_LINE, "%s", line);
    ++engine->queue_count;
}

static int queue_pop(L410Engine *engine, char *line, size_t size) {
    if (engine->queue_count <= 0) return 0;
    snprintf(line, size, "%s", engine->queued[engine->queue_head]);
    engine->queue_head = (engine->queue_head + 1) % L410_ENGINE_QUEUE;
    --engine->queue_count;
    return 1;
}

/* Pulls whatever the socket has into complete lines (non-blocking). */
static int pump_lines(L410Engine *engine) {
    if (!engine || engine->fd < 0) return -1;
    for (;;) {
        ssize_t got = recv(engine->fd, engine->incoming + engine->used,
                           sizeof(engine->incoming) - engine->used, 0);
        if (got > 0) {
            engine->used += (size_t)got;
        } else if (got == 0) {
            engine->connected = 0;
            return -1;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            engine->connected = 0;
            return -1;
        }
        for (;;) {
            char *newline = memchr(engine->incoming, '\n', engine->used);
            if (!newline) break;
            const size_t length = (size_t)(newline - engine->incoming);
            *newline = 0;
            if (length) queue_push(engine, engine->incoming);
            const size_t rest = engine->used - length - 1;
            memmove(engine->incoming, newline + 1, rest);
            engine->used = rest;
        }
        if (got <= 0) {
            if (engine->used >= sizeof(engine->incoming)) engine->used = 0;
            return 0;
        }
    }
}

int l410_engine_read(L410Engine *engine, char *line, size_t size) {
    if (!engine || engine->fd < 0 || !line || size < 2) return -1;
    if (!queue_pop(engine, line, size)) {
        if (pump_lines(engine) < 0) return -1;
        if (!queue_pop(engine, line, size)) return 0;
    }
    return (int)strlen(line);
}

int l410_engine_wait(L410Engine *engine, const char *prefix, char *line, size_t size, int timeout_ms) {
    if (!engine || engine->fd < 0 || !prefix || !line || size < 2) return -1;
    const size_t skip = strlen(prefix);
    const int trace = getenv("L410_ENGINE_TRACE") != NULL;
    struct pollfd waiting = {engine->fd, POLLIN, 0};
    for (int elapsed = 0;;) {
        /* 队列里的行会被反复查看（不匹配的先放回队列），所以每一轮都必须
           主动从套接字收新行：否则等待的行永远进不来，只能等到超时。 */
        if (pump_lines(engine) < 0) {
            if (trace) fprintf(stderr, "engine wait: channel closed after %dms\n", elapsed);
            return -1;
        }
        char peek[L410_ENGINE_LINE];
        const int length = l410_engine_read(engine, peek, sizeof(peek));
        if (length < 0) {
            if (trace) fprintf(stderr, "engine wait: channel closed after %dms\n", elapsed);
            return -1;
        }
        if (length > 0) {
            if (trace) fprintf(stderr, "engine wait %dms: %s\n", elapsed, peek);
            if (!strncmp(peek, prefix, skip)) {
                snprintf(line, size, "%s", peek);
                return 1;
            }
            queue_push(engine, peek);   /* keep it for the event loop */
        }
        if (elapsed >= timeout_ms) {
            if (trace) fprintf(stderr, "engine wait: timeout after %dms for %s\n", elapsed, prefix);
            return 0;
        }
        int ready = poll(&waiting, 1, 10);
        elapsed += 10;
        if (ready < 0 && errno != EINTR) return -1;
    }
}
