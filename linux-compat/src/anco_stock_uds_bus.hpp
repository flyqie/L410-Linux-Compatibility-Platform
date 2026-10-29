#pragma once

#include <stddef.h>

// Reconstructed from the original guest UdsRpcBus vtable and the host
// UdsClientInvoker call sites. Keep this ABI boundary local until the
// original vendor header is available; do not add project-specific RPCs.
namespace UdsIpc {
class IUdsBus {
public:
    virtual ~IUdsBus() = default;
    virtual bool Open() = 0;
    virtual int Recv(void *buffer, size_t length, int flags) = 0;
    virtual int Send(const void *buffer, int length) = 0;
    virtual void Close() = 0;
    virtual int GetFd() = 0;
};
} // namespace UdsIpc

class AncoStockInputBus final : public UdsIpc::IUdsBus {
public:
    ~AncoStockInputBus() override;
    bool Open() override;
    int Recv(void *buffer, size_t length, int flags) override;
    int Send(const void *buffer, int length) override;
    void Close() override;
    int GetFd() override;

private:
    int connection_ = -1;
    int fd_ = -1;
};
