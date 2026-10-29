/* Diagnostic FFI boundary to the stock Harmony UdsClientInvoker.
 * The vendor constructor writes through offset 448. This deliberately
 * overallocates the opaque object and calls only its exported methods.
 * It is not a production declaration of the private class. */
#include "anco_stock_uds_bus.hpp"

#include <cstdio>
#include <memory>
#include <unistd.h>

namespace UdsIpc {
class UdsClientInvoker {
public:
    explicit UdsClientInvoker(std::shared_ptr<IUdsBus> bus);
    ~UdsClientInvoker();
    bool Open();
    void Close();

private:
    alignas(16) unsigned char vendor_storage_[512];
};
} // namespace UdsIpc

int main()
{
    std::shared_ptr<UdsIpc::IUdsBus> bus =
        std::make_shared<AncoStockInputBus>();
    UdsIpc::UdsClientInvoker invoker(bus);
    bool opened = invoker.Open();
    printf("STOCK_INVOKER_OPEN result=%d\n", opened ? 1 : 0);
    fflush(stdout);
    if (opened) sleep(2);
    invoker.Close();
    puts("STOCK_INVOKER_CLOSE done");
    return opened ? 0 : 2;
}
