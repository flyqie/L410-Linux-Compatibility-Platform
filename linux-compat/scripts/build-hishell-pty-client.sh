#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin
root=/mnt/e/harmony6
project=$root/linux-compat
sys=${L410_QT_SYSROOT:-$HOME/qtos/sysroot}
crt=$root/toolchains/aarch64-linux-musl-cross/aarch64-linux-musl/lib
hostlib=$root/had-w32-6.0-analysis/extracted/system/system/lib64
out=$project/out/manager-r1
mkdir -p "$out"
clang-18 --target=aarch64-linux-ohos -O2 -Wall -Wextra -Werror \
  -fno-emulated-tls -nostdinc \
  -isystem /usr/lib/llvm-18/lib/clang/18/include \
  -isystem "$sys/include/musl" \
  -isystem "$root/upstream-third_party-musl/arch/aarch64" \
  -isystem "$root/upstream-third_party-musl/arch/generic" \
  -c "$project/src/hishell_pty_client.c" -o "$out/hishell_pty_client.o"
clang-18 --target=aarch64-linux-ohos -fuse-ld=lld -nostdlib -pie \
  -Wl,--build-id=none -Wl,--dynamic-linker,/lib/ld-musl-aarch64.so.1 \
  "$crt/Scrt1.o" "$crt/crti.o" "$out/hishell_pty_client.o" \
  -L"$hostlib" -lc "$crt/crtn.o" -o "$out/l410-hishell-client"
/usr/bin/aarch64-linux-gnu-readelf -d "$out/l410-hishell-client" | /usr/bin/grep NEEDED
