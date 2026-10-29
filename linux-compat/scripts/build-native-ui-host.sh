#!/bin/bash
# Native ArkUI control host: builds the module that turns the widget protocol
# into ArkUI components inside a node type XComponent.
set -euo pipefail
r=/mnt/e/harmony6
b=$r/linux-compat
s=$r/had-w32-6.0-analysis/extracted/system/system/lib64
n=$r/reference-openharmony-distributeddatamgr_kv_store/kvstoremock/external_inner_kit/napi/include
x=$r/reference-openharmony-arkui_ace_engine/interfaces/native
aarch64-linux-gnu-gcc -O2 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Wall -Wextra -fPIC \
  -mno-outline-atomics -I"$n" -I"$x" -shared -nostdlib -Wl,--build-id=none,--allow-shlib-undefined \
  -o "$b/out/libl410uihost.so" "$b/src/native_ui_host.c" \
  -L"$s" -L"$s/platformsdk" \
  -Wl,--no-as-needed,-l:libace_ndk.z.so,-l:libace_napi.z.so,-l:libc.so
echo BUILT "$(stat -c %s "$b/out/libl410uihost.so") bytes"
