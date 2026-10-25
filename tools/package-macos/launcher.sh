#!/bin/sh
# Frontier.app launcher: Contents/MacOS/Frontier
#
# The game opens every file relative to the current directory (fe2.s.bin,
# sfx/, music/, savs/, joystick.ini; the save folder is a fixed relative path
# inside the game; see src/hostcall.c),
# and a Finder launch starts with the current directory set to "/". This script
# runs the real binary from the user's data folder instead, so no change to the
# game's source is needed.
#
#   data folder   ~/Library/Application Support/Frontier
#   override      --data-dir DIR   (or --data-dir=DIR), or FRONTIER_DATA_DIR=DIR
#   other args    passed to the game unchanged (--old-renderer, --size w h, ...)
#
# If the data is missing or wrong, a dialog says what to copy where and the
# app exits with status 0.

HERE=$(cd "$(dirname "$0")" && pwd) || exit 1
RES="$HERE/../Resources"
BIN="$HERE/frontier-bin"

DATA_DIR=${FRONTIER_DATA_DIR:-"$HOME/Library/Application Support/Frontier"}

usage() {
	cat <<'EOF'
Frontier.app launcher options (everything else goes to the game):
  --data-dir DIR   use DIR instead of ~/Library/Application Support/Frontier
                   (also settable with the FRONTIER_DATA_DIR environment variable)
EOF
}

# --- arguments: take --data-dir, drop Finder's -psn_* token, keep the rest ----
want_help=0
n=$#
i=0
while [ "$i" -lt "$n" ]; do
	a=$1
	shift
	i=$((i + 1))
	case "$a" in
	--data-dir)
		if [ "$i" -ge "$n" ]; then
			echo "Frontier: --data-dir needs a folder" >&2
			exit 2
		fi
		DATA_DIR=$1
		shift
		i=$((i + 1))
		;;
	--data-dir=*) DATA_DIR=${a#--data-dir=} ;;
	-psn_*) ;;
	--help | -h)
		want_help=1
		set -- "$@" "$a"
		;;
	*) set -- "$@" "$a" ;;
	esac
done

if [ "$want_help" = 1 ]; then
	usage
	exec "$BIN" "$@"
fi

# --- dialog --------------------------------------------------------------------
# show_dialog TITLE MESSAGE. Text goes to AppleScript as arguments, never as
# script source, so odd characters in a path cannot break it.
# FRONTIER_DIALOG_TIMEOUT=N (seconds) dismisses the dialog by itself; it exists
# for automated tests and is unset in normal use.
show_dialog() {
	title=$1
	message=$2
	printf 'Frontier: %s\n%s\n' "$title" "$message" >&2
	[ -x /usr/bin/osascript ] || return 0
	giveup=""
	case "${FRONTIER_DIALOG_TIMEOUT:-}" in
	"" | *[!0-9]*) ;;
	*) giveup=" giving up after $FRONTIER_DIALOG_TIMEOUT" ;;
	esac
	script='on run argv
	tell current application to activate
	display dialog (item 2 of argv) with title (item 1 of argv) buttons {"Quit", "Open Data Folder"} default button "Open Data Folder" with icon caution'"$giveup"'
end run'
	out=$(/usr/bin/osascript -e "$script" "$title" "$message" 2>/dev/null) || out=""
	case "$out" in
	*"button returned:Open Data Folder"*) /usr/bin/open "$DATA_DIR" ;;
	esac
	return 0
}

mkdir -p "$DATA_DIR" 2>/dev/null || {
	show_dialog "Cannot create the data folder" "Frontier could not create:

$DATA_DIR

Check the folder's permissions, or start the app with --data-dir pointing at a folder you can write to."
	exit 0
}

# --- data check ------------------------------------------------------------------
# fe2.s.bin is the only file the game cannot start without. sfx/ and music/ are
# optional: without them the game runs silent.
if [ ! -f "$DATA_DIR/fe2.s.bin" ]; then
	show_dialog "Game data needed" "Frontier does not include the game's data. Copy these into the data folder, then open Frontier again:

  fe2.s.bin   (required)
  sfx/        (sound effects; optional, the game is silent without it)
  music/      (music tracks; optional)

Data folder:
$DATA_DIR

fe2.s.bin is made from your own copy of fe2.s when the app is built. On the build machine, tools/package-macos/package.sh --install-data copies it (and sfx/, music/) here for you."
	exit 0
fi

# The compiled game code and fe2.s.bin are a matched pair. Check the file is the
# one this build was made with; a mismatch aborts inside the game with no message.
if [ -f "$RES/fe2.s.bin.sha256" ]; then
	want=$(cut -d' ' -f1 "$RES/fe2.s.bin.sha256")
	have=$(/usr/bin/shasum -a 256 "$DATA_DIR/fe2.s.bin" | cut -d' ' -f1)
	if [ "$want" != "$have" ]; then
		show_dialog "Wrong game data" "The fe2.s.bin in the data folder is not the one this copy of Frontier was built with, and the game cannot run with it.

Data folder:
$DATA_DIR

Replace it with the fe2.s.bin from the machine that built this app (package.sh --install-data does this), then open Frontier again."
		exit 0
	fi
fi

# savs/ is where fe2.s saves games (it hard-codes "./savs"). The two small
# non-game files below are copied only when absent, so edits are kept.
mkdir -p "$DATA_DIR/savs" 2>/dev/null
for f in cursor.png joystick.ini; do
	[ -f "$DATA_DIR/$f" ] || [ ! -f "$RES/$f" ] || cp "$RES/$f" "$DATA_DIR/$f" 2>/dev/null
done

cd "$DATA_DIR" || exit 1

# A Finder launch has no console (stderr is /dev/null), so keep the game's
# stderr in a log. When stderr goes somewhere, leave it alone.
if [ /dev/fd/2 -ef /dev/null ]; then
	LOGDIR="$HOME/Library/Logs/Frontier"
	if mkdir -p "$LOGDIR" 2>/dev/null; then
		{ printf '--- %s launch\n' "$(date '+%Y-%m-%d %H:%M:%S')"; } >>"$LOGDIR/frontier.log" 2>/dev/null
		exec "$BIN" "$@" 2>>"$LOGDIR/frontier.log"
	fi
fi
exec "$BIN" "$@"
