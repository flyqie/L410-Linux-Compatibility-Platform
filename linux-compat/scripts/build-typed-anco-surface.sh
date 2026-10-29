#!/bin/bash
# Build the typed OHOS Surface -> GraphicBroker adapter.
set -euo pipefail
# Previous device experiments installed ARM64 command shims in ~/bin.  Host
# builds must use the WSL x86_64 utilities and explicit cross compilers.
export PATH=/usr/bin:/bin

ROOT=/mnt/e/harmony6
WORK=${L410_TYPED_WORK:-$HOME/l410-typed-window}
SYS=${L410_QT_SYSROOT:-$HOME/qtos/sysroot}
WRAP=${L410_QT_WRAPPERS:-$HOME/qtos/wrappers}
SRC=$ROOT/linux-compat/compat-engine/host/l410_anco_surface_typed.cpp
INPUT_SRC=$ROOT/linux-compat/compat-engine/host/l410_anco_input_typed.cpp
LIFECYCLE_SRC=$ROOT/linux-compat/compat-engine/host/l410_anco_lifecycle_typed.cpp
PROBE_SRC=$ROOT/linux-compat/compat-engine/host/l410_anco_typed_probe.cpp
TRIGGER_SRC=$ROOT/linux-compat/compat-engine/host/l410_broker_trigger.cpp
OUT=$ROOT/linux-compat/build/host/libl410_anco_surface_typed.z.so
PROBE_OUT=$ROOT/linux-compat/build/host/l410-anco-typed-probe
TRIGGER_OUT=$ROOT/linux-compat/build/host/libl410_broker_trigger.z.so
SYS_LIB=$ROOT/had-w32-6.0-analysis/extracted/system/system/lib64
SURFACE_LIB=$SYS_LIB/chipset-pub-sdk/libsurface.z.so
CLANG_RES=/usr/lib/llvm-18/lib/clang/18/include
MUSL_CRT=$ROOT/toolchains/aarch64-linux-musl-cross/aarch64-linux-musl/lib

test -f "$SRC"
test -f "$SURFACE_LIB"
test -d "$SYS/include/libcxx"
test -d "$SYS/include/musl"
mkdir -p "$WORK" "$(dirname "$OUT")"

CXXFLAGS=(
  --target=aarch64-linux-ohos -std=c++17 -fno-emulated-tls -fno-rtti -fPIC -O2
  -nostdinc -nostdinc++ -D_LIBCPP_ABI_NAMESPACE=__h
  -include "$WRAP/ohos-prelude.h"
  -isystem "$SYS/include/libcxx" -isystem "$SYS/include/libcxxabi"
  -I "$CLANG_RES" -isystem "$SYS/include/musl"
  -isystem "$ROOT/upstream-third_party-musl/arch/aarch64"
  -isystem "$ROOT/upstream-third_party-musl/arch/generic"
  -I "$ROOT/linux-compat/toolchain/include"
  -I "$ROOT/.tmp-openharmony-ipc-60/interfaces/innerkits/ipc_core/include"
  -isystem /usr/aarch64-linux-gnu/include
  -I "$ROOT/upstream-graphic-surface/interfaces/inner_api/common"
  -I "$ROOT/upstream-graphic-surface/interfaces/inner_api/surface"
  -I "$ROOT/upstream-graphic-surface/interfaces/inner_api/sync_fence"
  -I "$ROOT/upstream-graphic-surface/interfaces/inner_api/utils"
  -I "$ROOT/upstream-graphic-surface/surface/include"
  -I "$ROOT/reference-openharmony-graphic_graphic_2d/rosen/modules/platform/utils"
  -I "$ROOT/reference-openharmony-graphic_graphic_2d/rosen/modules/platform/ipc_core"
  -I "$ROOT/.tmp-openharmony-input/interfaces/native/innerkits/proxy/include"
  -I "$ROOT/.tmp-openharmony-input/interfaces/native/innerkits/event/include"
)

clang++-18 "${CXXFLAGS[@]}" -c "$SRC" -o "$WORK/l410_anco_surface_typed.o"
clang++-18 "${CXXFLAGS[@]}" -c "$INPUT_SRC" -o "$WORK/l410_anco_input_typed.o"
clang++-18 "${CXXFLAGS[@]}" -c "$LIFECYCLE_SRC" -o "$WORK/l410_anco_lifecycle_typed.o"
clang++-18 --target=aarch64-linux-ohos -fuse-ld=lld -nostdlib -shared \
  -Wl,--no-undefined -Wl,--build-id=none -Wl,-soname,libl410_anco_surface_typed.z.so \
  "$WORK/l410_anco_surface_typed.o" "$WORK/l410_anco_input_typed.o" \
  "$WORK/l410_anco_lifecycle_typed.o" "$SURFACE_LIB" \
  "$SYS_LIB/chipset-pub-sdk/libutils.z.so" \
  "$SYS_LIB/chipset-sdk-sp/libipc_single.z.so" \
  "$SYS_LIB/libgraphic_broker_client.z.so" \
  "$SYS_LIB/libgraphic_broker_proxy.z.so" \
  "$SYS_LIB/platformsdk/libmmi-client.z.so" \
  "$SYS_LIB/platformsdk/libinputmethod_client.z.so" \
  -L "$SYS_LIB" -lc++ -lc -o "$OUT"

file "$OUT"
readelf -d "$OUT" | grep NEEDED

clang++-18 "${CXXFLAGS[@]}" -c "$PROBE_SRC" -o "$WORK/l410_anco_typed_probe.o"
clang++-18 --target=aarch64-linux-ohos -fuse-ld=lld -nostdlib \
  -pie -Wl,--build-id=none -Wl,--dynamic-linker,/lib/ld-musl-aarch64.so.1 \
  "$MUSL_CRT/Scrt1.o" "$MUSL_CRT/crti.o" \
  "$WORK/l410_anco_typed_probe.o" "$OUT" \
  -L "$SYS_LIB" -lc++ -lc "$MUSL_CRT/crtn.o" -o "$PROBE_OUT"
file "$PROBE_OUT"

clang++-18 "${CXXFLAGS[@]}" -c "$TRIGGER_SRC" -o "$WORK/l410_broker_trigger.o"
clang++-18 --target=aarch64-linux-ohos -fuse-ld=lld -nostdlib -shared \
  -Wl,--no-undefined -Wl,--build-id=none -Wl,-soname,libl410_broker_trigger.z.so \
  "$WORK/l410_broker_trigger.o" -L "$SYS_LIB" -lc++ -lc -o "$TRIGGER_OUT"
file "$TRIGGER_OUT"
