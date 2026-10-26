#!/usr/bin/env bash
# check-no-game-data.sh -- refuse to let Frontier Developments game data into git.
#
# The 68000 source (fe2.s), everything derived from it (fe2.s.c, fe2.s.bin,
# fixups.h, gen/, the frontier binary) and the game assets (sfx, music, savs)
# are copyright Frontier Developments.  They must stay on the local machine.
# This script fails (exit 1) if any of them is tracked or staged.
#
# Usage:
#   tools/check-no-game-data.sh            # every path in the index
#   tools/check-no-game-data.sh --staged   # paths added or changed in the index (pre-commit hook)
#   tools/check-no-game-data.sh --rev REV  # every path in commit REV (CI: --rev HEAD)
#
# Each blob is refused by the first rule it meets (names are matched without
# regard to case):
#   name     a path named fe2.s, fe2.s.*, fixups.h or frontier, a path through a
#            gen/, sfx/, savs/, music/, captures/ or oracle-out/ directory, a
#            *.fstr, *.wav, *.ogg or *.sav file, or a path containing a newline;
#   copy     a blob identical to a local game file in this work tree: fe2.s and
#            its derived files, frontier, san/*/frontier, gen/*, every file under
#            sfx/, music/, savs/, captures/, oracle-out/, and every *.wav, *.ogg,
#            *.sav or *.fstr file;
#   hash     a blob over 64 KB whose SHA-256, CRs stripped, is a known fe2.s hash
#            or one listed in tools/oracle/gamedata.sha256 (needs no local files);
#   image    a raster image, by name or by PNG/JPEG/GIF/TIFF magic, whose path is
#            not listed in .githooks/allowed-images (screenshots show the game);
#   magic    an executable (Mach-O, ELF, PE, Atari ST), audio (RIFF, OggS) or
#            compressed or archive data (gzip, zip, bzip2, xz, zstd, 7z), unless
#            its path is listed in .githooks/allowed-binaries.  Compressed data is
#            refused, not unpacked.  The allow-lists are read from the index
#            (from REV with --rev), so an unstaged edit to them has no effect;
#   oracle   a file over 1 MB under tools/oracle/;
#   near     a blob within 1% of the size of local fe2.s.bin, frontier or
#            san/*/frontier that matches it at >= 98% of byte positions or in
#            >= 98% of its bytes counted by NUL-, CR- and LF-delimited runs (so
#            one inserted byte does not hide it);
#   excerpt  a file sharing 10 or more distinct lines with the local fe2.s,
#            fe2.s.c, fixups.h or gen/*.c, or, once 40 or more such lines are
#            shared by the files checked (and, with --staged, by every text file
#            in the index), each file that shares any (an excerpt split
#            across files or commits).  Lines are split at LF, CR and NUL and
#            compared with all whitespace removed and case folded; a line counts
#            only when 8 or more characters remain.
# The copy, near and excerpt rules need the local game files, so they do nothing
# where those are absent (CI).  What the rules do not catch: .githooks/README.md.
#
# Exit status: 0 clean, 1 game data found, 2 usage or git error.

set -u
shopt -s nocasematch

mode=index
rev=
while [ $# -gt 0 ]; do
  case "$1" in
    --staged) mode=staged ;;
    --rev) shift; [ $# -gt 0 ] || { echo "check-no-game-data: --rev needs a revision" >&2; exit 2; }
           mode=rev; rev=$1 ;;
    -h|--help) sed -n '2,/^# Exit status/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "check-no-game-data: unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

top=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "check-no-game-data: not in a git work tree" >&2; exit 2; }
cd "$top" || exit 2
if [ "$mode" = rev ]; then
  git rev-parse -q --verify "$rev^{tree}" >/dev/null || { echo "check-no-game-data: bad revision: $rev" >&2; exit 2; }
fi

# SHA-256 of fe2.s with CRs stripped, lineages A and D (docs/05, docs/07 section 2).
known_hashes="fa7699c1611ed0cc32c71a9f6c08c8ac77ba84c3d7854ccd5b6b646f6f3d298f
77bd74fa4ce35d7cd95af24105c581f83400a0ab74485bc3d62e4b376c1103b4"
if [ -f tools/oracle/gamedata.sha256 ]; then
  known_hashes="$known_hashes
