#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin
root=/mnt/e/harmony6
project=$root/linux-compat
sys=${L410_QT_SYSROOT:-$HOME/qtos/sysroot}
out=$project/build/anco-uds/anco-stock-uds-bus.o
smoke=$project/build/anco-uds/anco-stock-uds-bus-smoke
invoker_smoke=$project/build/anco-uds/anco-stock-uds-invoker-smoke
hostlib=$root/had-w32-6.0-analysis/extracted/system/system/lib64
channel=$root/had-w32-analysis/extracted/system/p111-system/system/lib64/chipset-sdk/libuds_channel.z.so
crt=$root/toolchains/aarch64-linux-musl-cross/aarch64-linux-musl/lib
test -d "$sys/include/libcxx"
test -d "$sys/include/musl"
test -r "$channel" && test -r "$hostlib/libc++.so"
mkdir -p "$(dirname "$out")"
clang++-18 --target=aarch64-linux-ohos -std=c++17 -O2 -Wall -Wextra -Werror \
  -D_LIBCPP_ABI_NAMESPACE=__h -fno-exceptions -fno-rtti -fno-emulated-tls \
  -nostdinc++ \
  -isystem /usr/lib/llvm-18/lib/clang/18/include \
  -isystem "$sys/include/libcxx" -isystem "$sys/include/musl" \
  -isystem "$root/upstream-third_party-musl/arch/aarch64" \
  -isystem "$root/upstream-third_party-musl/arch/generic" \
  -c "$project/src/anco_stock_uds_bus.cpp" -o "$out"
aarch64-linux-gnu-nm -C "$out" | grep -E 'AncoStockInputBus::(Open|Recv|Send|Close|GetFd)|vtable for AncoStockInputBus'
clang++-18 --target=aarch64-linux-ohos -std=c++17 -O2 -Wall -Wextra -Werror \
  -D_LIBCPP_ABI_NAMESPACE=__h -fno-exceptions -fno-rtti -fno-emulated-tls \
  -nostdinc++ \
  -isystem "$sys/include/libcxx" -isystem "$sys/include/musl" \
  -isystem "$root/upstream-third_party-musl/arch/aarch64" \
  -isystem "$root/upstream-third_party-musl/arch/generic" \
  -c "$project/src/anco_stock_uds_bus_smoke.cpp" \
  -o "$project/build/anco-uds/anco-stock-uds-bus-smoke.o"
clang++-18 --target=aarch64-linux-ohos -fuse-ld=lld -nostdlib -pie \
  -Wl,--build-id=none -Wl,--dynamic-linker,/lib/ld-musl-aarch64.so.1 \
  "$crt/Scrt1.o" "$crt/crti.o" \
  "$out" "$project/build/anco-uds/anco-stock-uds-bus-smoke.o" \
  "$channel" "$hostlib/libc++.so" -L"$hostlib" -lc \
  "$crt/crtn.o" -o "$smoke"
readelf -d "$smoke" | grep NEEDED
clang++-18 --target=aarch64-linux-ohos -std=c++17 -O2 -Wall -Wextra -Werror \
  -D_LIBCPP_ABI_NAMESPACE=__h -fno-exceptions -fno-rtti -fno-emulated-tls \
  -nostdinc++ \
  -isystem "$sys/include/libcxx" -isystem "$sys/include/musl" \
  -isystem "$root/upstream-third_party-musl/arch/aarch64" \
  -isystem "$root/upstream-third_party-musl/arch/generic" \
  -c "$project/src/anco_stock_uds_invoker_smoke.cpp" \
  -o "$project/build/anco-uds/anco-stock-uds-invoker-smoke.o"
clang++-18 --target=aarch64-linux-ohos -fuse-ld=lld -nostdlib -pie \
  -Wl,--build-id=none -Wl,--dynamic-linker,/lib/ld-musl-aarch64.so.1 \
  "$crt/Scrt1.o" "$crt/crti.o" \
  "$out" "$project/build/anco-uds/anco-stock-uds-invoker-smoke.o" \
  "$channel" "$hostlib/libuds_ipc_client.z.so" "$hostlib/libuds_ipc_core.z.so" \
  "$hostlib/libc++.so" -L"$hostlib" -lc \
  "$crt/crtn.o" -o "$invoker_smoke"
readelf -d "$invoker_smoke" | grep NEEDED
