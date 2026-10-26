#!/usr/bin/env bash
# Tests for tools/soak/soak.sh that need no game and no screen.
#
# A stand-in "game" (a script that prints the loader line and exits 0 on
# SIGTERM) is soaked with tools stubbed on PATH.  The cases run in parallel
# and take about 100 s in all.
#
# Item 3 (watchdog), 75 s, swiftc and screencapture stubbed:
#   changing       every capture differs                    -> item 3 PASS
#   once           one capture, then every one fails        -> item 3 FAIL
#   nowindow       one capture, then no window found        -> item 3 FAIL
#   san-once       as once, on a build otool shows linking   -> item 3 INFO,
#                  the UBSan runtime                            watchdog=FAIL
# Item 4, leak half, 30 s with --no-frames, codesign and leaks stubbed:
#   leaks-zero     "0 leaks"                                -> leaks=PASS
#   leaks-lib      leaks whose stacks have no game frame    -> leaks=PASS
#   leaks-host     a leak whose stack has a game frame      -> leaks=FAIL
#   leaks-nostack  leaks counted but no allocation stacks   -> leaks=FAIL
#   leaks-nocount  no "Process N: K leaks" line             -> leaks=FAIL
#   codesign-fail  codesign fails, so leaks cannot run      -> leaks=FAIL
# Item 4, slope half, 100 s with --no-frames --no-leaks:
#   steady         a stand-in whose memory does not grow    -> slope=PASS
#   grow           a stand-in that grows about 6 MB/min     -> slope=FAIL
#   asan-grow      as grow, on a build otool shows linking   -> item 4 INFO,
#                  the ASan runtime                             slope=FAIL
# Exit 0 when every verdict is as expected.
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
# Touches 100 KB of new memory a second.
cat >"$work/grow" <<'G'
#!/usr/bin/perl
$SIG{TERM} = sub { exit 0 };
print STDERR "fe2.s.bin: 0x1 bytes (code end 0x1), 0 fixups; loaded at 0x0.\n";
my @a;
while (1) { push @a, "x" x 102400; select(undef, undef, undef, 1); }
G
chmod +x "$work/game" "$work/grow"

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
case $SOAK_TEST_MODE in once | san-once) [ "$n" -ge 1 ] && exit 1 ;; esac
echo "frame $n" >"$f"
S
# otool stub: in san-* modes the "game" links the UBSan runtime, in asan-*
# modes the ASan runtime.
cat >"$work/bin/otool" <<'S'
#!/bin/sh
case $SOAK_TEST_MODE in
san-*) echo "game:"; echo "	@rpath/libclang_rt.ubsan_osx_dynamic.dylib (compatibility version 0.0.0)" ;;
asan-*) echo "game:"; echo "	@rpath/libclang_rt.asan_osx_dynamic.dylib (compatibility version 0.0.0)" ;;
*) exit 1 ;;
esac
S
# codesign stub: fails in codesign-fail mode, else does nothing.
cat >"$work/bin/codesign" <<'S'
#!/bin/sh
[ "$SOAK_TEST_MODE" = codesign-fail ] && { echo "stub: codesign failed" >&2; exit 1; }
exit 0
S
# leaks stub: a report in the format of macOS leaks(1) with MallocStackLogging.
cat >"$work/bin/leaks" <<'S'
#!/bin/sh
p=$1
echo "Process:         fe2soak [$p]"
echo
case $SOAK_TEST_MODE in
leaks-zero)
	echo "Process $p: 100 nodes malloced for 10 KB"
	echo "Process $p: 0 leaks for 0 total leaked bytes."
	exit 0 ;;
leaks-nocount)
	echo "leaks: cannot examine process $p"
	exit 1 ;;
esac
echo "Process $p: 100 nodes malloced for 10 KB"
echo "Process $p: 3 leaks for 96 total leaked bytes."
echo
case $SOAK_TEST_MODE in
leaks-nostack)
	echo "    3 (96 bytes) ROOT LEAK: 0x600000c00000 [32]" ;;
leaks-lib | leaks-host)
	echo "STACK OF 3 INSTANCES OF 'ROOT LEAK: <malloc in helper>':"
	echo "4   dyld                                  0x18eab44e4 start + 6992"
	[ "$SOAK_TEST_MODE" = leaks-host ] &&
		echo "3   fe2soak                               0x102f914d4 main + 932"
	echo "2   libSDL2-2.0.0.dylib                   0x103000000 helper + 12"
	echo "1   libsystem_malloc.dylib                0x18e900000 malloc + 40"
	echo "===="
	echo "    3 (96 bytes) << TOTAL >>" ;;
esac
exit 1
S
chmod +x "$work/bin/"*

run() { # mode game duration options... -> the soak's report in $work/case.mode/report
	local mode=$1 game=$2 dur=$3; shift 3
	mkdir -p "$work/case.$mode"
	SOAK_TEST_MODE=$mode SOAK_TEST_DIR=$work/case.$mode PATH="$work/bin:$PATH" \
		"$top/tools/soak/soak.sh" --binary "$work/$game" --duration "$dur" "$@" \
		--cwd "$work/cwd" --out "$work/case.$mode/out" >"$work/case.$mode/report" 2>"$work/case.$mode/log"
}
# case: mode item expected game duration options; expected is token=VALUE[,...]
# where the token "verdict" is the item's verdict, the line's last word.
cases='changing 3 verdict=PASS game 75 --no-leaks
once 3 verdict=FAIL game 75 --no-leaks
nowindow 3 verdict=FAIL game 75 --no-leaks
san-once 3 verdict=INFO,watchdog=FAIL game 75 --no-leaks
leaks-zero 4 leaks=PASS game 30 --no-frames
leaks-lib 4 leaks=PASS game 30 --no-frames
leaks-host 4 leaks=FAIL game 30 --no-frames
leaks-nostack 4 leaks=FAIL game 30 --no-frames
leaks-nocount 4 leaks=FAIL game 30 --no-frames
codesign-fail 4 leaks=FAIL game 30 --no-frames
steady 4 slope=PASS game 100 --no-frames --no-leaks
grow 4 slope=FAIL grow 100 --no-frames --no-leaks
asan-grow 4 verdict=INFO,slope=FAIL grow 100 --no-frames --no-leaks'

while read -r mode item want game dur opts; do
	# shellcheck disable=SC2086  # split the options
	run "$mode" "$game" "$dur" $opts &
done <<<"$cases"
wait
fails=0 n=0
while read -r mode item want game dur opts; do
	n=$((n + 1))
	line=$(grep "^soak item=$item " "$work/case.$mode/report")
	bad=
	for kv in ${want//,/ }; do
		token=${kv%%=*} value=${kv#*=}
		if [ "$token" = verdict ]; then got=${line##* }
		else got=$(printf '%s\n' "$line" | sed -n "s/.* $token=\([A-Z]*\).*/\1/p"); fi
		[ "$got" = "$value" ] || bad="$bad $token=$got (want $value)"
	done
	if [ -z "$bad" ]; then echo "ok   $mode: $want: $line"
	else echo "FAIL $mode:$bad: $line"; fails=$((fails + 1)); fi
done <<<"$cases"
echo "soak test: $n cases, $fails failures"
[ "$fails" -eq 0 ]