$(grep -oE '[0-9a-fA-F]{64}' tools/oracle/gamedata.sha256 | tr 'A-F' 'a-f')"
fi
if command -v sha256sum >/dev/null 2>&1; then sha256() { sha256sum; }; else sha256() { shasum -a 256; }; fi

# Excerpt thresholds: distinct shared lines per file, and over all files.
per_file=10
all_files=40

nl='
'
tab='	'
tmp=$(mktemp -d "${TMPDIR:-/tmp}/check-no-game-data.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT

# "<blob> <flag>\t<path>" for each blob to check (gitlinks are skipped).  The
# flag is "nl" when the path holds a newline, shown as \n.
emit() {
  local flag=ok p=$2
  case "$p" in *"$nl"*) flag=nl p=${p//$nl/\\n} ;; esac
  printf '%s %s\t%s\n' "$1" "$flag" "$p"
}
list_blobs() {
  local meta path
  case "$mode" in
    index)
      git ls-files -s -z | while IFS= read -r -d '' meta; do
        path=${meta#*$tab}; set -- ${meta%%$tab*}     # mode blob stage
        [ "$1" = 160000 ] || emit "$2" "$path"
      done ;;
    staged)
      git diff --cached --raw -z --no-renames --no-abbrev --diff-filter=ACMRT |
      while IFS= read -r -d '' meta && IFS= read -r -d '' path; do
        set -- $meta                                  # :old new oldblob blob status
        [ "$2" = 160000 ] || emit "$4" "$path"
      done ;;
    rev)
      git ls-tree -r -z "$rev" | while IFS= read -r -d '' meta; do
        path=${meta#*$tab}; set -- ${meta%%$tab*}     # mode type blob
        [ "$2" = blob ] && emit "$3" "$path"
      done ;;
  esac
}
list_blobs >"$tmp/list" || exit 2
# Prefix each with its size: "<size> <blob> <flag>\t<path>".
cut -d' ' -f1 "$tmp/list" | git cat-file --batch-check='%(objectsize)' | paste -d' ' - "$tmp/list" >"$tmp/entries" || exit 2

