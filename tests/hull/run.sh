#!/usr/bin/env bash
# Build and run the hull unit tests (src/hull.c, no GL, no game data).
#   tests/hull/run.sh            # plain build
#   SAN=1 tests/hull/run.sh      # with -fsanitize=address,undefined
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
out=${TMPDIR:-/tmp}/hull-test.$$
trap 'rm -f "$out"' EXIT
extra=
[ "${SAN:-}" = 1 ] && extra="-fsanitize=address,undefined -fno-sanitize-recover=undefined -g"
# shellcheck disable=SC2086
${CC:-cc} -Wall -Wno-unused -I"$top/src" $extra -o "$out" "$here/test_anim.c" -lm
"$out"
