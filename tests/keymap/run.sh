#!/usr/bin/env bash
# Build and run the keymap unit test (S-3, docs/07 I-10).  macOS only.
#   tests/keymap/run.sh            # plain build
#   SAN=1 tests/keymap/run.sh      # with -fsanitize=address,undefined
# Needs SDL 1.2 headers (sdl12-compat); does not link SDL.
# Known limit, by design (docs/05 S-3): kVK 0 maps to A only when the symbol
# is SDLK_a, so on AZERTY the key in the A position (symbol q) is dropped.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
out=${TMPDIR:-/tmp}/keymap-test.$$
trap 'rm -f "$out"' EXIT
sdl=$(pkg-config --cflags sdl12_compat 2>/dev/null || sdl-config --cflags)
extra=
[ "${SAN:-}" = 1 ] && extra="-fsanitize=address,undefined -fno-sanitize-recover=undefined -g"
# shellcheck disable=SC2086
${CC:-cc} -Wall -Wno-unused -Wno-pointer-sign $sdl -I"$top/src" $extra \
  -o "$out" "$here/test_keymap.c"
"$out"
