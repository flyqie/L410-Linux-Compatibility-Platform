#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin
b=/mnt/e/harmony6/linux-compat
# Candidate stays separate from the proven offscreen ICD.
aarch64-linux-gnu-gcc -O2 -Wall -Wextra -Wno-unused-parameter -mno-outline-atomics \
 -Wl,--build-id=none -I/mnt/e/harmony6/analysis_upstream_vulkan_headers_ohos/include \
 -DL410_X11_WSI -DVK_USE_PLATFORM_XCB_KHR -DVK_USE_PLATFORM_XLIB_KHR \
 -fPIC -shared -Wl,-Bsymbolic,-z,nodelete "$b/src/hybris_vulkan_icd.c" \
 -L"$b/out/hybris" -Wl,-l:libhybris-common.so.1.0.0 -L"$b/out" -lX11 \
 -o "$b/out/libvulkan-l410-wsi.so"
