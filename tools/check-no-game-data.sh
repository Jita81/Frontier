#!/usr/bin/env bash
# check-no-game-data.sh -- refuse to let Frontier Developments game data into git.
#
# The 68000 source (fe2.s), everything derived from it (fe2.s.c, fe2.s.bin,
# fixups.h, gen/, the frontier binary) and the game assets (sfx, music, savs)
# are copyright Frontier Developments.  They must stay on the local machine.
# This script fails (exit 1) if any of them is tracked or staged.
#
# Usage:
#   tools/check-no-game-data.sh            # every path in the index (CI: the checked-out tree)
#   tools/check-no-game-data.sh --staged   # only paths added/changed in the index (pre-commit hook)
#   tools/check-no-game-data.sh --rev REV  # every path in commit REV (e.g. a PR head)
#
# CI-ready invocation (any runner with git + bash + shasum):
#   bash tools/check-no-game-data.sh --rev HEAD
#
# What counts as game data (docs/07 T-0.13 and section 10.2).  Names are matched
# without regard to case (FE2.S, Gen/ and *.WAV are refused like fe2.s, gen/, *.wav):
#   - a path named fe2.s, fe2.s.*, fixups.h, or a binary named frontier, at any depth;
#   - anything under a gen/, sfx/, savs/, music/, captures/ or oracle-out/ directory;
#   - any *.fstr, *.wav or *.ogg file;
#   - a file larger than 1 MB under tools/oracle/ (tapes and goldens are small text);
#   - any blob whose SHA-256, after stripping CRs, equals a known fe2.s lineage hash
#     or a hash listed in tools/oracle/gamedata.sha256 (catches a renamed fe2.s);
#   - any executable (Mach-O, fat Mach-O, ELF or PE): the repository is source, and
#     every build of the game links the translated core (catches a renamed binary,
#     e.g. the fe2soak copy tools/soak/soak.sh makes);
#   - any blob identical to a local game file in this work tree: fe2.s, fe2.s.c,
#     fe2.s.bin, fixups.h, frontier, san/*/frontier, gen/* (catches any renaming);
#   - when fe2.s is present locally, any file sharing 10 or more distinct lines of
#     8+ characters with it, after whitespace is collapsed (catches an excerpt);
#   - any raster image (png, jpg, gif, bmp, ppm, tga, tiff, ...), since screenshots
#     show game screens, unless its path is listed in .githooks/allowed-images.
# Oracle tapes (tools/oracle/tapes/*.fe2rep) hold only ST key codes, VBL indices
# and hashes, so they are allowed.
#
# Exit status: 0 clean, 1 game data found, 2 usage or git error.

set -u

mode=index
rev=
while [ $# -gt 0 ]; do
  case "$1" in
    --staged) mode=staged ;;
    --rev) shift; [ $# -gt 0 ] || { echo "check-no-game-data: --rev needs a revision" >&2; exit 2; }
           mode=rev; rev=$1 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "check-no-game-data: unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

top=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "check-no-game-data: not in a git work tree" >&2; exit 2; }
cd "$top" || exit 2

# SHA-256 of fe2.s with CRs stripped, lineages A and D (docs/05, docs/07 section 2).
known_hashes="fa7699c1611ed0cc32c71a9f6c08c8ac77ba84c3d7854ccd5b6b646f6f3d298f
77bd74fa4ce35d7cd95af24105c581f83400a0ab74485bc3d62e4b376c1103b4"

# Lines "<mode> <type> <blob> <size>\t<path>" for the files to check.
list_entries() {
  case "$mode" in
    index)
      git ls-files -s -z | tr '\0' '\n' | while IFS= read -r line; do
        meta=${line%%	*}; path=${line#*	}
        set -- $meta
        printf '%s %s\t%s\n' "$2" "$(git cat-file -s "$2")" "$path"
      done ;;
    staged)
      git diff --cached --name-only -z --diff-filter=ACMRT | tr '\0' '\n' | while IFS= read -r path; do
        [ -n "$path" ] || continue
        blob=$(git rev-parse -q --verify ":$path" 2>/dev/null) || continue
        printf '%s %s\t%s\n' "$blob" "$(git cat-file -s "$blob")" "$path"
      done ;;
    rev)
      git rev-parse -q --verify "$rev^{tree}" >/dev/null || { echo "check-no-game-data: bad revision: $rev" >&2; exit 2; }
      git ls-tree -r -l -z "$rev" | tr '\0' '\n' | while IFS= read -r line; do
        meta=${line%%	*}; path=${line#*	}
        set -- $meta
        [ "$2" = blob ] || continue
        printf '%s %s\t%s\n' "$3" "$4" "$path"
      done ;;
  esac
}

extra_hashes=
if [ -f tools/oracle/gamedata.sha256 ]; then
  extra_hashes=$(grep -oE '[0-9a-fA-F]{64}' tools/oracle/gamedata.sha256 | tr 'A-F' 'a-f')
