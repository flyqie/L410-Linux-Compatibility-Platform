#!/bin/sh
# Build an isolated wlroots 0.18.2 candidate with its standard rootless
# Xwayland/XWM implementation.  The active runtime and its SDK are untouched.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
base="$root/linux-compat/out/wayland-arm64-20261005"
source="$root/analysis_sp68_sceneboard_lifecycle/wlroots-protocol-sdk-20261004-r2/sources/wlroots-0.18.2"
debs="$root/linux-compat/out/xwayland-sdk-debs"
out=${1:-"$root/linux-compat/out/wayland-arm64-xwayland-20261014"}
sysroot="$out/sysroot"
build="$out/build/wlroots-arm64"

test -f "$root/linux-compat/build-support/wlroots-cross.ini"
test -f "$source/meson.build"
test -d "$debs"
test ! -e "$out"

mkdir -p "$out/build"
cp -a "$base/sysroot" "$sysroot"
for package in "$debs"/*.deb; do
    dpkg-deb -x "$package" "$sysroot"
done

sed "s|$base/sysroot|$sysroot|g" "$root/linux-compat/build-support/wlroots-cross.ini" > "$out/cross.ini"

original="$root/analysis_sp68_sceneboard_lifecycle/wlroots-protocol-sdk-20261004-r2"
export PATH="$original/prefix/bin:$PATH"
export PKG_CONFIG_PATH="$original/prefix/lib/pkgconfig:$original/prefix/share/pkgconfig"
export PKG_CONFIG_PATH_FOR_BUILD="$PKG_CONFIG_PATH"

meson setup "$build" "$source" \
    --cross-file "$out/cross.ini" \
    --prefix=/usr --libdir=lib/aarch64-linux-gnu --wrap-mode=nodownload \
    -Dc_std=c2x -Dauto_features=disabled -Dexamples=false \
    -Dbackends=[] -Drenderers=[] -Dallocators=[] -Dsession=disabled \
    -Dxwayland=enabled -Dxcb-errors=disabled -Dcolor-management=disabled
python3 - "$out/cross.ini" "$build" <<'PY'
import configparser, subprocess, sys
c = configparser.ConfigParser()
c.read(sys.argv[1])
subprocess.run([
    "meson", "configure", sys.argv[2],
    "-Dc_args=" + c["built-in options"]["c_args"],
], check=True)
PY
ninja -C "$build" -j4
DESTDIR="$sysroot" meson install -C "$build" --no-rebuild

grep -q '#define WLR_HAS_XWAYLAND 1' \
    "$sysroot/usr/include/wlroots-0.18/wlr/config.h"
readelf -Ws "$sysroot/usr/lib/aarch64-linux-gnu/libwlroots-0.18.so" \
    | grep -q 'wlr_xwayland_create'

printf '%s\n' 'ANCO_XWAYLAND_WLROOTS_BUILD_PASS'
