#!/bin/bash
cd "$(dirname "$0")/../.."

BIN=bin/macosx-arm64-metal/Release
status=0
for t in metal_fan_test metal_format_test metal_inst_test metal_keys_test metal_pass_test; do
	echo "== $t"
	"$BIN/$t" || status=1
done

SMOKE=$BIN/metal_smoke
if [ "$METAL_SMOKE_ASAN" = 1 ]; then
	SMOKE=${SMOKE}_asan
fi
errlog=$(mktemp)
trap 'rm -f "$errlog"' EXIT

echo "== metal_smoke"
MTL_DEBUG_LAYER=1 MTL_DEBUG_LAYER_ERROR_MODE=assert MTL_DEBUG_LAYER_WARNING_MODE=ignore \
	"$SMOKE" 2> "$errlog" || status=1
cat "$errlog" >&2

if ! grep -q "Metal API Validation Enabled" "$errlog"; then
	echo "Metal validation layer did not load" >&2
	exit 1
fi
if grep -Eq "MTLDebug|failed assertion|command buffer error" "$errlog"; then
	echo "Metal error on stderr" >&2
	exit 1
fi
exit $status
