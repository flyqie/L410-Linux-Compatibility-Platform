/* Short diagnostic of the original IUdsBus ABI, not an input receiver. */
#include "anco_stock_uds_bus.hpp"

#include <cstdio>
#include <unistd.h>

int main()
{
    AncoStockInputBus bus;
    if (!bus.Open()) {
        puts("STOCK_BUS_OPEN failed");
        return 2;
    }
    printf("STOCK_BUS_OPEN fd_valid=%d\n", bus.GetFd() >= 0);
    fflush(stdout);
    sleep(2);
    bus.Close();
    puts("STOCK_BUS_CLOSE done");
    return 0;
}
