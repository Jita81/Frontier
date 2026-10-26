#!/usr/bin/env bash
# Real-time soak of one Frontier build (S-13), printing the six 04 §8.3 lines.
#
#   tools/soak/soak.sh [options] [-- game arguments]
#
# Options:
#   --binary PATH    build to soak (default ./frontier; san/ubsan/frontier or
#                    san/asan/frontier for item 5)
#   --duration SEC   wall-clock run length (default 1800; 04 §8.3 asks for >= 1800)
#   --pos X,Y        SDL_VIDEO_WINDOW_POS, so two soaks never share a spot (default 60,80)
#   --cwd DIR        directory the game runs in, holding fe2.s.bin (default: repo top)
#   --out DIR        run directory (default: a new $TMPDIR/fe2-soak.XXXXXX)
#   --tape FILE      replay an oracle tape as the scripted route; needs a build with
#                    --oracle-replay (src/oracle.c), else exit 2.  The game runs with
#                    --oracle-hold: when the tape ends it writes the trace's end and
#                    keeps running with no input until the SIGTERM of item 6.  A tape
#                    that ends after --duration gets a warning: item 6 would then stop
#                    the route early (exit 3).
#   --keep-frames    keep the window captures in DIR/frames (they show game screens)
#   --no-frames      skip the frame watchdog (item 3), e.g. with SDL_VIDEODRIVER=dummy
#   --no-leaks       skip the codesign and `leaks` step of item 4
#
# How each item is judged:
#   1 session  PASS only for --duration >= 1800 with a --tape route; the 30-minute
#              idle intro is a second run without --tape.  With a tape the line also
#              says route_done=yes|no: whether oracle.out holds an end line and no
#              invalid line (detail only, not a gate).
#   2 liveness kill -0 on the game's PID every 5 s.
#   3 watchdog window-only capture every 10 s (needs Screen Recording permission for
#              the terminal); FAIL if no frame changed for 60 s or nothing is captured.
#              A check with no window or a failed capture is not a change: it extends
#              the static stretch, so one good capture cannot carry a run of failures.
#              Each capture also records how much of the window other windows cover.
#              That is diagnostic only and never changes the verdict: a covered window
#              has not been shown to stop drawing, and an uncovered UBSan build has
#              been seen static for 100 s.
#              On a sanitizer build (UBSan or ASan) item 3 is INFO, not a gate: the
#              line still says what the 60 s rule would give (watchdog=PASS|FAIL),
#              but instrumented builds hold one picture for over 60 s and then move
#              on (tools/soak/README), so the rule cannot tell a freeze from slow
#              code there.  A plain build is the item-3 gate.
#   4 memory   RSS and phys_footprint every 10 s; the least-squares slope of each over
#              the last 20 min, skipping the first 60 s of start-up, must be < 1 MB/min.
#              (RSS alone is blind on macOS: compression shrank it 5 MB/min in a run
#              that leaked 4 MB, so the footprint slope gates too.)  `leaks <pid>` before SIGTERM
#              must report 0 blocks whose allocation stack touches the game binary
#              (conservative: an SDL leak reached from host code counts as host).
#              The leak half FAILs when leaks reports leaks but no allocation stacks
#              (host and library leaks cannot then be told apart), prints no leak
#              count, times out, or cannot run because codesign failed; it is SKIP
#              only with --no-leaks or on an ASan build.  On an ASan build item 4 is
#              INFO: the line still says slope=PASS|FAIL, but ASan's allocator moves
#              the slope by tens of MB/min with no leak (tools/soak/README).
#   5 sanitizer  UBSan build: 0 "runtime error" lines on stderr.  ASan build: 0
#              "ERROR: AddressSanitizer" reports.  Any other build: SKIP.
#              An ASan build runs with ASAN_OPTIONS=detect_leaks=0 unless set (LSan is
#              not supported on macOS arm64) and without `leaks` or MallocStackLogging,
#              which clash with ASan's allocator; item 4's leak half is then SKIP.
#              sdl12-compat cannot find libSDL2 from an ASan process unless the SDL2
#              libdir is on DYLD_FALLBACK_LIBRARY_PATH (it hangs before main), so the
#              script prepends `pkg-config --variable=libdir sdl2` there.
#   6 exit     SIGTERM to the PID gives exit 0 and stderr holds only the loader line
#              (MallocStackLogging's own notices, which this script causes, are ignored).
#              With a tape the SIGTERM must come after the tape's end: before it the
#              oracle marks the trace early-exit and exits 3.
#
# The game runs as a copy named fe2soak in the run directory, so it is never hit by
# anyone's `pkill frontier`, and only that PID is ever signalled.  The copy (game
# code) and, unless --keep-frames, the captures are deleted when the script exits.
# Exit status: 0 verdict PASS, 1 verdict FAIL, 2 usage or setup error.
set -u

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
binary=./frontier duration=1800 pos=60,80 cwd=$top out='' tape=''
keep_frames=0 frames=1 leaks_check=1

