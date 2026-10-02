#!/bin/bash
set -e
cd "$(dirname "$0")/../.."

case "$METAL_SMOKE_HOST" in
""|glfw|cocoa) ;;
*) echo "METAL_SMOKE_HOST must be glfw or cocoa" >&2; exit 1 ;;
esac

OPTS=
if [ "$METAL_SMOKE_ASAN" = 1 ]; then
	OPTS=--metal-asan
fi
premake5 $OPTS gmake2
if [ "$METAL_SMOKE_HOST" = cocoa ]; then
	make -C build config=release_macosx-arm64-metal-cocoa -j8 librw metal_smoke
	exit 0
fi
make -C build config=release_macosx-arm64-metal -j8 \
	librw metal_fan_test metal_format_test metal_inst_test metal_keys_test metal_pass_test metal_modes_test metal_drawable_test metal_settings_test metal_phase_test metal_sceneblend_test metal_output_test metal_bloom_test metal_dering_test metal_aa_test metal_prepass_test metal_temporal_test metal_relit_test metal_normals_test metal_shadow_test metal_smoke
