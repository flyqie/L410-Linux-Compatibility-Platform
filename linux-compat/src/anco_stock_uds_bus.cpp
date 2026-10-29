#include "anco_stock_uds_bus.hpp"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>

extern "C" int ConnectUdsServer(const char *, const char *, int, const char *);
extern "C" int GetFd(int);
extern "C" void CloseConn(int);

AncoStockInputBus::~AncoStockInputBus()
{
    Close();
}

bool AncoStockInputBus::Open()
{
    if (connection_ >= 0) return true;
    // Host UdsChannel C ABI: os_type=1 is the verified Harmony-side value.
    // The original guest UdsRpcBus sends this NUL-terminated client name.
    connection_ = ConnectUdsServer("virt_input_broker", "virtinput.sock", 1, "hmos");
    if (connection_ < 0) return false;
    fd_ = ::GetFd(connection_);
    if (fd_ < 0 || Send("input", sizeof("input")) != sizeof("input")) {
        Close();
        return false;
    }
    return true;
}

int AncoStockInputBus::Recv(void *buffer, size_t length, int flags)
{
    if (fd_ < 0) return -1;
    ssize_t count;
    do {
        count = recv(fd_, buffer, length, flags);
    } while (count < 0 && errno == EINTR);
    return static_cast<int>(count);
}

int AncoStockInputBus::Send(const void *buffer, int length)
{
    if (fd_ < 0 || length < 0) return -1;
    auto *bytes = static_cast<const unsigned char *>(buffer);
    int total = 0;
    while (total < length) {
        ssize_t sent = send(fd_, bytes + total,
                            static_cast<size_t>(length - total), MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return -1;
        total += static_cast<int>(sent);
    }
    return total;
}

void AncoStockInputBus::Close()
{
    if (connection_ < 0) return;
    if (fd_ >= 0) shutdown(fd_, SHUT_RDWR);
    CloseConn(connection_);
    connection_ = -1;
    fd_ = -1;
}

int AncoStockInputBus::GetFd()
{
    return fd_;
}
