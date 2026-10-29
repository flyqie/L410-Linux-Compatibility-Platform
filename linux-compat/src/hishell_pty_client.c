#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <sys/un.h>
#include <unistd.h>

/* Standard-stream relay used as HiShell's child command for one tab. */
static int write_all(int fd, const char *data, size_t size) {
    while (size) {
        ssize_t written = write(fd, data, size);
        if (written < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        data += written;
        size -= (size_t)written;
    }
    return 0;
}

int main(void) {
    struct termios saved;
    int have_tty = tcgetattr(STDIN_FILENO, &saved) == 0;
    if (have_tty) {
        struct termios raw = saved;
        cfmakeraw(&raw);
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) have_tty = 0;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("Linux Shell socket");
        if (have_tty) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        return 1;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    const char name[] = "l410-hishell";
    memcpy(address.sun_path + 1, name, sizeof(name) - 1);
    socklen_t length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + sizeof(name) - 1);
    if (connect(fd, (struct sockaddr *)&address, length) != 0) {
        fprintf(stderr, "Linux 兼容服务未运行或 Shell 服务未就绪：%s\r\n", strerror(errno));
        close(fd);
        if (have_tty) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        return 1;
    }
    const char hello[] = "L410_HISHELL_V1\n";
    if (write_all(fd, hello, sizeof(hello) - 1) != 0) {
        if (have_tty) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        return 1;
    }

    struct pollfd pollers[2] = {{STDIN_FILENO, POLLIN, 0}, {fd, POLLIN, 0}};
    char buffer[8192];
    for (;;) {
        int ready = poll(pollers, 2, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pollers[0].revents & POLLIN) {
            ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count <= 0 || write_all(fd, buffer, (size_t)count) != 0) break;
        }
        if (pollers[1].revents & POLLIN) {
            ssize_t count = read(fd, buffer, sizeof(buffer));
            if (count <= 0 || write_all(STDOUT_FILENO, buffer, (size_t)count) != 0) break;
        }
        if (pollers[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (pollers[1].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
    }
    close(fd);
    if (have_tty) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    return 0;
}
