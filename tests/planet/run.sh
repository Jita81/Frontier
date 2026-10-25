#!/bin/sh
# Planet tests: S-10 (radius normalization), S-11/S-21 (feature parser
# and rasterizer fuzz), S-9/S-23 (the planet code of gl.c under ASan).
# Usage: tests/planet/run.sh   (from anywhere; needs clang or gcc)
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
out=${TMPDIR:-/tmp}/frontier-planet-tests.$$
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT
CC=${CC:-cc}
# the game's own flags that change semantics (-fwrapv), plus the sanitizers
SAN="-fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all"
CFLAGS="-O1 -g -fwrapv -Wall -Wno-unused $SAN"
export ASAN_OPTIONS=abort_on_error=0:detect_leaks=0
export UBSAN_OPTIONS=print_stacktrace=1

# the planet block of gl.c and the colour helper it uses
awk '/==== PLANET BEGIN ====/{f=1} f{print} /==== PLANET END ====/{exit}' \
	"$top/src/gl.c" > "$out/planet_block.c"
grep -q 'PLANET END' "$out/planet_block.c"
awk '/^static inline void split_rgb444b/{f=1} f{print} f&&/^}/{exit}' \
	"$top/src/gl.c" > "$out/split_rgb.c"

$CC $CFLAGS -o "$out/norm" "$here/test_norm_radius.c" "$top/src/planet.c" -lm
$CC $CFLAGS -o "$out/fuzz" "$here/fuzz_features.c" "$top/src/planet.c" -lm
$CC $CFLAGS -Wno-pointer-arith -Wno-deprecated-declarations -I"$out" -I"$here" \
	-o "$out/render" "$here/test_render.c" "$top/src/planet.c" -lm

"$out/norm"
"$out/fuzz" ${FUZZ_ITERS:-10000}
"$out/render"
echo "planet tests: all passed"
