#!/bin/sh
# Cross-build only. Run the output on actual L410, never through QEMU.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
deps=$root/analysis_sp68_sceneboard_lifecycle/wms-no-parent-arm64-20260930
sysroot=${ANCO_WAYLAND_SYSROOT:-$root/linux-compat/out/wayland-arm64-20261005/sysroot}
sdk=$sysroot/usr
out=${1:?supply output directory}
mkdir -p "$out"
protocol="$out/protocol"
sh "$root/linux-compat/scripts/generate-wayland-protocols.sh" "$protocol"
python3 "$root/linux-compat/scripts/build-anco-binder-hitrace.py" "$out/binder-ohos"
headers=$(aarch64-linux-gnu-gcc -print-file-name=include)
aarch64-linux-gnu-gcc -std=gnu11 -DANCO_AMS_AGENT_EMBEDDED -DANCO_HOST_DEPENDENCY_EMBEDDED -O2 -Wall -Wextra -Werror \
 --sysroot="$sysroot" -B"$sysroot/usr/lib/aarch64-linux-gnu/" \
 -nostdinc -isystem "$headers" -isystem "$sysroot/usr/include/aarch64-linux-gnu" \
 -isystem "$sysroot/usr/include" -I"$sysroot/usr/include/glib-2.0" \
 -I"$sysroot/usr/include/ibus-1.0" \
 -I"$root/linux-compat/out/ibus-arm64-bookworm-20261014/ibus-1.0" \
 -I"$sysroot/usr/include/gio-unix-2.0" \
 -I"$sysroot/usr/lib/aarch64-linux-gnu/glib-2.0/include" \
 -I"$deps/gbinder/include" -I"$deps/gbinder/src" -I"$deps/glibutil/include" \
 -I"$sdk/include" -I"$sdk/include/wlroots-0.18" -I"$sdk/include/pixman-1" -I"$sdk/include/libdrm" -I"$root/upstream-third_party-mesa3d/include/android_stub" -I"$root/linux-compat/out/wayland-arm64-20261005/build/wlroots-arm64/protocol" -I"$protocol" -I"$sysroot/usr/include/json-glib-1.0" -I"$root/linux-compat/src" "$root/linux-compat/src/anco_linux_runtime.c" \
 "$root/linux-compat/src/wayland_egl_backend.c" \
 "$root/linux-compat/src/anco_shm_queue.c" "$root/linux-compat/src/anco_gralloc1_device.c" "$root/linux-compat/src/anco_native_handle_legacy.c" \
 "$root/linux-compat/src/anco_ams_agent.c" "$root/linux-compat/src/anco_wayland_context.c" \
 "$root/linux-compat/src/anco_resource_agent.c" \
 "$root/linux-compat/src/anco_text_receiver.c" \
 "$root/linux-compat/src/anco_text_v3_state.c" \
 "$root/linux-compat/src/anco_text_v2_state.c" \
 "$root/linux-compat/src/anco_imf_editor_consumer.c" \
 "$root/linux-compat/src/anco_imf_lifecycle.c" \
 "$root/linux-compat/src/anco_ime_editor.c" \
 "$root/linux-compat/src/anco_imf_client_v340.c" \
 "$root/linux-compat/src/anco_imf_broker.c" \
 "$root/linux-compat/src/anco_imf_callback_codec.c" \
 "$root/linux-compat/src/anco_imf_callback_receiver.c" \
 "$root/linux-compat/src/anco_imf_publication.c" \
 "$root/linux-compat/src/anco_original_ime_key.c" \
 "$root/linux-compat/src/anco_linux_imf_runtime.c" \
 "$root/linux-compat/src/anco_ibus_bridge.c" \
 "$protocol/text-input-unstable-v2-protocol.c" \
 "$protocol/text-input-unstable-v3-protocol.c" \
 "$root/linux-compat/src/anco_linux_client_properties.c" \
 "$root/linux-compat/src/anco_linux_session_bus.c" \
 "$root/linux-compat/src/anco_linux_pasteboard.c" \
 "$root/linux-compat/src/anco_linux_host_windows.c" \
 "$root/linux-compat/src/anco_host_dependency.c" \
 "$root/linux-compat/src/anco_wayland_ownership.c" "$root/linux-compat/src/anco_wlroots_admission.c" \
 "$root/linux-compat/src/anco_linux_catalog.c" "$root/linux-compat/src/anco_linux_ownership.c" \
 "$root/linux-compat/src/anco_bms_registration.c" \
 "$root/linux-compat/src/anco_wms_catalog.c" \
 "$root/linux-compat/src/anco_wms_broker.c" \
 "$root/linux-compat/src/anco_wms_transport.c" \
 "$root/linux-compat/src/anco_wms_layout.c" \
 "$root/linux-compat/src/anco_wms_callback_codec.c" \
 "$root/linux-compat/src/anco_wms_callback_receiver.c" \
 "$root/linux-compat/src/anco_graphic_transport.c" \
 "$root/linux-compat/src/anco_graphic_registration.c" \
 "$root/linux-compat/src/anco_graphic_display.c" \
 "$root/linux-compat/src/anco_graphic_layers.c" \
 "$root/linux-compat/src/anco_wayland_display.c" \
 "$root/linux-compat/src/anco_wayland_geometry.c" \
 "$root/linux-compat/src/anco_delegator_registry.c" \
 "$root/linux-compat/src/anco_surface_identity.c" \
 "$root/linux-compat/src/anco_delegator_producer.c" \
 "$root/linux-compat/src/anco_delegator_receiver.c" \
 "$root/linux-compat/src/anco_delegator_release.c" \
 "$root/linux-compat/src/anco_delegator_release_state.c" \
 "$root/linux-compat/src/anco_delegator_pending_frame.c" \
 "$root/linux-compat/src/anco_delegator_metadata_state.c" \
 "$root/linux-compat/src/anco_delegator_metadata.c" \
 "$root/linux-compat/src/anco_delegator_transport.c" \
 "$root/linux-compat/src/anco_delegator_codec.c" \
 -Wl,--build-id=none -Wl,--start-group \
 "$out/binder-ohos/libgbinder.a" "$deps/glibutil/build/release/libglibutil.a" \
 -Wl,--end-group -L"$sysroot/usr/lib/aarch64-linux-gnu" \
 -Wl,-rpath-link,"$sysroot/usr/lib/aarch64-linux-gnu" \
 -Wl,-rpath-link,"$sysroot/lib/aarch64-linux-gnu" \
 -L"$sdk/lib/aarch64-linux-gnu" -Wl,-rpath-link,"$sdk/lib/aarch64-linux-gnu" -lwlroots-0.18 -lpixman-1 -lwayland-server -lxkbcommon -l:libxcb.so.1.1.0 -ljson-glib-1.0 -libus-1.0 -lgio-2.0 -lgobject-2.0 -lglib-2.0 -lm -ldl -pthread -o "$out/anco-linux-runtime"

