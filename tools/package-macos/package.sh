#!/bin/bash
# Build the game and assemble dist/Frontier.app (Apple silicon).
#
#   tools/package-macos/package.sh [options]
#
#   --no-build            use the ./frontier and ./fe2.s.bin already built
#   --install-data [DIR]  also copy fe2.s.bin, sfx/ and music/ from this
#                         checkout into DIR (default: the app's data folder,
#                         ~/Library/Application Support/Frontier)
#   -h, --help            this text
#
# environment: JOBS (make -j, default 4), BUNDLE_ID (CFBundleIdentifier,
# default local.glfrontier.macos), DIST (output directory, default <repo>/dist)
#
# What goes in the bundle, and what does not
#   in:      the frontier binary, the Homebrew dylibs it needs (re-pointed so no
#            /opt/homebrew or /usr/local path remains), Info.plist, a generated
#            icon, a launcher script, cursor.png and joystick.ini (both are
#            tracked in this repository), and the SHA-256 of fe2.s.bin
#   NOT in:  fe2.s.bin, sfx/, music/, savs/. The launcher reads them from the
#            user's data folder (see launcher.sh).
#   NOTE:    the frontier binary itself is the translation of fe2.s to C, so the
#            app is a derivative of that file. Keep it on the machine that built
#            it; do not redistribute it.
#
# Nothing here touches src/, Makefile-C or any git-tracked file.

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(git -C "$HERE" rev-parse --show-toplevel)
DIST=${DIST:-"$REPO/dist"}
APP="$DIST/Frontier.app"
JOBS=${JOBS:-4}
BUNDLE_ID=${BUNDLE_ID:-local.glfrontier.macos}
DEFAULT_DATA_DIR="$HOME/Library/Application Support/Frontier"

DO_BUILD=1
INSTALL_DATA=0
INSTALL_DIR=$DEFAULT_DATA_DIR

die() { echo "package.sh: $*" >&2; exit 1; }
say() { echo "==> $*"; }

while [ $# -gt 0 ]; do
	case "$1" in
	--no-build) DO_BUILD=0 ;;
	--install-data)
		INSTALL_DATA=1
		if [ $# -gt 1 ] && [ "${2#-}" = "$2" ]; then
			INSTALL_DIR=$2
			shift
		fi
		;;
	-h | --help) sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) die "unknown option: $1 (try --help)" ;;
	esac
	shift
done

# --- preflight -------------------------------------------------------------------
[ "$(uname -s)" = Darwin ] || die "this packages a macOS app; run it on a Mac"
for t in git make otool install_name_tool codesign sips iconutil plutil shasum lipo python3; do
	command -v "$t" >/dev/null 2>&1 || die "missing tool: $t"
done

cd "$REPO"

if [ "$DO_BUILD" = 1 ]; then
	[ -f fe2.s ] || die "fe2.s is not in $REPO (it is not part of this repository; supply your own, see docs/04-build-macos.md section 3)"
	say "building the game: make -f Makefile-C -j$JOBS"
	make -f Makefile-C -j"$JOBS"
fi

[ -x frontier ] || die "no ./frontier; run without --no-build"
[ -f fe2.s.bin ] || die "no ./fe2.s.bin; run without --no-build"
archs=$(lipo -archs frontier)
[ "$archs" = arm64 ] || die "./frontier is not an arm64 binary: $archs"
deps=$(otool -L frontier)
case "$deps" in
*OpenGL.framework*) ;;
*) die "./frontier is not the OpenGL build; run: rm -f host.o src/*.o && make -f Makefile-C" ;;
esac

# --- helpers ---------------------------------------------------------------------
# rpaths of a Mach-O file, one per line
rpaths_of() { otool -l "$1" | awk '/cmd LC_RPATH/{r=1} r&&$1=="path"{print $2; r=0}'; }

# deployment target of a Mach-O file (empty for a non-Mach-O)
minos_of() {
	otool -l "$1" 2>/dev/null | awk '
		/cmd LC_BUILD_VERSION/ {b=1}
		b && $1=="minos" {print $2; exit}
		/cmd LC_VERSION_MIN_MACOSX/ {v=1}
		v && $1=="version" {print $2; exit}'
}

