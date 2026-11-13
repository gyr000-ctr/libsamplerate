#!/usr/bin/env bash
#
# End-to-end, non-interactive verification of the dual precision paths.
# Uses only the repository's own build system and ctest entry points plus
# the independent Python reference checker. Produces every artifact under
# output/. No keyboard input is required at any step.
#
# Usage:  output/run_verification.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD="$ROOT/build-verify"
OUT="$HERE"

run_mode () {
	local mode="$1" define="$2" tag="$3"
	echo "==> configuring $tag build ($mode)"
	cmake -S "$ROOT" -B "$BUILD-$tag" -DCMAKE_BUILD_TYPE=Debug \
		-DLIBSAMPLERATE_DOUBLE_PRECISION="$define" \
		-DLIBSAMPLERATE_EXAMPLES=OFF > "$OUT/cmake_$tag.log" 2>&1
	echo "==> building $tag"
	cmake --build "$BUILD-$tag" -j"$(nproc)" > "$OUT/build_$tag.log" 2>&1
	echo "==> ctest $tag"
	( ctest --test-dir "$BUILD-$tag" --output-on-failure ) 2>&1 | tee "$OUT/ctest_$tag.log"
	cp "$BUILD-$tag/src/double_precision_dump.txt" "$OUT/dump_$tag.txt"
}

run_mode "float (default)"  OFF float
run_mode "double"           ON  double

echo "==> independent double precision reference check"
python3 -I "$OUT/precision_check.py" \
	"$OUT/dump_float.txt" "$OUT/dump_double.txt" "$ROOT/src" \
	"$OUT/precision_report.json" "$OUT/precision_report.txt"

echo "==> artifacts written to $OUT"
