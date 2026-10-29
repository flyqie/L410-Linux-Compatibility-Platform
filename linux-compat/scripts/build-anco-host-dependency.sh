#!/bin/sh
# Cross-build only. Run the output on actual L410, never through QEMU.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
deps=$root/analysis_sp68_sceneboard_lifecycle/wms-no-parent-arm64-20260930
sysroot=$root/linux-compat/out/wayland-arm64-20261005/sysroot
out=${1:?supply output directory}
mkdir -p "$out"
python3 "$root/linux-compat/scripts/build-anco-binder-hitrace.py" "$out/binder-ohos"
headers=$(aarch64-linux-gnu-gcc -print-file-name=include)
aarch64-linux-gnu-gcc -std=c11 -O2 -Wall -Wextra -Werror \
 --sysroot="$sysroot" -B"$sysroot/usr/lib/aarch64-linux-gnu/" \
 -nostdinc -isystem "$headers" -isystem "$sysroot/usr/include/aarch64-linux-gnu" \
 -isystem "$sysroot/usr/include" -I"$sysroot/usr/include/glib-2.0" \
 -I"$sysroot/usr/lib/aarch64-linux-gnu/glib-2.0/include" \
 -I"$deps/gbinder/include" -I"$deps/gbinder/src" -I"$deps/glibutil/include" \
 -I"$root/linux-compat/src" "$root/linux-compat/src/anco_host_dependency.c" \
 -Wl,--build-id=none -Wl,--start-group \
 "$out/binder-ohos/libgbinder.a" "$deps/glibutil/build/release/libglibutil.a" \
 -Wl,--end-group -L"$sysroot/usr/lib/aarch64-linux-gnu" \
 -Wl,-rpath-link,"$sysroot/usr/lib/aarch64-linux-gnu" \
 -Wl,-rpath-link,"$sysroot/lib/aarch64-linux-gnu" \
 -lgobject-2.0 -lglib-2.0 -pthread -o "$out/anco-host-dependency"