usage() { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; }
die() { echo "soak: $*" >&2; exit 2; }

while [ $# -gt 0 ]; do
	case $1 in
	--binary) [ $# -ge 2 ] || die "--binary needs a path"; binary=$2; shift 2 ;;
	--duration) [ $# -ge 2 ] || die "--duration needs seconds"; duration=$2; shift 2 ;;
	--pos) [ $# -ge 2 ] || die "--pos needs X,Y"; pos=$2; shift 2 ;;
	--cwd) [ $# -ge 2 ] || die "--cwd needs a directory"; cwd=$2; shift 2 ;;
	--out) [ $# -ge 2 ] || die "--out needs a directory"; out=$2; shift 2 ;;
	--tape) [ $# -ge 2 ] || die "--tape needs a file"; tape=$2; shift 2 ;;
	--keep-frames) keep_frames=1; shift ;;
	--no-frames) frames=0; shift ;;
	--no-leaks) leaks_check=0; shift ;;
	-h | --help) usage; exit 0 ;;
	--) shift; break ;;
	*) usage >&2; die "unknown option $1" ;;
	esac
done
game_args=("$@")

case $duration in '' | *[!0-9]*) die "--duration must be whole seconds" ;; esac
[ "$duration" -ge 30 ] || die "--duration must be at least 30"
case $pos in *[0-9],*[0-9]) ;; *) die "--pos must be X,Y" ;; esac
[ -x "$binary" ] || { [ -x "$top/$binary" ] && binary=$top/$binary; }
[ -x "$binary" ] || die "no executable build at $binary"
binary=$(cd "$(dirname "$binary")" && pwd)/$(basename "$binary")
[ -f "$cwd/fe2.s.bin" ] || echo "soak: warning: no fe2.s.bin in $cwd" >&2

if [ -n "$tape" ]; then
	[ -f "$tape" ] || die "no tape at $tape"
	tape=$(cd "$(dirname "$tape")" && pwd)/$(basename "$tape")
	# Static check: running the game with an unknown flag would open a window.
	grep -q -a -e '--oracle-replay' "$binary" ||
		die "$binary has no --oracle-replay; build from a tree with src/oracle.c"
	grep -q -a -e '--oracle-hold' "$binary" ||
		die "$binary has no --oracle-hold; build from a tree with it in src/oracle.c"
	# --pace replays at 50 VBLs a second
	tape_end=$(sed -n 's/^end \([0-9][0-9]*\)$/\1/p' "$tape" | head -n 1)
	[ -n "$tape_end" ] && [ $(((tape_end + 49) / 50)) -ge "$duration" ] &&
		echo "soak: warning: the tape ends at VBL $tape_end (about $(((tape_end + 49) / 50)) s), not before --duration $duration; item 6 will stop the route early" >&2
fi

if [ -z "$out" ]; then
	out=$(mktemp -d "${TMPDIR:-/tmp}/fe2-soak.XXXXXX") || die "cannot make a run directory"
else
	mkdir -p "$out" || die "cannot make $out"
fi
out=$(cd "$out" && pwd)
[ -n "$tape" ] && game_args+=(--oracle-replay "$tape" --oracle-out "$out/oracle.out" --pace --oracle-hold)

ubsan=0 asan=0
otool -L "$binary" 2>/dev/null | grep -q 'libclang_rt\.ubsan' && ubsan=1
otool -L "$binary" 2>/dev/null | grep -q 'libclang_rt\.asan' && asan=1
leaks_note='' leaks_failed=0
if [ "$asan" = 1 ] && [ "$leaks_check" = 1 ]; then
	leaks_check=0 leaks_note="ASan build: leaks and MallocStackLogging clash with ASan's allocator"
fi

