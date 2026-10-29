#define _GNU_SOURCE
#include "anco_graphic_display.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <limits.h>
#include <endian.h>

_Static_assert(__BYTE_ORDER == __LITTLE_ENDIAN, "paired original Parcel endian");
_Static_assert(sizeof(float) == 4, "original float32");

static int64_t now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int transfer(int fd, void *data, size_t size, int write_data, int64_t deadline)
{
    unsigned char *cursor = data;
    while (size) {
        int64_t remaining = deadline - now_ms();
        if (remaining <= 0) return -ETIMEDOUT;
        struct pollfd p = {.fd = fd, .events = write_data ? POLLOUT : POLLIN};
        int ready = poll(&p, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (ready < 0) { if (errno == EINTR) continue; return -errno; }
        if (!ready) return -ETIMEDOUT;
        ssize_t n = write_data ? send(fd, cursor, size, MSG_NOSIGNAL | MSG_DONTWAIT) :
            recv(fd, cursor, size, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -errno;
        }
        if (!n) return -ECONNRESET;
        cursor += n;
        size -= (size_t)n;
    }
    return 0;
}

int anco_graphic_query_displays(const char *path, int timeout_ms,
    AncoGraphicDisplay **out, size_t *count, int32_t *service)
{
    if (!path || !out || !count || !service || timeout_ms <= 0 ||
        !*path || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return -EINVAL;
    int64_t started = now_ms();
    if (started < 0) return -errno;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -errno;
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, path);
    int status = 0;
    if (connect(fd, (struct sockaddr *)&address,
        offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1)) {
        if (errno != EINPROGRESS) { status = -errno; goto done; }
        struct pollfd pending = {.fd = fd, .events = POLLOUT};
        int ready;
        do { ready = poll(&pending, 1, timeout_ms); } while (ready < 0 && errno == EINTR);
        if (ready <= 0) { status = ready ? -errno : -ETIMEDOUT; goto done; }
        int error = 0; socklen_t size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size)) { status = -errno; goto done; }
        if (error) { status = -error; goto done; }
    }
    /* Original donghu RetryConnect type1, source bytes at0xc40e: ten bytes. */
    unsigned char common[10] = {'c','o','m','m','o','n',0,0,0,0};
    uint32_t request[2] = {4, 2}; /* Parcel length, original UDS command */
    uint32_t length = 0;
    int64_t deadline = started + timeout_ms;
    status = transfer(fd, common, sizeof(common), 1, deadline);
    if (!status) status = transfer(fd, request, sizeof(request), 1, deadline);
    if (!status) status = transfer(fd, &length, sizeof(length), 0, deadline);
    if (status) goto done;
    if (length < 4 || length > 512) { status = -EMSGSIZE; goto done; }
    unsigned char body[512];
    status = transfer(fd, body, length, 0, deadline);
    if (status) goto done;
    int32_t reply_status;
    memcpy(&reply_status, body, 4);
    *service = reply_status;
    if (reply_status) { status = -EREMOTEIO; goto done; }
    int32_t number;
    if (length < 8) { status = -EBADMSG; goto done; }
    memcpy(&number, body + 4, 4);
    if (number <= 0 || (uint32_t)number > (length - 8) / 28 ||
        length != 8 + (uint32_t)number * 28) { status = -EBADMSG; goto done; }
    AncoGraphicDisplay *displays = calloc((size_t)number, sizeof(*displays));
    if (!displays) { status = -ENOMEM; goto done; }
    for (int32_t i = 0; i < number; ++i) {
        const unsigned char *item = body + 8 + (size_t)i * 28;
        AncoGraphicDisplay *d = &displays[i];
        memcpy(&d->id, item, 8);
        memcpy(&d->width, item + 8, 4);
        memcpy(&d->height, item + 12, 4);
        memcpy(&d->xdpi, item + 16, 4);
        memcpy(&d->ydpi, item + 20, 4);
        memcpy(&d->refresh_hz, item + 24, 4);
        if (d->width <= 0 || d->height <= 0 || !isfinite(d->xdpi) ||
            !isfinite(d->ydpi) || d->xdpi <= 0 || d->ydpi <= 0 ||
            !d->refresh_hz) { status = -EBADMSG; break; }
        for (int32_t j = 0; j < i; ++j)
            if (displays[j].id == d->id) { status = -EBADMSG; break; }
        if (status) break;
    }
    if (status) free(displays);
    else { *out = displays; *count = (size_t)number; }
done:
    close(fd);
    return status;
}
