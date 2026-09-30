#!/bin/bash
set -e
cd "$(dirname "$0")/../.."

OPTS=
if [ "$METAL_SMOKE_ASAN" = 1 ]; then
	OPTS=--metal-asan
fi
premake5 $OPTS gmake2
make -C build config=release_macosx-arm64-metal -j8 \
	librw metal_fan_test metal_format_test metal_inst_test metal_keys_test metal_pass_test metal_smoke
