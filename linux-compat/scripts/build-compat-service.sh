#!/bin/bash
# 交叉编译容器生命周期系统服务（静态链接，设备上 root 直接跑；不依赖 OHOS 的 musl）。
set -e
cd /mnt/e/harmony6/linux-compat
mkdir -p compat-engine/out
aarch64-linux-gnu-gcc -static -O2 -Wall -Wextra -o compat-engine/out/l410-compat-serviced \
    compat-engine/service/l410_compat_service.c
ls -l compat-engine/out/l410-compat-serviced
echo "BUILT l410-compat-serviced $(stat -c%s compat-engine/out/l410-compat-serviced) bytes"
