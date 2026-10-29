#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin
b=/mnt/e/harmony6/linux-compat
work=/var/tmp/l410-hybris
mkdir -p "$work" "$b/out/hybris"
test -d "$work/libhybris-master" || tar -xf "$b/vendor/libhybris-master.tar.gz" -C "$work"
test -d "$work/android-headers-29-droidian" || tar -xf "$b/vendor/android-headers-29-droidian.tar.gz" -C "$work"
cd "$work/libhybris-master/hybris"
# Use Debian's runtime C++ ABI rather than embedding the build host libsupc++.
sed -i 's/-lsupc++/-lstdc++/g' common/q/Makefile.am
# Mate30 libc++ queries MB_CUR_MAX during locale construction. Route this
# through the same glibc locale state as the existing locale hooks.
python3 - <<'PY'
from pathlib import Path
p=Path('common/hooks.c');s=p.read_text()
entry='    HOOK_DIRECT_NO_DEBUG(__ctype_get_mb_cur_max),'
if entry not in s:s=s.replace('    /* locale.h */', '    /* locale.h */\n'+entry)
p.write_text(s)
PY
autoreconf -fi
./configure --host=aarch64-linux-gnu --prefix=/opt/l410/hybris \
 --with-android-headers="$work/android-headers-29-droidian" --enable-arch=arm64 \
 --enable-mali-quirks --with-default-hybris-ld-library-path=/opt/l410/vulkan-bionic \
 CFLAGS="-O2 -mno-outline-atomics -include $b/src/hybris_pre_c23.h" \
 CXXFLAGS="-O2 -mno-outline-atomics -include $b/src/hybris_pre_c23.h" \
 LDFLAGS="-Wl,--build-id=none"
make -C common clean
make -C common -j4
cp common/.libs/libhybris-common.so.1.0.0 "$b/out/hybris/"
cp common/q/.libs/q.so "$b/out/hybris/"