# resolve a load-command path to a real file. $1 = the path as written,
# $2 = the original (pre-copy) location of the file that names it.
resolve_dep() {
	local dep=$1 orig=$2 base cand rp
	case "$dep" in
	/*) [ -f "$dep" ] && { python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$dep"; return 0; } ;;
	@loader_path/*)
		cand="$(dirname "$orig")/${dep#@loader_path/}"
		[ -f "$cand" ] && { python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$cand"; return 0; } ;;
	@rpath/*)
		base=${dep#@rpath/}
		while IFS= read -r rp; do
			[ -n "$rp" ] || continue
			case "$rp" in
			@loader_path*) rp="$(dirname "$orig")${rp#@loader_path}" ;;
			@executable_path*) rp="$(dirname "$REPO/frontier")${rp#@executable_path}" ;;
			esac
			cand="$rp/$base"
			[ -f "$cand" ] && { python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$cand"; return 0; }
		done <<EOF
$(rpaths_of "$orig")
EOF
		;;
	esac
	return 1
}

# --- assemble the bundle -----------------------------------------------------------
say "assembling $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Frameworks"
FW="$APP/Contents/Frameworks"
MAIN="$APP/Contents/MacOS/frontier-bin"   # not "frontier": the launcher is "Frontier" and APFS ignores case

cp frontier "$MAIN"
chmod u+w "$MAIN"

# Bundled libraries. LIBS holds "bundled-name|original-real-path" lines; PENDING
# holds the ones whose own dependencies have not been walked yet. Nothing here
# runs in a command substitution, so the variables survive.
LIBS=""
PENDING=""
queue_lib() {  # $1 = file name in Frameworks, $2 = real source file
	local name=$1 src=$2
	[ -f "$FW/$name" ] && return 0
	cp "$src" "$FW/$name"
	chmod u+w "$FW/$name"
	LIBS="$LIBS$name|$src
"
	PENDING="$PENDING$name|$src
"
}
src_of() { printf '%s' "$LIBS" | awk -F'|' -v n="$1" '$1==n{print $2; exit}'; }

# Point every non-system dependency of one bundled file ($1, originally at $2)
# at @rpath/<name> and queue the library for bundling.
walk() {
	local f=$1 orig=$2 dep name real own
	own=$(otool -D "$f" 2>/dev/null | sed -n 2p)
	while IFS= read -r dep; do
		[ -n "$dep" ] || continue
		case "$dep" in /usr/lib/* | /System/*) continue ;; esac
		[ "$dep" = "$own" ] && continue
		name=$(basename "$dep")
		real=$(resolve_dep "$dep" "$orig") || die "cannot find $dep (needed by $orig)"
		queue_lib "$name" "$real"
		install_name_tool -change "$dep" "@rpath/$name" "$f" 2>/dev/null
	done <<EOF
$(otool -L "$f" | tail -n +2 | awk '{print $1}')
EOF
}

drain() {
	local item name src
	while [ -n "$PENDING" ]; do
		item=${PENDING%%$'\n'*}
		PENDING=${PENDING#*$'\n'}
		name=${item%%|*}
		src=${item#*|}
		walk "$FW/$name" "$src"
	done
}

walk "$MAIN" "$REPO/frontier"
drain

# sdl12-compat and sdl2-compat load the next SDL layer with dlopen at run time,
# so otool -L does not list it. Each tries @loader_path/<name> first, which is
# Frameworks/ here, and finds it on its own rpath in Homebrew:
#   libSDL-1.2.0.dylib  ->  libSDL2-2.0.0.dylib
#   libSDL2-2.0.0.dylib ->  libSDL3.dylib   (the Homebrew file is libSDL3.0.dylib)
runtime_layer() {  # $1 = bundled lib, $2 = file wanted on its rpath, $3 = name to bundle it as
	local lib=$1 want=$2 as=$3 src rp cand real
	[ -f "$FW/$lib" ] || return 0
	src=$(src_of "$lib")
	while IFS= read -r rp; do
		[ -n "$rp" ] || continue
		case "$rp" in @loader_path*) rp="$(dirname "$src")${rp#@loader_path}" ;; esac
		cand="$rp/$want"
		if [ -f "$cand" ]; then
			real=$(python3 -c 'import os,sys;print(os.path.realpath(sys.argv[1]))' "$cand")
			queue_lib "$as" "$real"
			drain
			return 0
		fi
	done <<EOF
$(rpaths_of "$src")
EOF
	die "$lib loads $want at run time, but it is not on its rpath"
}
runtime_layer libSDL-1.2.0.dylib libSDL2-2.0.0.dylib libSDL2-2.0.0.dylib
runtime_layer libSDL2-2.0.0.dylib libSDL3.0.dylib libSDL3.dylib

# Install names and rpaths. The binary finds Frameworks/ through
# @executable_path; each library through its own directory. Homebrew rpaths
# are removed so nothing can fall back to a path outside the bundle.
reset_rpaths() {  # $1 file, $2 the one rpath to keep
	local f=$1 rp
	while IFS= read -r rp; do
		[ -n "$rp" ] || continue
		install_name_tool -delete_rpath "$rp" "$f" 2>/dev/null
	done <<EOF
$(rpaths_of "$f")
EOF
	install_name_tool -add_rpath "$2" "$f" 2>/dev/null
}
reset_rpaths "$MAIN" "@executable_path/../Frameworks"
for lib in "$FW"/*.dylib; do
	install_name_tool -id "@rpath/$(basename "$lib")" "$lib" 2>/dev/null
	reset_rpaths "$lib" "@loader_path"
done

# --- resources ---------------------------------------------------------------------
cp "$HERE/launcher.sh" "$APP/Contents/MacOS/Frontier"
chmod 755 "$APP/Contents/MacOS/Frontier"

for f in cursor.png joystick.ini; do
	[ -f "$f" ] && cp "$f" "$APP/Contents/Resources/$f"
done
shasum -a 256 fe2.s.bin | cut -d' ' -f1 >"$APP/Contents/Resources/fe2.s.bin.sha256"

say "drawing the icon"
ICONTMP=$(mktemp -d "${TMPDIR:-/tmp}/frontier-icon.XXXXXX")
trap 'rm -rf "$ICONTMP"' EXIT
python3 "$HERE/make_icon.py" "$ICONTMP/icon1024.png" 1024
ICONSET="$ICONTMP/Frontier.iconset"
mkdir "$ICONSET"
for s in 16 32 128 256 512; do
	sips -z "$s" "$s" "$ICONTMP/icon1024.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
	d=$((s * 2))
	if [ "$d" = 1024 ]; then
		cp "$ICONTMP/icon1024.png" "$ICONSET/icon_${s}x${s}@2x.png"
	else
		sips -z "$d" "$d" "$ICONTMP/icon1024.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
	fi
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/Frontier.icns"

# --- Info.plist ----------------------------------------------------------------------
VERSION=$(git describe --tags --always --dirty 2>/dev/null || echo unknown)
BUILD=$(git rev-list --count HEAD 2>/dev/null || echo 1)

# LSMinimumSystemVersion = the highest deployment target of anything in the bundle
MINOS=""
maxver() {  # compare dotted versions, print the larger
	awk -v a="$1" -v b="$2" 'BEGIN{
		n=split(a,x,"."); m=split(b,y,".");
		for(i=1;i<=3;i++){ xv=(i<=n?x[i]:0)+0; yv=(i<=m?y[i]:0)+0;
			if(xv>yv){print a; exit} if(yv>xv){print b; exit} }
		print a }'
}
for f in "$MAIN" "$FW"/*.dylib; do
	v=$(minos_of "$f")
	[ -n "$v" ] || continue
	if [ -z "$MINOS" ]; then MINOS=$v; else MINOS=$(maxver "$MINOS" "$v"); fi
done
[ -n "$MINOS" ] || die "could not read the deployment target from the binary"

sed -e "s|@BUNDLE_ID@|$BUNDLE_ID|g" \
	-e "s|@VERSION@|$VERSION|g" \
	-e "s|@BUILD@|$BUILD|g" \
	-e "s|@MINOS@|$MINOS|g" \
	"$HERE/Info.plist.in" >"$APP/Contents/Info.plist"
plutil -lint "$APP/Contents/Info.plist" >/dev/null
printf 'APPL????' >"$APP/Contents/PkgInfo"

# --- check: no Homebrew paths left -------------------------------------------------------
say "checking for leftover Homebrew paths"
bad=0
for f in "$MAIN" "$FW"/*.dylib; do
	if otool -L "$f" | tail -n +2 | grep -E '/opt/homebrew|/usr/local' >/dev/null; then
		echo "still references a non-system path: $f" >&2
		otool -L "$f" | grep -E '/opt/homebrew|/usr/local' >&2
		bad=1
	fi
	if rpaths_of "$f" | grep -E '/opt/homebrew|/usr/local' >/dev/null; then
		echo "still has a Homebrew rpath: $f" >&2
		bad=1
	fi
done
[ "$bad" = 0 ] || die "Homebrew paths remain in the bundle"

# --- sign (ad hoc), inside out ----------------------------------------------------------------
say "signing ad hoc"
for lib in "$FW"/*.dylib; do codesign --force -s - "$lib" 2>/dev/null; done
codesign --force -s - "$MAIN" 2>/dev/null
codesign --force --deep -s - "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"

# --- optional: put the data where the launcher looks for it --------------------------------------------
if [ "$INSTALL_DATA" = 1 ]; then
	say "installing game data into $INSTALL_DIR"
	mkdir -p "$INSTALL_DIR/savs"
	cp fe2.s.bin "$INSTALL_DIR/fe2.s.bin"
	for d in sfx music; do
		if [ -d "$d" ]; then
			mkdir -p "$INSTALL_DIR/$d"
			cp -R "$d/." "$INSTALL_DIR/$d/"
		else
			echo "    (no $d/ in this checkout; skipped)"
		fi
	done
fi

say "done"
echo "    app:      $APP"
echo "    version:  $VERSION (build $BUILD), minimum macOS $MINOS, arm64"
echo "    bundled:  $(ls "$FW" | tr '\n' ' ')"
echo "    size:     $(du -sh "$APP" | cut -f1)"
echo "    data:     $DEFAULT_DATA_DIR  (fe2.s.bin required; sfx/ and music/ optional)"
