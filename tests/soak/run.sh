#!/usr/bin/env bash
# Item-3 watchdog test for tools/soak/soak.sh, with no game and no screen.
#
# A stand-in "game" (a shell script that prints the loader line and waits for
# SIGTERM) is soaked for 75 s with swiftc and screencapture stubbed on PATH:
#   changing  every capture differs              -> item 3 PASS
#   once      one capture, then every one fails  -> item 3 FAIL (was a false PASS)
#   nowindow  one capture, then no window found  -> item 3 FAIL
# Exit 0 when all three verdicts are as expected.
set -u
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/soak-test.XXXXXX") || exit 2
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin" "$work/cwd"

cat >"$work/game" <<'G'
#!/bin/sh
echo 'fe2.s.bin: 0x1 bytes (code end 0x1), 0 fixups; loaded at 0x0.' >&2
trap 'exit 0' TERM
while :; do sleep 1; done
G
chmod +x "$work/game"

# swiftc stub: "builds" a winid that reports window 1, or none after the
# first call in nowindow mode.
cat >"$work/bin/swiftc" <<'S'
#!/bin/sh
while [ $# -gt 0 ]; do [ "$1" = -o ] && o=$2; shift; done
cat >"$o" <<'W'
#!/bin/sh
c=$SOAK_TEST_DIR/winid.count; n=$(cat "$c" 2>/dev/null || echo 0); echo $((n + 1)) >"$c"
[ "$SOAK_TEST_MODE" = nowindow ] && [ "$n" -ge 1 ] && exit 0
echo "1 0"
W
chmod +x "$o"
S
# screencapture stub: the last argument is the file.
cat >"$work/bin/screencapture" <<'S'
#!/bin/sh
for f; do :; done
c=$SOAK_TEST_DIR/cap.count; n=$(cat "$c" 2>/dev/null || echo 0); echo $((n + 1)) >"$c"
[ "$SOAK_TEST_MODE" = once ] && [ "$n" -ge 1 ] && exit 1
echo "frame $n" >"$f"
S
chmod +x "$work/bin/swiftc" "$work/bin/screencapture"

run() { # mode -> item 3 verdict
	mkdir -p "$work/$1"
	SOAK_TEST_MODE=$1 SOAK_TEST_DIR=$work/$1 PATH="$work/bin:$PATH" \
		"$top/tools/soak/soak.sh" --binary "$work/game" --duration 75 --no-leaks \
		--cwd "$work/cwd" --out "$work/$1/out" >"$work/$1/report" 2>"$work/$1/log"
	sed -n 's/^soak item=3 .* \([A-Z]*\)$/\1/p' "$work/$1/report"
}
fails=0
for case in changing:PASS once:FAIL nowindow:FAIL; do
	mode=${case%%:*} want=${case#*:}
	run "$mode" >"$work/$mode.verdict" &
done
wait
for case in changing:PASS once:FAIL nowindow:FAIL; do
	mode=${case%%:*} want=${case#*:}
	got=$(cat "$work/$mode.verdict")
	line=$(grep '^soak item=3 ' "$work/$mode/report")
	if [ "$got" = "$want" ]; then echo "ok   $mode: $line"
	else echo "FAIL $mode: want $want, $line"; fails=$((fails + 1)); fi
done
echo "soak item-3 test: 3 cases, $fails failures"
[ "$fails" -eq 0 ]