pid=
# shellcheck disable=SC2329  # run by the EXIT trap
cleanup() {
	if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
		kill -TERM "$pid" 2>/dev/null
		sleep 2
		kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null
	fi
	rm -f "$out/fe2soak"
	[ "$keep_frames" = 1 ] || rm -rf "$out/frames"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

cp "$binary" "$out/fe2soak" || die "cannot copy $binary"
if [ "$leaks_check" = 1 ]; then
	# leaks can only read a process whose binary carries get-task-allow.
	cat >"$out/get-task-allow.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.get-task-allow</key><true/></dict></plist>
PLIST
	if ! codesign -s - -f --entitlements "$out/get-task-allow.plist" "$out/fe2soak" \
		>"$out/codesign.log" 2>&1; then
		leaks_check=0 leaks_failed=1 leaks_note="codesign failed, see codesign.log"
	fi
fi
if [ "$frames" = 1 ]; then
	if ! command -v swiftc >/dev/null 2>&1; then
		frames=0 frames_note="no swiftc"
	elif ! swiftc -O "$here/winid.swift" -o "$out/winid" >"$out/winid-build.log" 2>&1; then
		frames=0 frames_note="winid.swift did not build, see winid-build.log"
	fi
fi
mkdir -p "$out/frames"
: >"$out/rss.tsv"
: >"$out/footprint.tsv"
: >"$out/frames.tsv"

envs=(SDL_VIDEO_WINDOW_POS="$pos" SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}")
[ "$leaks_check" = 1 ] && envs+=(MallocStackLogging=lite)
[ "$ubsan" = 1 ] && envs+=(UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1}")
if [ "$asan" = 1 ]; then
	sdl2_lib=$(pkg-config --variable=libdir sdl2 2>/dev/null)
	[ -n "$sdl2_lib" ] || sdl2_lib=/opt/homebrew/lib
	envs+=(ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}")
	envs+=(DYLD_FALLBACK_LIBRARY_PATH="$sdl2_lib:${DYLD_FALLBACK_LIBRARY_PATH:-$HOME/lib:/usr/local/lib:/usr/lib}")
fi

echo "soak: run dir $out" >&2
echo "soak: $binary for ${duration}s at $pos (ubsan=$ubsan asan=$asan frames=$frames leaks=$leaks_check)" >&2
cd "$cwd" || die "cannot cd to $cwd"
env "${envs[@]}" "$out/fe2soak" ${game_args[@]+"${game_args[@]}"} \
	>"$out/stdout.log" 2>"$out/stderr.log" &
pid=$!
start=$(date +%s)
end=$((start + duration))
echo "soak: pid $pid" >&2

checks=0 died_at='' last_rss=0 last_frame=0
captures=0 cap_fail=0 nowin=0 distinct=0 prev_md5='' last_change=0 max_static=0
covered=0 static_covered=0 run_covered=0

stale() { # $1 = seconds since start; no new frame at this check
	local static=$(($1 - last_change))
	if [ "$static" -gt "$max_static" ]; then max_static=$static static_covered=$run_covered; fi
}

capture() { # $1 = seconds since start
	local wid f md5 pct
	read -r wid pct <<<"$("$out/winid" "$pid" 2>/dev/null)"
	if [ -z "$wid" ]; then
		nowin=$((nowin + 1)); printf '%s\tnowindow\n' "$1" >>"$out/frames.tsv"; stale "$1"; return
	fi
	f=$out/frames/$1.png
	if ! screencapture -x -o -l "$wid" "$f" 2>/dev/null || [ ! -s "$f" ]; then
		cap_fail=$((cap_fail + 1)); printf '%s\tfailed\n' "$1" >>"$out/frames.tsv"; stale "$1"; return
	fi
	md5=$(md5 -q "$f")
	captures=$((captures + 1))
	[ "${pct:-0}" -ge 50 ] && covered=$((covered + 1))
	if [ "$md5" != "$prev_md5" ]; then
		distinct=$((distinct + 1)) prev_md5=$md5 last_change=$1 run_covered=0
	else
		[ "${pct:-0}" -ge 50 ] && run_covered=1
		stale "$1"
	fi
	printf '%s\t%s\t%s%%\n' "$1" "$md5" "${pct:-?}" >>"$out/frames.tsv"
	[ "$keep_frames" = 1 ] || rm -f "$f"
}

while :; do
	sleep 5
	now=$(date +%s) t=$((now - start))
	checks=$((checks + 1))
	if ! kill -0 "$pid" 2>/dev/null; then died_at=$t; break; fi
	if [ $((now - last_rss)) -ge 10 ]; then
		rss=$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ')
		[ -n "$rss" ] && printf '%s\t%s\n' "$t" "$rss" >>"$out/rss.tsv"
		fp=$(footprint -f bytes "$pid" 2>/dev/null | sed -n 's/^ *phys_footprint: \([0-9]*\) B$/\1/p')
		[ -n "$fp" ] && printf '%s\t%s\n' "$t" "$((fp / 1024))" >>"$out/footprint.tsv"
		last_rss=$now
	fi
	if [ "$frames" = 1 ] && [ $((now - last_frame)) -ge 10 ]; then
		capture "$t"; last_frame=$now
	fi
	[ "$now" -ge "$end" ] && break
done
ran=$(($(date +%s) - start))

# Item 4: leaks, while the process is still alive, before SIGTERM.
leaks_total='' leaks_host='' leaks_stacks=0
if [ -z "$died_at" ] && [ "$leaks_check" = 1 ]; then
	leaks "$pid" >"$out/leaks.txt" 2>&1 &
	lpid=$!
	i=0
	while kill -0 "$lpid" 2>/dev/null && [ $i -lt 240 ]; do sleep 1; i=$((i + 1)); done
	if kill -0 "$lpid" 2>/dev/null; then
		kill -TERM "$lpid" 2>/dev/null; wait "$lpid" 2>/dev/null
		leaks_note="leaks timed out"
	else
		wait "$lpid"
		leaks_total=$(sed -n 's/^Process [0-9]*: \([0-9]*\) leaks\{0,1\} for .*/\1/p' "$out/leaks.txt" | head -1)
		[ -n "$leaks_total" ] || leaks_note="no leak count in leaks.txt"
		# shellcheck disable=SC2046  # split the two fields
		set -- $(awk '
			/^STACK OF [0-9]+ INSTANCES? OF/ { n = $3; host = 0; inside = 1; stacks++; next }
			inside && $2 == "fe2soak" { host = 1 }
			inside && /^====/ { if (host) sum += n; inside = 0 }
			END { print sum + 0, stacks + 0 }' "$out/leaks.txt")
		leaks_host=$1 leaks_stacks=$2
		if [ -n "$leaks_total" ] && [ "$leaks_total" -gt 0 ] && [ "$leaks_stacks" -eq 0 ]; then
			leaks_note="$leaks_total leaks but no allocation stacks, so host and game leaks cannot be told apart"
		fi
	fi
fi

# Item 6: SIGTERM to our PID only, then the exit status.
status=
if [ -z "$died_at" ]; then
	kill -TERM "$pid" 2>/dev/null
	i=0
	while kill -0 "$pid" 2>/dev/null && [ $i -lt 30 ]; do sleep 0.5; i=$((i + 1)); done
	if kill -0 "$pid" 2>/dev/null; then
		kill -KILL "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; status="hung"
	fi
fi
if [ -z "$status" ]; then wait "$pid"; status=$?; fi
pid=

fails=
result() { # item verdict details...
	local item=$1 verdict=$2; shift 2
	echo "soak item=$item $* $verdict"
	[ "$verdict" = FAIL ] && fails="${fails:+$fails,}$item"
}

route=none; [ -n "$tape" ] && route=tape
if [ -n "$tape" ]; then
	route_done=no
	grep -q '^end ' "$out/oracle.out" 2>/dev/null && ! grep -q '^invalid' "$out/oracle.out" &&
		route_done=yes
	route="$route route_done=$route_done"
fi
if [ "$duration" -ge 1800 ] && [ -n "$tape" ] && [ -z "$died_at" ]; then v=PASS; else v=FAIL; fi
result 1 $v "session_s=$ran route=$route needs=1800s+tape"

if [ -z "$died_at" ]; then
	result 2 PASS "checks=$checks every=5s"
else
	result 2 FAIL "checks=$checks died_at=${died_at}s"
fi

if [ "$frames" = 0 ]; then
	result 3 SKIP "reason=\"${frames_note:-disabled by --no-frames}\""
else
	note=''
	[ "$max_static" -ge 60 ] && [ "$static_covered" = 1 ] &&
		note=" note=\"the window was >= 50% covered during the longest static stretch (diagnostic only)\""
	[ "$captures" -gt 1 ] && [ "$distinct" -le 1 ] &&
		note=" note=\"every frame identical: frozen, or a blank capture (Screen Recording permission?)\""
	if [ "$captures" -eq 0 ]; then v=FAIL; elif [ "$max_static" -ge 60 ]; then v=FAIL; else v=PASS; fi
	fields="captures=$captures distinct=$distinct max_static_s=$max_static covered_captures=$covered no_window=$nowin failed=$cap_fail$note"
	if [ "$ubsan" = 1 ] || [ "$asan" = 1 ]; then
		result 3 INFO "$fields watchdog=$v why=\"sanitizer build: instrumented code holds one picture for over 60 s and then moves on, so the 60 s rule gates plain builds only\""
	else
		result 3 $v "$fields"
	fi
fi

slope_of() { # FILE of "seconds<TAB>KB" -> "MB_per_min window_s samples", or "na ..."
	awk -F'\t' '
	{ t[NR] = $1; r[NR] = $2; last = $1 }
	END {
		from = last - 1200; if (from < 60) from = 60
		for (i = 1; i <= NR; i++) if (t[i] >= from) { n++; sx += t[i]; sy += r[i]; sxx += t[i]*t[i]; sxy += t[i]*r[i] }
		if (n < 3 || n*sxx - sx*sx == 0) { print "na 0 " n + 0; exit }
		b = (n*sxy - sx*sy) / (n*sxx - sx*sx)          # KB per second
		printf "%.3f %d %d\n", b * 60 / 1024, last - from, n
	}' "$1"
}
under_1mb() { [ "$1" != na ] && awk -v s="$1" 'BEGIN { exit !(s < 1.0) }'; }
# shellcheck disable=SC2046  # split the three fields
set -- $(slope_of "$out/rss.tsv")
rss_mb=$1 window=$2 samples=$3
# shellcheck disable=SC2046
set -- $(slope_of "$out/footprint.tsv")
fp_mb=$1
slope_v=PASS
under_1mb "$rss_mb" || slope_v=FAIL
under_1mb "$fp_mb" || slope_v=FAIL
# The leak half is SKIP only when it was not wanted (--no-leaks) or cannot apply
# (ASan).  Wanted but not run (codesign failed, leaks timed out), no leak count,
# or leaks reported without allocation stacks is FAIL: a report with no stacks
# cannot show that no leak is the game's.
if [ "$leaks_failed" = 1 ]; then
	leaks_v=FAIL
elif [ "$leaks_check" = 0 ]; then
	leaks_v=SKIP
elif [ -z "$leaks_total" ]; then
	leaks_v=FAIL
elif [ "$leaks_total" -gt 0 ] && [ "$leaks_stacks" -eq 0 ]; then
	leaks_v=FAIL
elif [ "${leaks_host:-0}" -eq 0 ]; then
	leaks_v=PASS
else
	leaks_v=FAIL
fi
v=PASS; [ "$slope_v" = FAIL ] && v=FAIL; [ "$leaks_v" = FAIL ] && v=FAIL
why=
# ASan's own allocator (quarantine, shadow memory) moves both slopes by tens of
# MB/min with no game leak (tools/soak/README), and the leak half is SKIP there,
# so on ASan item 4 says what the slope rule would give but does not gate.
if [ "$asan" = 1 ]; then
	v=INFO
	why=' why="ASan build: allocator churn swamps the 1 MB/min rule; plain and UBSan builds gate item 4"'
fi
result 4 $v "rss_slope_mb_per_min=$rss_mb footprint_slope_mb_per_min=$fp_mb window_s=$window samples=$samples slope=$slope_v" \
	"leaks_total=${leaks_total:-na} leaks_host=${leaks_host:-na} leaks=$leaks_v${leaks_note:+ note=\"$leaks_note\"}$why"

if [ "$ubsan" = 1 ] || [ "$asan" = 1 ]; then
	n=0 a=0
	[ "$ubsan" = 1 ] && n=$(grep -c 'runtime error' "$out/stderr.log")
	[ "$asan" = 1 ] && a=$(grep -c 'ERROR: AddressSanitizer' "$out/stderr.log")
	if [ "$n" -eq 0 ] && [ "$a" -eq 0 ]; then v=PASS; else v=FAIL; fi
	result 5 $v "ubsan=$ubsan runtime_errors=$n asan=$asan asan_errors=$a"
else
	result 5 SKIP "reason=\"not a sanitizer build\""
fi

loader='^fe2\.s\.bin: 0x[0-9a-f]+ bytes \(code end 0x[0-9a-f]+\), [0-9]+ fixups; loaded at 0x[0-9a-f]+\.$'
grep -E -v -e "$loader" -e '^fe2soak\([0-9]+\) MallocStackLogging: ' "$out/stderr.log" >"$out/stderr-unexpected.log"
extra=$(wc -l <"$out/stderr-unexpected.log" | tr -d ' ')
loader_n=$(grep -E -c -e "$loader" "$out/stderr.log")
if [ "$status" = 0 ] && [ "$extra" -eq 0 ] && [ "$loader_n" -eq 1 ]; then v=PASS; else v=FAIL; fi
first=$(head -1 "$out/stderr-unexpected.log" | cut -c1-120)
result 6 $v "exit=$status loader_lines=$loader_n other_stderr_lines=$extra${first:+ first=\"$first\"}"

if [ -z "$fails" ]; then
	echo "soak verdict=PASS"; exit 0
fi
echo "soak verdict=FAIL failed=$fails"
exit 1
