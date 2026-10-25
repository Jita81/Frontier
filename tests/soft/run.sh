#!/usr/bin/env bash
# Build and run the software renderer video-mode test (S-16) under ASan and UBSan.
# Needs SDL 1.2 and libpng headers (sdl12-compat, libpng); links neither.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
out=${TMPDIR:-/tmp}/soft-vidmode-test.$$
trap 'rm -rf "$out" "$out.dSYM"' EXIT
sdl=$(pkg-config --cflags sdl12_compat 2>/dev/null || sdl-config --cflags)
png=$(pkg-config --cflags libpng 2>/dev/null || true)
# shellcheck disable=SC2086
${CC:-cc} -Wall -Wno-unused -Wno-pointer-sign -Werror=incompatible-pointer-types \
  $sdl $png -DNO_SDL_IMAGE -I"$top/src" -I"$top" -g \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -o "$out" "$here/test_vidmode.c"
"$out"