# The local game files in this work tree (absent ones are skipped; none in CI).
{
  for f in fe2.s fe2.s.c fe2.s.bin fixups.h frontier san/*/frontier gen/*; do
    [ -f "$f" ] && printf '%s\n' "$f"
  done
  for d in sfx music savs captures oracle-out; do
    [ -e "$d" ] && find -L "$d" -type f 2>/dev/null
  done
  find . -name .git -prune -o -type f \( -iname '*.wav' -o -iname '*.ogg' -o -iname '*.sav' \
    -o -iname '*.fstr' \) -print 2>/dev/null | sed 's|^\./||'
} | sort -u >"$tmp/local"
# "<blob> <path>" for each, so a copy is named in the refusal.
: >"$tmp/local-blobs"
[ -s "$tmp/local" ] && git hash-object --stdin-paths <"$tmp/local" | paste -d' ' - "$tmp/local" >"$tmp/local-blobs"
local_ids=$(cut -d' ' -f1 "$tmp/local-blobs")
# References for the near rule: "<size> <path>".
for f in fe2.s.bin frontier san/*/frontier; do
  [ -f "$f" ] && printf '%s %s\n' "$(wc -c <"$f" | tr -d ' ')" "$f"
done >"$tmp/near-refs"
# Corpus for the excerpt rule.
corpus=()
for f in fe2.s fe2.s.c fixups.h gen/*.c; do
  [ -f "$f" ] && corpus+=("$f")
done

# The allow-lists, read from the index (or REV) like the files they excuse.
case "$mode" in rev) at="$rev:" ;; *) at=":" ;; esac
for l in allowed-images allowed-binaries; do
  git cat-file blob "${at}.githooks/$l" 2>/dev/null | grep -v '^#' >"$tmp/$l"
done

bad=0
report() { echo "check-no-game-data: REFUSED $1 ($2)" >&2; bad=1; }
listed() { grep -qxF -- "$2" "$tmp/$1"; }   # allow-list path

# near_match CANDIDATE SIZE: print "<ref> <positional ratio> <run ratio>" for the
# first reference the candidate nearly matches; fail if none.
near_match() {
  local cand=$1 size=$2 rsize ref d big small diffs pos runs
  while read -r rsize ref; do
    d=$((size - rsize)); [ "$d" -lt 0 ] && d=$((-d))
    [ $((d * 100)) -le "$rsize" ] || continue
    if [ "$size" -gt "$rsize" ]; then big=$size small=$rsize; else big=$rsize small=$size; fi
    # Byte positions: differing bytes over the common length, capped at 2%.
    diffs=$(cmp -l "$cand" "$ref" 2>/dev/null | head -n $((big / 50 + 1)) | wc -l | tr -d ' ')
    pos=$(awk -v s="$small" -v d="$diffs" -v b="$big" 'BEGIN { printf "%.4f", (s - d) / b }')
    # Runs between NUL, CR and LF bytes: the share of the candidate's bytes that
    # lie in runs also found in the reference.  An insertion changes one run.
    runs=$(tr '\0\r' '\n\n' <"$cand" | LC_ALL=C awk -v ref="$ref" '
      BEGIN { cmd = "tr \"\\000\\r\" \"\\n\\n\" < \"" ref "\""
              while ((cmd | getline l) > 0) if (l != "") seen[l] = 1
              close(cmd) }
      $0 != "" { t += length($0); if ($0 in seen) m += length($0) }
      END { printf "%.4f", t ? m / t : 0 }')
    if awk -v p="$pos" -v r="$runs" 'BEGIN { exit !(p >= 0.98 || r >= 0.98) }'; then
      printf '%s %s %s\n' "$ref" "$pos" "$runs"; return 0
    fi
  done <"$tmp/near-refs"
  return 1
}

mkdir "$tmp/text"
: >"$tmp/excerpt-list"   # "<text file>\t<f|c>\t<path>" for the excerpt pass
n=0
while IFS= read -r entry; do
  [ -n "$entry" ] || continue
  path=${entry#*$tab}
  set -- ${entry%%$tab*}
  size=$1 blob=$2 flag=$3
  base=${path##*/}
  # name (nocasematch is on, so these patterns ignore case)
  [ "$flag" = nl ] && { report "$path" "name: path contains a newline"; continue; }
  case "/$path" in
    */gen/*|*/sfx/*|*/savs/*|*/music/*|*/captures/*|*/oracle-out/*)
      report "$path" "name: game-data directory"; continue ;;
  esac
  case "$base" in
    fe2.s|fe2.s.*|fixups.h|frontier) report "$path" "name: fe2.s or a file derived from it"; continue ;;
    gen|sfx|savs|music|captures|oracle-out) report "$path" "name: game-data path"; continue ;;
    *.fstr|*.wav|*.ogg|*.sav) report "$path" "name: game frame stream, audio or save"; continue ;;
  esac
  # copy
  case "$nl$local_ids$nl" in
    *"$nl$blob$nl"*)
      copy=$(awk -v b="$blob" '$1 == b { sub(/^[^ ]* /, ""); print; exit }' "$tmp/local-blobs")
      report "$path" "copy: identical to the local game file $copy"; continue ;;
  esac
  git cat-file blob "$blob" >"$tmp/raw" || exit 2
  # hash (fe2.s is ~1.8 MB, so small blobs are skipped)
  if [ "$size" -gt 65536 ]; then
    h=$(tr -d '\r' <"$tmp/raw" | sha256 | cut -c1-64)
    case "$nl$known_hashes$nl" in
      *"$nl$h$nl"*) report "$path" "hash: content matches a known game-data SHA-256"; continue ;;
    esac
  fi
  magic=$(od -An -tx1 -N12 "$tmp/raw"); magic=${magic// /}; magic=${magic//$nl/}
  # image
  is_image=0
  case "$base" in
    *.png|*.jpg|*.jpeg|*.gif|*.bmp|*.ppm|*.pgm|*.pnm|*.tga|*.tif|*.tiff|*.webp|*.heic|*.iff|*.pi1|*.neo) is_image=1 ;;
  esac
  case "$magic" in
    89504e470d0a1a0a*|ffd8ff*|474946383761*|474946383961*|49492a00*|4d4d002a*) is_image=1 ;;
  esac
  if [ "$is_image" = 1 ] && ! listed allowed-images "$path"; then
    report "$path" "image: screenshots show game screens; list it in .githooks/allowed-images if it is not one"
    continue
  fi
  # magic
  kind=
  case "$magic" in
    cffaedfe*|cefaedfe*|feedfacf*|feedface*|cafebabe*|bebafeca*) kind="executable (Mach-O)" ;;
    7f454c46*) kind="executable (ELF)" ;;
    4d5a*) kind="executable (PE)" ;;
    601a*) kind="executable (Atari ST)" ;;
    52494646*) kind="audio or media (RIFF)" ;;
    4f676753*) kind="audio (OggS)" ;;
    1f8b*) kind="compressed (gzip)" ;;
    504b0304*|504b0506*|504b0708*) kind="archive (zip)" ;;
    425a683[1-9]*) kind="compressed (bzip2)" ;;
    fd377a585a00*) kind="compressed (xz)" ;;
    28b52ffd*) kind="compressed (zstd)" ;;
    377abcaf271c*) kind="archive (7z)" ;;
  esac
  if [ -n "$kind" ] && ! listed allowed-binaries "$path"; then
    report "$path" "magic: $kind; list it in .githooks/allowed-binaries if it is not game data"
    continue
  fi
  # oracle
  case "$path" in
    tools/oracle/*) [ "$size" -gt 1048576 ] && { report "$path" "oracle: over 1 MB under tools/oracle/"; continue; } ;;
  esac
  # near
  if [ -s "$tmp/near-refs" ] && near=$(near_match "$tmp/raw" "$size"); then
    set -- $near
    report "$path" "near: within 1% of the size of $1 and matches it (positions $2, runs $3)"
    continue
  fi
  # excerpt: keep the text; it is compared with the corpus in one pass below.
  if [ ${#corpus[@]} -gt 0 ]; then
    n=$((n + 1))
    tr '\0\r' '\n\n' <"$tmp/raw" >"$tmp/text/$n"
    printf '%s\tf\t%s\n' "$tmp/text/$n" "$path" >>"$tmp/excerpt-list"
  fi
done <"$tmp/entries"

# excerpt: the checked files' lines go into one table; the corpus is read once.
if [ ${#corpus[@]} -gt 0 ] && [ -s "$tmp/excerpt-list" ]; then
  if [ "$mode" = staged ]; then
    # Everything already in the index counts towards the total, so an excerpt
    # split across commits is refused once the tree holds enough of it.
    git -c grep.lineNumber=false -c grep.column=false grep --no-color --cached -a -h -e '' 2>/dev/null |
      tr '\0\r' '\n\n' >"$tmp/text/index"
    printf '%s\tc\t\n' "$tmp/text/index" >>"$tmp/excerpt-list"
  fi
  LC_ALL=C awk -v list="$tmp/excerpt-list" '
    function norm(l) { gsub(/[ \t\r\f\v]+/, "", l); return tolower(l) }
    BEGIN {
      while ((getline rec < list) > 0) {
        nf++; t = index(rec, "\t"); fn = substr(rec, 1, t - 1)
        ctx[nf] = substr(rec, t + 1, 1) == "c"; name[nf] = substr(rec, t + 3)
        while ((getline l < fn) > 0) {
          l = norm(l)
          if (length(l) >= 8 && !((nf, l) in mine)) { mine[nf, l] = 1; cand[l] = cand[l] " " nf }
        }
        close(fn)
      }
    }
    { l = norm($0); if (length(l) >= 8 && (l in cand)) hit[l] = 1 }
    END {
      for (l in hit) { u++; k = split(cand[l], id, " "); for (i = 1; i <= k; i++) cnt[id[i]]++ }
      printf "%d\n", u
      for (i = 1; i <= nf; i++) if (!ctx[i] && cnt[i] > 0) printf "%d\t%s\n", cnt[i], name[i]
    }' "${corpus[@]}" >"$tmp/shared"
  total=$(head -1 "$tmp/shared")
  while IFS= read -r line; do
    k=${line%%$tab*}; path=${line#*$tab}
    if [ "$k" -ge "$per_file" ]; then
      report "$path" "excerpt: shares $k lines with fe2.s or the C built from it"
    elif [ "$total" -ge "$all_files" ]; then
      report "$path" "excerpt: shares $k lines, and the tree $total in all, with fe2.s or the C built from it: a split excerpt"
    fi
  done < <(tail -n +2 "$tmp/shared")
fi

if [ "$bad" -ne 0 ]; then
  echo "check-no-game-data: game data is Frontier Developments copyright and must not be committed." >&2
  echo "check-no-game-data: unstage with: git reset -q -- <path>" >&2
  exit 1
fi
echo "check-no-game-data: OK ($mode)"
exit 0
