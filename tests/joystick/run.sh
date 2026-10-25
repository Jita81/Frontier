#!/usr/bin/env bash
# Build and run the joystick.ini bounds test (S-17) under ASan and UBSan.
# Needs SDL 1.2 headers (sdl12-compat); does not link SDL.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
out=${TMPDIR:-/tmp}/joystick-test.$$
trap 'rm -f "$out"' EXIT
sdl=$(pkg-config --cflags sdl12_compat 2>/dev/null || sdl-config --cflags)
# shellcheck disable=SC2086
${CC:-cc} -Wall -Wno-unused -Wno-pointer-sign $sdl -I"$top/src" -g \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined \
  -o "$out" "$here/test_joystick.c"
"$out"