fi
all_hashes="$known_hashes
$extra_hashes"

entries=$(list_entries) || exit 2
bad=0
report() { echo "check-no-game-data: REFUSED $1 ($2)" >&2; bad=1; }

# Blob ids of the game files present in this work tree (absent ones are skipped).
local_blobs=$(
  { for f in fe2.s fe2.s.c fe2.s.bin fixups.h frontier san/*/frontier gen/*; do
      [ -f "$f" ] && printf '%s\n' "$f"
    done; } | git hash-object --stdin-paths 2>/dev/null)

allowed_images=
[ -f .githooks/allowed-images ] && allowed_images=$(grep -v '^#' .githooks/allowed-images)

# Excerpt check input: the text blobs to compare with fe2.s, one stream.
tmp=$(mktemp -d "${TMPDIR:-/tmp}/check-no-game-data.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT
: >"$tmp/stream"

while IFS= read -r entry; do
  [ -n "$entry" ] || continue
  meta=${entry%%	*}; path=${entry#*	}
  blob=${meta%% *}; size=${meta#* }
  lpath=$(printf '%s' "$path" | tr 'A-Z' 'a-z')
  base=${lpath##*/}
  case "/$lpath" in
    */gen/*|*/sfx/*|*/savs/*|*/music/*|*/captures/*|*/oracle-out/*)
      report "$path" "game-data directory"; continue ;;
  esac
  case "$base" in
    fe2.s|fe2.s.*|fixups.h|frontier) report "$path" "fe2.s or a file derived from it"; continue ;;
    gen|sfx|savs|music) report "$path" "game-data path"; continue ;;
    *.fstr|*.wav|*.ogg) report "$path" "game frame stream or audio"; continue ;;
    *.png|*.jpg|*.jpeg|*.gif|*.bmp|*.ppm|*.pgm|*.pnm|*.tga|*.tif|*.tiff|*.webp|*.heic|*.iff|*.pi1|*.neo)
      if ! printf '%s\n' "$allowed_images" | grep -qxF -- "$path"; then
        report "$path" "image (screenshots show game screens); list it in .githooks/allowed-images if it is not one"
        continue
      fi ;;
  esac
  if [ -n "$local_blobs" ] && printf '%s\n' "$local_blobs" | grep -qx "$blob"; then
    report "$path" "identical to a local game file"; continue
  fi
  magic=$(git cat-file blob "$blob" | od -An -tx1 -N4 | tr -d ' \n')
  case "$magic" in
    cffaedfe|cefaedfe|feedfacf|feedface|cafebabe|bebafeca|7f454c46|4d5a*)
      report "$path" "executable binary ($magic)"; continue ;;
  esac
  case "$path" in
    tools/oracle/*) [ "$size" -gt 1048576 ] && { report "$path" "over 1 MB under tools/oracle/"; continue; } ;;
  esac
  # Content check: a renamed copy of fe2.s.  fe2.s is ~1.8 MB, so skip tiny blobs.
  if [ "$size" -gt 65536 ]; then
    h=$(git cat-file blob "$blob" | tr -d '\r' | shasum -a 256 | cut -c1-64)
    if printf '%s\n' "$all_hashes" | grep -qx "$h"; then
      report "$path" "content matches a known game-data SHA-256"; continue
    fi
  fi
  if [ -f fe2.s ] && [ "$size" -le 4194304 ]; then
    { printf '\001%s\n' "$path"; git cat-file blob "$blob"; printf '\n'; } >>"$tmp/stream"
  fi
done <<ENTRIES
$entries
ENTRIES

# Excerpt check: distinct normalised lines (8+ chars) a file shares with fe2.s.
if [ -f fe2.s ] && [ -s "$tmp/stream" ]; then
  excerpts=$(LC_ALL=C awk '
    function norm(l) { gsub(/\r/, "", l); gsub(/[ \t]+/, " ", l); sub(/^ /, "", l); sub(/ $/, "", l); return l }
    FNR == NR { l = norm($0); if (length(l) >= 8) src[l] = 1; next }
    /^\001/ { flush(); file = substr($0, 2); delete seen; n = 0; next }
    { l = norm($0); if (length(l) >= 8 && (l in src) && !(l in seen)) { seen[l] = 1; n++ } }
    function flush() { if (file != "" && n >= 10) printf "%s\t%d\n", file, n }
    END { flush() }' fe2.s "$tmp/stream")
  while IFS='	' read -r path n; do
    [ -n "$path" ] && report "$path" "shares $n lines with fe2.s: an excerpt"
  done <<EXCERPTS
$excerpts
EXCERPTS
fi

if [ "$bad" -ne 0 ]; then
  echo "check-no-game-data: game data is Frontier Developments copyright and must not be committed." >&2
  echo "check-no-game-data: unstage with: git reset -q -- <path>" >&2
  exit 1
fi
echo "check-no-game-data: OK ($mode)"
exit 0
