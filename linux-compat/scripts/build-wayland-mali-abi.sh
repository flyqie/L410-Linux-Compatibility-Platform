#!/bin/bash
set -euo pipefail
b=/mnt/e/harmony6/linux-compat
work=/var/tmp/l410-wayland-mali-abi
sdk=$b/out/wayland-sdk/usr
mkdir -p "$work" "$b/out/wayland/mali-abi"
if [ ! -d "$work/wayland-1.21.0" ]; then
 tar -xJf "$b/vendor/wayland-1.21.0.tar.xz" -C "$work"
fi
python3 - "$work/wayland-1.21.0/src/wayland-client.c" <<'PY'
import sys
from pathlib import Path
p=Path(sys.argv[1]); s=p.read_text()
a='static void\nwl_proxy_unref(struct wl_proxy *proxy)'
b='WL_EXPORT void wl_proxy_unref(struct wl_proxy *proxy);\n\nWL_EXPORT void\nwl_proxy_unref(struct wl_proxy *proxy)'
if a in s:
 assert s.count(a)==1
 p.write_text(s.replace(a,b))
else:
 assert b in s
s=p.read_text()
anchor='WL_EXPORT void wl_proxy_unref(struct wl_proxy *proxy);'
ref='WL_EXPORT struct wl_proxy *wl_proxy_ref(struct wl_proxy *proxy);\nWL_EXPORT struct wl_proxy *wl_proxy_ref(struct wl_proxy *proxy) { proxy->refcount++; return proxy; }\n\n'
if 'WL_EXPORT struct wl_proxy *wl_proxy_ref' not in s:p.write_text(s.replace(anchor,ref+anchor))
s=p.read_text()
getref='WL_EXPORT int wl_proxy_get_refcount(struct wl_proxy *proxy);\nWL_EXPORT int wl_proxy_get_refcount(struct wl_proxy *proxy) { assert(proxy && proxy->refcount > 0); return proxy->refcount; }\n\n'
if 'WL_EXPORT int wl_proxy_get_refcount' not in s:p.write_text(s.replace(anchor,getref+anchor))
# The explicit strtol call parses a decimal WAYLAND_SOCKET fd. Use the baseline
# ABI explicitly; newer cross headers redirect strtol to GLIBC_2.38 C23 ABI.
s=p.read_text()
old='fd = strtol(connection, &end, 10);'
new='extern long l410_decimal_strtol(const char *, char **, int) __asm__("strtol");\n\t\tfd = l410_decimal_strtol(connection, &end, 10);'
if old in s:p.write_text(s.replace(old,new))
else:assert new in s
p=p.with_name('connection.c');s=p.read_text()
old='since = atoi(message->signature);'
new='extern long l410_decimal_strtol(const char *, char **, int) __asm__("strtol");\n\tsince = (int)l410_decimal_strtol(message->signature, NULL, 10);'
if old in s:p.write_text(s.replace(old,new))
else:assert new in s
PY
if [ ! -f "$work/native/build.ninja" ]; then
 CC=/usr/bin/gcc meson setup "$work/native" "$work/wayland-1.21.0" -Dlibraries=false -Dtests=false -Ddocumentation=false -Ddtd_validation=false
fi
ninja -C "$work/native"
mkdir -p "$work/pc-native" "$work/pc-cross"
cat > "$work/pc-native/wayland-scanner.pc" <<EOF
Name: wayland-scanner
Description: isolated native scanner
Version: 1.21.0
wayland_scanner=$work/native/src/wayland-scanner
EOF
cat > "$work/pc-cross/libffi.pc" <<EOF
Name: libffi
Description: target Debian libffi
Version: 3.4.4
Libs: -L$sdk/lib/aarch64-linux-gnu -l:libffi.so.8.1.2
Cflags: -I$sdk/include
EOF
cat > "$work/cross.ini" <<EOF
[binaries]
c = '/usr/bin/aarch64-linux-gnu-gcc'
ar = '/usr/bin/aarch64-linux-gnu-ar'
strip = '/usr/bin/aarch64-linux-gnu-strip'
pkg-config = '/usr/bin/pkg-config'
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
[built-in options]
pkg_config_path = ['$work/pc-cross']
c_args = ['-mno-outline-atomics']
c_link_args = ['-Wl,--build-id=none']
EOF
if [ ! -f "$work/cross/build.ninja" ]; then
 PKG_CONFIG_PATH_FOR_BUILD=$work/pc-native meson setup "$work/cross" "$work/wayland-1.21.0" --cross-file "$work/cross.ini" -Dscanner=false -Dtests=false -Ddocumentation=false -Ddtd_validation=false
fi
ninja -C "$work/cross" src/libwayland-client.so.0.21.0
cp "$work/cross/src/libwayland-client.so.0.21.0" "$b/out/wayland/mali-abi/libwayland-client.so.0"
aarch64-linux-gnu-nm -D "$b/out/wayland/mali-abi/libwayland-client.so.0" | grep -E ' T wl_proxy_(ref|unref|get_refcount|marshal_flags)$'
