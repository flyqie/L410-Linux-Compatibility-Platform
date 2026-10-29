#!/bin/sh
# Generate upstream protocol bindings; keep generated files out of source.
set -eu
out=${1:?supply generated protocol output directory}
scanner=${WAYLAND_SCANNER:-wayland-scanner}
: "${TEXT_INPUT_V2_XML:?set path to upstream text-input-unstable-v2.xml}"
: "${TEXT_INPUT_V3_XML:?set path to upstream text-input-unstable-v3.xml}"
mkdir -p "$out"
for version in 2 3; do
    case "$version" in
        2) xml=$TEXT_INPUT_V2_XML ;;
        3) xml=$TEXT_INPUT_V3_XML ;;
    esac
    "$scanner" server-header "$xml" "$out/text-input-unstable-v$version-server.h"
    "$scanner" private-code "$xml" "$out/text-input-unstable-v$version-protocol.c"
done
"$scanner" client-header "$TEXT_INPUT_V2_XML" "$out/text-input-unstable-v2-client.h"