# The deployed XWayland-enabled wlroots has an isolated SONAME so it cannot
# replace the rootfs' non-XWayland ABI.  Keep the runtime bound to that exact
# library; omitting this packaging step makes the loader select the generic
# libwlroots-0.18.so and fail before main() at wlr_xwayland_create.
/usr/bin/patchelf --replace-needed libwlroots-0.18.so \
 libwlroots-xwayland-r94.so "$out/anco-linux-runtime"
/usr/bin/readelf -d "$out/anco-linux-runtime" | \
 grep -q 'Shared library: \[libwlroots-xwayland-r94.so\]'

aarch64-linux-gnu-gcc -std=gnu11 -O2 -Wall -Wextra -Werror \
 --sysroot="$sysroot" -B"$sysroot/usr/lib/aarch64-linux-gnu/" \
 -nostdinc -isystem "$headers" -isystem "$sysroot/usr/include/aarch64-linux-gnu" \
 -isystem "$sysroot/usr/include" -I"$sysroot/usr/include/glib-2.0" \
 -I"$sysroot/usr/lib/aarch64-linux-gnu/glib-2.0/include" \
 -I"$deps/gbinder/include" -I"$deps/gbinder/src" -I"$deps/glibutil/include" \
 -I"$root/linux-compat/src" \
 "$root/linux-compat/src/anco_resource_provider.c" \
 "$root/linux-compat/src/anco_resource_agent.c" \
 -Wl,--build-id=none -Wl,--start-group \
 "$out/binder-ohos/libgbinder.a" "$deps/glibutil/build/release/libglibutil.a" \
 -Wl,--end-group -L"$sysroot/usr/lib/aarch64-linux-gnu" \
 -Wl,-rpath-link,"$sysroot/usr/lib/aarch64-linux-gnu" \
 -Wl,-rpath-link,"$sysroot/lib/aarch64-linux-gnu" \
 -lgio-2.0 -lgobject-2.0 -lglib-2.0 -pthread -o "$out/anco-resource-provider"
