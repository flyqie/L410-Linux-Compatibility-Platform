#!/bin/bash
# 引擎宿主 N-API 模块。强类型 OHOS C++ Surface 适配器作为同 HAP
# 中的独立共享库构建，由此模块通过稳定的 C ABI 加载。
set -euo pipefail
export PATH=/usr/bin:/bin
r=/mnt/e/harmony6
b=$r/linux-compat
s=$r/had-w32-6.0-analysis/extracted/system/system/lib64
n=$r/reference-openharmony-distributeddatamgr_kv_store/kvstoremock/external_inner_kit/napi/include
mkdir -p "$b/compat-engine/out"

echo "== 链接成模块 =="
aarch64-linux-gnu-gcc -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Wall -Wextra -fPIC -mno-outline-atomics \
  -I"$n" -I"$b/src/ohos-client" -shared -nostdlib -Wl,--build-id=none,--allow-shlib-undefined \
  -o "$b/compat-engine/out/libl410engine.so" \
  "$b/compat-engine/host/native_engine_host.c" "$b/compat-engine/host/l410_broker_window_server.c" \
  -Wl,--no-as-needed "$b/build/host/libl410_anco_surface_typed.z.so" \
  -L"$s" -L"$s/platformsdk" -Wl,--no-as-needed,-l:libace_ndk.z.so,-l:libace_napi.z.so,-l:libc.so

size=$(stat -c %s "$b/compat-engine/out/libl410engine.so")
echo "BUILT libl410engine.so $size bytes"
[ "$size" -gt 10000 ] || { echo "too small, build failed"; exit 1; }
