#!/usr/bin/env bash
# Regression test for the game-data guard (tools/check-no-game-data.sh), its
# pre-commit hook (.githooks/) and `make -f Makefile-C install-hooks`.  Every
# claim in .githooks/README.md has a case here, named in brackets there.
#
# Scratch repositories are made under ${TMPDIR:-/tmp} and deleted at the end.
# Game-data fixtures are derived at run time from FE2_REF_DIR (default
# $HOME/Frontier-build-a), a work tree holding fe2.s, fe2.s.c, fe2.s.bin,
# fixups.h, gen/*.c, frontier and sfx/*.wav.  They are copied into the scratch
# directory, never into this repository, and deleted with it.  When
# FE2_REF_DIR lacks them the cases that need them are skipped, and say so.
#
# Every commit goes through the hook: git -c core.hooksPath=.githooks commit.
# Exit 0 when every case that ran behaved as expected.
set -u
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
ref=${FE2_REF_DIR:-$HOME/Frontier-build-a}
work=$(mktemp -d "${TMPDIR:-/tmp}/gamedata-test.XXXXXX") || exit 2
trap 'rm -rf "$work"' EXIT
# Isolate git from this user's and the system's configuration; the "global"
# file is then $work/home/.gitconfig, so a --global write would be seen.
mkdir -p "$work/home"
export HOME=$work/home XDG_CONFIG_HOME=$work/home/.config GIT_CONFIG_NOSYSTEM=1
passed=0 failed=0 skipped=0
pass() { passed=$((passed + 1)); echo "PASS $*"; }
fail() { failed=$((failed + 1)); echo "FAIL $*"; }
skip() { skipped=$((skipped + 1)); echo "SKIP $*"; }

# new_repo DIR [local]: a scratch repository holding the guard, its hook, the
# allow-lists, this repository's .gitignore and Makefile-C; with "local", also
# the game files a developer's work tree has (copies of FE2_REF_DIR's).
new_repo() {
  local r=$1 f
  git init -q "$r" || exit 2
  git -C "$r" config user.name gamedata-test
  git -C "$r" config user.email gamedata-test@example.invalid
  git -C "$r" config commit.gpgsign false
  mkdir -p "$r/tools/oracle" "$r/.githooks"
  cp "$top/tools/check-no-game-data.sh" "$r/tools/"
  cp "$top/.githooks/pre-commit" "$top/.githooks/allowed-images" "$top/.githooks/allowed-binaries" "$r/.githooks/"
  cp "$top/.gitignore" "$top/Makefile-C" "$r/"
  if [ "${2:-}" = local ]; then
    for f in fe2.s fe2.s.c fe2.s.bin fixups.h frontier gen sfx; do ln -s "$work/game/$f" "$r/$f"; done
  fi
  git -C "$r" add .gitignore Makefile-C tools .githooks
  git -C "$r" -c core.hooksPath=.githooks commit -q -m init || { echo "cannot make $r" >&2; exit 2; }
}

# commit_hook REPO MESSAGE: commit through the hook; output in $out, status in $rc.
commit_hook() {
  out=$(git -C "$1" -c core.hooksPath=.githooks commit -q -m "$2" 2>&1); rc=$?
}

# refuse REPO CASE RULE PATH...: stage PATH... (already written), commit through
# the hook, and expect the commit to fail with every PATH refused for a reason
# containing RULE.  The paths are then unstaged and deleted.
refuse() {
  local r=$1 name=$2 rule=$3 p ok=1; shift 3
  git -C "$r" add -f -- "$@" || { fail "$name: cannot stage $*"; return; }
  commit_hook "$r" "$name"
  [ "$rc" -ne 0 ] || ok=0
  for p in "$@"; do
    printf '%s\n' "$out" | grep -F "REFUSED $p (" | grep -qF "$rule" || ok=0
  done
  if [ "$ok" = 1 ]; then pass "$name: refused ($rule)"
  else fail "$name: expected refusal ($rule), rc=$rc: $(printf '%s\n' "$out" | head -5)"; fi
  git -C "$r" reset -q -- "$@" 2>/dev/null
  (cd "$r" && rm -f -- "$@")
}

# accept REPO CASE [keep|drop] PATH...: stage and commit through the hook and
# expect it to succeed.  With drop the commit is undone afterwards.
accept() {
  local r=$1 name=$2 how=$3 p; shift 3
  git -C "$r" add -f -- "$@" || { fail "$name: cannot stage $*"; return; }
  commit_hook "$r" "$name"
  if [ "$rc" -eq 0 ]; then pass "$name: accepted"
  else
    fail "$name: expected acceptance, rc=$rc: $(printf '%s\n' "$out" | head -5)"
    git -C "$r" reset -q -- "$@" 2>/dev/null; return
  fi
  [ "$how" = drop ] || return 0
  git -C "$r" reset -q --hard HEAD~1
  # The reset removes a directory the dropped file left empty; later cases write there.
  for p in "$@"; do mkdir -p "$r/$(dirname "$p")"; done
}

# A text file of N benign lines, each unique.
benign_text() { awk -v n="$1" -v t="$2" 'BEGIN { for (i = 1; i <= n; i++) printf "%s line %d of a benign test file\n", t, i }'; }

# --- usage ----------------------------------------------------------------
syn=$work/syn
new_repo "$syn"
"$syn/tools/check-no-game-data.sh" --bogus >/dev/null 2>&1; r1=$?
(cd "$syn" && tools/check-no-game-data.sh --rev no-such-rev >/dev/null 2>&1); r2=$?
(cd "$work" && "$syn/tools/check-no-game-data.sh" >/dev/null 2>&1); r3=$?
if [ "$r1" = 2 ] && [ "$r2" = 2 ] && [ "$r3" = 2 ]; then pass "[usage] bad argument, bad revision, no work tree: exit 2"
else fail "[usage] expected exit 2/2/2, got $r1/$r2/$r3"; fi

# --- name rule: case variants and paths that need no game files ------------
cd "$syn" || exit 2
mkdir -p a b Gen SFX Music Savs Captures Oracle-Out x
for p in FE2.S Fe2.S.c a/FE2.S.BIN a/Fixups.H b/Frontier Gen/part.c SFX/readme.txt \
         Music/notes.txt Savs/slot1.txt Captures/c1.txt Oracle-Out/o.txt x/TEST.WAV \
         x/theme.OGG x/game.SAV x/run.FSTR; do
  printf 'not game data\n' >"$p"
  refuse "$syn" "[name] $p" "name:" "$p"
done
nlpath="x/a
b.txt"
printf 'x\n' >"$nlpath"
git add -f -- "$nlpath"; commit_hook "$syn" newline
if [ "$rc" -ne 0 ] && printf '%s\n' "$out" | grep -qF 'REFUSED x/a\nb.txt (name: path contains a newline)'; then
  pass "[name-newline] a path holding a newline: refused"
else fail "[name-newline] rc=$rc: $out"; fi
git reset -q -- "$nlpath"; rm -f -- "$nlpath"

# --- magic rule (synthetic headers) ------------------------------------------
mkdir -p bin
magic_case() { # file printf-bytes kind
  { printf "$2"; benign_text 20 magic; } >"$1"
  refuse "$syn" "[magic] $1" "magic: $3" "$1"
}
magic_case bin/a.out '\317\372\355\376' 'executable (Mach-O)'
magic_case bin/fat.out '\312\376\272\276' 'executable (Mach-O)'
magic_case bin/a.elf '\177ELF' 'executable (ELF)'
magic_case bin/a.exe 'MZ' 'executable (PE)'
magic_case bin/a.prg '\140\032' 'executable (Atari ST)'
magic_case bin/a.dat 'RIFF\000\000\000\000WAVE' 'audio or media (RIFF)'
magic_case bin/b.dat 'OggS' 'audio (OggS)'
magic_case bin/c.dat '\037\213' 'compressed (gzip)'
magic_case bin/d.dat 'PK\003\004' 'archive (zip)'
magic_case bin/e.dat 'BZh9' 'compressed (bzip2)'
magic_case bin/f.dat '\375\067\172\130\132\000' 'compressed (xz)'
magic_case bin/g.dat '\050\265\057\375' 'compressed (zstd)'
magic_case bin/h.dat '\067\172\274\257\047\034' 'archive (7z)'
# [allow-binaries] a listed path is exempt from the magic rule.
printf 'vendor/fixture.gz\n' >>.githooks/allowed-binaries
git add .githooks/allowed-binaries && commit_hook "$syn" allow-list
mkdir -p vendor && benign_text 50 vendor | gzip -c >vendor/fixture.gz
accept "$syn" "[allow-binaries] listed gzip" keep vendor/fixture.gz
# [allow-unstaged] the allow-lists are read from the index: an edit that is not
# staged exempts nothing.
printf 'vendor/other.gz\n' >>.githooks/allowed-binaries
benign_text 50 other | gzip -c >vendor/other.gz
refuse "$syn" "[allow-unstaged] unstaged allow-list entry" "magic: compressed (gzip)" vendor/other.gz
git checkout -q -- .githooks/allowed-binaries
# [gap-allow-same-commit] a path added to an allow-list in the same commit as
# the file is exempt: the guard cannot tell a reviewed entry from a new one.
printf 'vendor/third.gz\n' >>.githooks/allowed-binaries
benign_text 50 third | gzip -c >vendor/third.gz
accept "$syn" "[gap-allow-same-commit] allow-list entry staged with the file" drop .githooks/allowed-binaries vendor/third.gz
rm -f vendor/third.gz

# --- image rule ----------------------------------------------------------------
mkdir -p docs/screenshots img
{ printf '\211PNG\r\n\032\n'; benign_text 5 png; } >img/shot.png
refuse "$syn" "[image] unlisted .png" "image:" img/shot.png
{ printf '\211PNG\r\n\032\n'; benign_text 5 png; } >img/pic.dat
refuse "$syn" "[image] PNG magic under another name" "image:" img/pic.dat
{ printf '\377\330\377\340'; benign_text 5 jpg; } >img/photo.bin
refuse "$syn" "[image] JPEG magic" "image:" img/photo.bin
{ printf 'GIF89a'; benign_text 5 gif; } >img/anim.bin
refuse "$syn" "[image] GIF magic" "image:" img/anim.bin
benign_text 5 shot >docs/screenshots/title.png
refuse "$syn" "[image] docs/screenshots/*.png" "image:" docs/screenshots/title.png

# --- oracle rule -----------------------------------------------------------
benign_text 30000 oracle >tools/oracle/big.txt
refuse "$syn" "[oracle] over 1 MB under tools/oracle/" "oracle:" tools/oracle/big.txt

# --- hash rule via gamedata.sha256 (needs no game files) ------------------------
benign_text 3000 listed >src.txt
h=$(tr -d '\r' <src.txt | shasum -a 256 | cut -c1-64)
printf '%s  listed fixture\n' "$h" >tools/oracle/gamedata.sha256
git add tools/oracle/gamedata.sha256 && commit_hook "$syn" sha-list
refuse "$syn" "[hash] a SHA-256 listed in tools/oracle/gamedata.sha256" "hash:" src.txt

# --- benign controls -----------------------------------------------------------
benign_controls() { # repo
  mkdir -p "$1/src" "$1/doc"
  cat >"$1/src/util.c" <<'C'
#include <stdio.h>
#include <stdlib.h>

/* Clamp v to [lo, hi]. */
static int clamp(int v, int lo, int hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

int main(int argc, char **argv)
{
	int i, total = 0;
	for (i = 1; i < argc; i++)
		total += clamp(atoi(argv[i]), 0, 100);
	printf("%d\n", total);
	return 0;
}
C
  cat >"$1/doc/guide.md" <<'M'
# Building

Run `make -f Makefile-C` from the top of the tree, then `./frontier`.
The game files (fe2.s and what is built from it) stay on your machine.

| Target | What it does |
|---|---|
| install-hooks | runs the game-data guard before each commit |
M
  accept "$1" "[benign] C file" keep src/util.c
  cp "$top/cursor.png" "$1/cursor.png"
  accept "$1" "[benign] PNG icon cursor.png (listed in allowed-images)" keep cursor.png
  accept "$1" "[benign] Markdown doc" keep doc/guide.md
  (cd "$top" && git ls-files -z tests) | (cd "$top" && xargs -0 tar cf -) | (cd "$1" && tar xf -)
  (cd "$1" && git ls-files -o --exclude-standard -z tests | xargs -0 git add -f --)
  commit_hook "$1" tests
  if [ "$rc" -eq 0 ]; then pass "[benign] this repository's tests/ ($(cd "$1" && git ls-files tests | wc -l | tr -d ' ') files): accepted"
  else fail "[benign] tests/: rc=$rc: $out"; fi
}
benign_controls "$syn"

# --- install-hooks -----------------------------------------------------------
global_before=$(git config --global --get core.hooksPath 2>/dev/null || echo unset)
make -s -C "$syn" -f Makefile-C install-hooks >/dev/null 2>&1; mrc=$?
global_after=$(git config --global --get core.hooksPath 2>/dev/null || echo unset)
local_value=$(git -C "$syn" config --local --get core.hooksPath)
if [ "$mrc" = 0 ] && [ "$local_value" = .githooks ] && [ "$global_before" = "$global_after" ]; then
  pass "[install-hooks] sets core.hooksPath=.githooks in the repository, global unchanged ($global_after)"
else fail "[install-hooks] rc=$mrc local=$local_value global $global_before -> $global_after"; fi
printf 'x\n' >a/fe2.s
git add -f a/fe2.s
out=$(git commit -q -m installed 2>&1); rc=$?
if [ "$rc" -ne 0 ] && printf '%s\n' "$out" | grep -qF 'REFUSED a/fe2.s (name:'; then
  pass "[install-hooks] the installed hook refuses a plain git commit"
else fail "[install-hooks] plain commit rc=$rc: $out"; fi
git reset -q -- a/fe2.s; rm -f a/fe2.s
git worktree add -q "$work/syn-wt" >/dev/null 2>&1 && git -C "$work/syn-wt" config --local --unset core.hooksPath
make -s -C "$work/syn-wt" -f Makefile-C install-hooks >/dev/null 2>&1
if [ "$(git -C "$syn" config --local --get core.hooksPath)" = .githooks ]; then
  pass "[install-hooks-worktree] run in a linked worktree, it sets the shared repository config"
else fail "[install-hooks-worktree] the main repository has no core.hooksPath"; fi
git worktree remove --force "$work/syn-wt"

# --- cases that need the game files -------------------------------------------
need="fe2.s fe2.s.c fe2.s.bin fixups.h frontier"
have_ref=1
for f in $need; do [ -f "$ref/$f" ] || have_ref=0; done
ls "$ref"/gen/*.c >/dev/null 2>&1 || have_ref=0
ls "$ref"/sfx/*.wav >/dev/null 2>&1 || have_ref=0
if [ "$have_ref" = 0 ]; then
  skip "every case that needs the game files: FE2_REF_DIR=$ref lacks $need, gen/*.c or sfx/*.wav"
else
  mkdir -p "$work/game/gen" "$work/game/sfx"
  for f in $need; do cp "$ref/$f" "$work/game/"; done
  cp "$ref"/gen/*.c "$work/game/gen/"
  cp "$ref"/sfx/*.wav "$work/game/sfx/"
  dev=$work/dev
  new_repo "$dev" local
  cd "$dev" || exit 2
  mkdir -p assets src data bin docs split

  # .gitignore keeps the game files out of `git status`.
  mkdir -p music savs san/asan docs/screenshots
  : >music/a.ogg; : >savs/a.sav; : >san/asan/frontier; : >x.wav; : >docs/screenshots/a.png
  miss=
  for p in fe2.s fe2.s.c fe2.s.bin fixups.h frontier gen sfx music savs/a.sav san/asan/frontier x.wav docs/screenshots/a.png; do
    git check-ignore -q "$p" || miss="$miss $p"
  done
  git check-ignore -q src/util.c && miss="$miss (src/util.c ignored)"
  if [ -z "$miss" ] && [ -z "$(git status --porcelain)" ]; then pass "[gitignore] game files, sfx symlink, san/, *.wav, docs/screenshots/ ignored"
  else fail "[gitignore] not ignored:$miss; status: $(git status --porcelain | head -5)"; fi
  rm -rf music savs san x.wav docs/screenshots

  benign_controls "$dev"

  # (a) a sound effect under another name, exact and with one byte changed.
  wav=$(ls "$work"/game/sfx/*.wav | head -1)
  cp "$wav" assets/beep.dat
  refuse "$dev" "[copy] (a) sfx wav renamed assets/beep.dat" "copy:" assets/beep.dat
  cp "$wav" assets/beep2.dat; printf 'x' >>assets/beep2.dat
  refuse "$dev" "[magic] (a) sfx wav plus one byte" "magic: audio or media (RIFF)" assets/beep2.dat
  # (b) fe2.s.c plus one line, renamed.
  { cat "$work/game/fe2.s.c"; printf 'int gamedata_test_extra;\n'; } >src/core.c
  refuse "$dev" "[excerpt] (b) fe2.s.c plus one line as src/core.c" "excerpt:" src/core.c
  # (c) a generated C file plus one line: the first with 50 or more counted lines.
  gen=$(LC_ALL=C awk 'function norm(l) { gsub(/[ \t\r\f\v]+/, "", l); return tolower(l) }
    FNR == 1 { delete s; k = 0 } { l = norm($0); if (length(l) >= 8 && !(l in s)) { s[l] = 1; if (++k == 50) { print FILENAME; exit } } }' "$work"/game/gen/*.c)
  { cat "$gen"; printf 'int gamedata_test_extra;\n'; } >src/part.c
  refuse "$dev" "[excerpt] (c) gen/${gen##*/} plus one line as src/part.c" "excerpt:" src/part.c
  # (d) fixups.h plus one line.
  { cat "$work/game/fixups.h"; printf '/* extra */\n'; } >src/fix.h
  refuse "$dev" "[excerpt] (d) fixups.h plus one line as src/fix.h" "excerpt:" src/fix.h
  # (e) fe2.s.bin plus one byte, and variants that defeat the magic rule.
  bin=$work/game/fe2.s.bin
  { cat "$bin"; printf 'x'; } >data/core.bin
  refuse "$dev" "[magic] (e) fe2.s.bin plus one byte" "magic: executable (Atari ST)" data/core.bin
  { printf 'X'; tail -c +2 "$bin"; printf 'x'; } >data/core2.bin
  refuse "$dev" "[near] (e) fe2.s.bin, first byte changed, plus one byte" "near:" data/core2.bin
  { printf 'X'; cat "$bin"; } >data/core3.bin
  refuse "$dev" "[near] (e) fe2.s.bin with one byte inserted at the start" "near:" data/core3.bin
  printf 'data/core4.bin\n' >>.githooks/allowed-binaries
  git add .githooks/allowed-binaries && commit_hook "$dev" allow-list
  { cat "$bin"; printf 'x'; } >data/core4.bin
  refuse "$dev" "[near] (e) fe2.s.bin plus one byte at a path in allowed-binaries" "near:" data/core4.bin
  # (f) fe2.s compressed.
  gzip -c "$work/game/fe2.s" >data/listing.gz
  refuse "$dev" "[magic] (f) fe2.s gzipped" "magic: compressed (gzip)" data/listing.gz
  for c in bzip2 xz zstd; do
    if command -v "$c" >/dev/null 2>&1; then
      "$c" -c "$work/game/fe2.s" >"data/listing.$c" 2>/dev/null
      refuse "$dev" "[magic] fe2.s through $c" "magic: compressed ($c)" "data/listing.$c"
    else skip "[magic] fe2.s through $c: $c not installed"; fi
  done
  if command -v zip >/dev/null 2>&1; then
    (cd "$work/game" && zip -q "$dev/data/listing.zip" fe2.s)
    refuse "$dev" "[magic] fe2.s zipped" "magic: archive (zip)" data/listing.zip
  else skip "[magic] fe2.s zipped: zip not installed"; fi
  # The frontier binary renamed: exact, plus one byte, first byte changed.
  cp "$work/game/frontier" bin/game
  refuse "$dev" "[copy] frontier renamed bin/game" "copy:" bin/game
  { cat "$work/game/frontier"; printf 'x'; } >bin/game2
  refuse "$dev" "[magic] frontier plus one byte" "magic: executable (Mach-O)" bin/game2
  { printf 'X'; tail -c +2 "$work/game/frontier"; } >bin/game3
  refuse "$dev" "[near] frontier with its first byte changed" "near:" bin/game3
  # A 100-line fe2.s excerpt, plain and disguised (case, whitespace, CRLF, CR, NUL).
  sed -n '2001,2100p' "$work/game/fe2.s" >docs/notes.txt
  refuse "$dev" "[excerpt] fe2.s lines 2001-2100" "excerpt:" docs/notes.txt
  sed -n '2001,2100p' "$work/game/fe2.s" | tr 'a-z\t' 'A-Z ' | awk '{ gsub(/ /, "   "); printf "%s\r\n", $0 }' >docs/notes2.txt
  refuse "$dev" "[excerpt] the same, upper case, spaces widened, CRLF" "excerpt:" docs/notes2.txt
  sed -n '2001,2100p' "$work/game/fe2.s" | tr '\n' '\r' >docs/notes3.txt
  refuse "$dev" "[excerpt] the same with CR line ends" "excerpt:" docs/notes3.txt
  sed -n '2001,2100p' "$work/game/fe2.s" | tr '\n' '\0' >docs/notes4.bin
  refuse "$dev" "[excerpt] the same with NUL line ends" "excerpt:" docs/notes4.bin
  # fe2.s lines 2001-2099 split into 11 files of 9 lines in one commit: no file
  # reaches 10 lines, but together they pass 40.
  parts=
  for i in 1 2 3 4 5 6 7 8 9 10 11; do
    s=$((2001 + (i - 1) * 9))
    sed -n "${s},$((s + 8))p" "$work/game/fe2.s" >"split/p$i.txt"
    parts="$parts split/p$i.txt"
  done
  # shellcheck disable=SC2086
  refuse "$dev" "[excerpt-split] fe2.s split 11 x 9 lines in one commit" "a split excerpt" $parts
  # The same split, one file per commit: each commit is accepted until the
  # lines in the tree reach 40; that commit is refused.  Part 1 has NUL line
  # ends, so it is a binary file, and still counts towards the total.
  # tree_lines FILE: distinct counted lines that the files in the index plus
  # FILE share with the corpus (what the hook totals).
  tree_lines() {
    { git -C "$dev2" grep --cached -a -h -e ''; cat "$dev2/$1"; } | LC_ALL=C tr '\0\r' '\n\n' | LC_ALL=C awk '
      function norm(l) { gsub(/[ \t\r\f\v]+/, "", l); return tolower(l) }
      FNR == NR { l = norm($0); if (length(l) >= 8) c[l] = 1; next }
      { l = norm($0); if (length(l) >= 8 && (l in c)) h[l] = 1 }
      END { n = 0; for (l in h) n++; print n }' - "$dev2/fe2.s" "$dev2/fe2.s.c" "$dev2/fixups.h" "$dev2"/gen/*.c
  }
  dev2=$work/dev2
  new_repo "$dev2" local
  crossed=0
  for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
    s=$((3001 + (i - 1) * 9))
    if [ "$i" = 1 ]; then sed -n "${s},$((s + 8))p" "$work/game/fe2.s" | tr '\n' '\0' >"$dev2/p$i.txt"
    else sed -n "${s},$((s + 8))p" "$work/game/fe2.s" >"$dev2/p$i.txt"; fi
    n=$(tree_lines "p$i.txt")
    if [ "$n" -lt 40 ]; then
      accept "$dev2" "[excerpt-commits] part $i ($n lines in the tree)" keep "p$i.txt"
    else
      refuse "$dev2" "[excerpt-commits] part $i takes the tree to $n lines" "a split excerpt" "p$i.txt"
      crossed=$i; break
    fi
  done
  if [ "$crossed" -gt 1 ] && git -C "$dev2" grep --cached -I -q -e '' -- p1.txt; then
    fail "[excerpt-commits] part 1 should be a binary file"
  fi
  [ "$crossed" != 0 ] || fail "[excerpt-commits] the parts never reached 40 lines"
  # The parts already committed: the full check reports them once the part the
  # hook refused is committed without it.
  s=$((3001 + (crossed - 1) * 9))
  sed -n "${s},$((s + 8))p" "$work/game/fe2.s" >"$dev2/p$crossed.txt"
  git -C "$dev2" add -f "p$crossed.txt" && git -C "$dev2" commit -q --no-verify -m late
  out=$(cd "$dev2" && tools/check-no-game-data.sh 2>&1); rc=$?
  k=$(printf '%s\n' "$out" | grep -c 'a split excerpt')
  if [ "$rc" = 1 ] && [ "$k" -ge 2 ]; then pass "[excerpt-commits] full check refuses all $k committed parts"
  else fail "[excerpt-commits] full check rc=$rc, $k parts: $(printf '%s\n' "$out" | head -3)"; fi

  # Documented gaps: each is ACCEPTED (see "What the guard does not catch").
  sed -n '2001,2009p' "$work/game/fe2.s" >docs/gap1.txt
  accept "$dev" "[gap-threshold] a 9-line fe2.s excerpt" drop docs/gap1.txt
  sed -n '2001,2100p' "$work/game/fe2.s" | sed 's|^|// |' >docs/gap2.txt
  accept "$dev" "[gap-rewrite] 100 fe2.s lines, each behind a comment marker" drop docs/gap2.txt
  base64 <"$bin" >docs/gap3.txt
  accept "$dev" "[gap-encoding] fe2.s.bin as base64" drop docs/gap3.txt
  { head -c 65536 /dev/zero | tr '\0' 'h'; cat "$bin"; } >data/gap4.dat
  accept "$dev" "[gap-embedded] fe2.s.bin behind a 64 KB header" drop data/gap4.dat
  { head -c 4096 /dev/zero | tr '\0' 'h'; cat "$bin"; } >data/core5.bin
  refuse "$dev" "[near] fe2.s.bin behind a 4 KB header (within 1% of its size)" "near:" data/core5.bin
  tail -c +45 "$wav" >assets/gap5.raw
  accept "$dev" "[gap-raw-audio] a sound effect without its RIFF header" drop assets/gap5.raw
  if command -v compress >/dev/null 2>&1; then
    compress -c "$work/game/fe2.s" >data/gap6.Z
    accept "$dev" "[gap-unknown-compressor] fe2.s through compress(1), which has no magic rule" drop data/gap6.Z
  else skip "[gap-unknown-compressor] compress not installed"; fi
  printf 'data/gap7.gz\n' >>.githooks/allowed-binaries
  git add .githooks/allowed-binaries && commit_hook "$dev" allow-list-2
  gzip -c "$work/game/fe2.s" >data/gap7.gz
  accept "$dev" "[gap-allowed-contents] fe2.s gzipped at a path in allowed-binaries" drop data/gap7.gz
  # gen/*.c files with fewer than 10 counted lines, and a modified copy of the
  # smallest of them.
  small=$(LC_ALL=C awk 'function norm(l) { gsub(/[ \t\r\f\v]+/, "", l); return tolower(l) }
    FNR == 1 { if (f != "" && k < 10) print k, f; f = FILENAME; k = 0; delete s }
    { l = norm($0); if (length(l) >= 8 && !(l in s)) { s[l] = 1; k++ } }
    END { if (f != "" && k < 10) print k, f }' "$work"/game/gen/*.c | sort -n)
  ngen=$(ls "$work"/game/gen/*.c | wc -l | tr -d ' ')
  nsmall=$(printf '%s\n' "$small" | grep -c .)
  if [ "$nsmall" -gt 0 ]; then
    first=$(printf '%s\n' "$small" | head -1 | cut -d' ' -f2-)
    { cat "$first"; printf 'int gamedata_test_extra;\n'; } >src/small.c
    accept "$dev" "[gap-small-gen] $nsmall of $ngen gen/*.c files have under 10 counted lines; the smallest plus one line" drop src/small.c
  else pass "[gap-small-gen] no gen/*.c file of the $ngen has under 10 counted lines"; fi
  # This repository's tracked files share some lines with the corpus.
  shared=$( { git -C "$top" grep --cached -a -h -e ''; } | LC_ALL=C tr '\0\r' '\n\n' | LC_ALL=C awk '
      function norm(l) { gsub(/[ \t\r\f\v]+/, "", l); return tolower(l) }
      FNR == NR { l = norm($0); if (length(l) >= 8) c[l] = 1; next }
      { l = norm($0); if (length(l) >= 8 && (l in c)) h[l] = 1 }
      END { n = 0; for (l in h) n++; print n }' - "$work/game/fe2.s" "$work/game/fe2.s.c" "$work/game/fixups.h" "$work"/game/gen/*.c)
  if [ "$shared" -lt 40 ]; then pass "[shared-lines] this repository's tracked files share $shared lines with the corpus (< 40)"
  else fail "[shared-lines] this repository's tracked files share $shared lines with the corpus (>= 40)"; fi

  # Speed: a 200 KB benign C file through the hook, and the guard alone.
  awk 'BEGIN { print "static const unsigned int gamedata_test_table[] = {";
               for (i = 0; i < 6700; i++) printf "\t0x%08x, /* entry %d */\n", i * 2654435761 % 4294967296, i;
               print "};" }' >src/table.c
  git add -f src/table.c
  t_guard=$( { TIMEFORMAT=%R; time tools/check-no-game-data.sh --staged >/dev/null 2>&1; } 2>&1 )
  t_commit=$( { TIMEFORMAT=%R; time git -c core.hooksPath=.githooks commit -q -m table >/dev/null 2>&1; } 2>&1 )
  if git log -1 --format=%s | grep -qx table; then
    pass "[speed] 200 KB C file ($(wc -c <src/table.c | tr -d ' ') bytes): guard --staged ${t_guard}s, whole commit ${t_commit}s"
  else fail "[speed] the 200 KB benign C file was refused"; fi

  # CI: no local game files; only the name, hash, image, magic and oracle rules run.
  ci=$work/ci
  new_repo "$ci"
  awk '{ printf "%s\r\n", $0 }' "$work/game/fe2.s" >"$ci/listing.txt"
  refuse "$ci" "[hash] fe2.s with CRLF line ends, no local files" "hash:" listing.txt
  awk '{ printf "%s\r\n", $0 }' "$work/game/fe2.s" >"$ci/listing.txt"
  git -C "$ci" add -f listing.txt && git -C "$ci" commit -q --no-verify -m bypass
  out=$(cd "$ci" && tools/check-no-game-data.sh --rev HEAD 2>&1); rc=$?
  if [ "$rc" = 1 ] && printf '%s\n' "$out" | grep -qF 'REFUSED listing.txt (hash:'; then
    pass "[hash-rev] --rev HEAD refuses a commit made with --no-verify"
  else fail "[hash-rev] rc=$rc: $out"; fi
  git -C "$ci" reset -q --hard HEAD~1
  { cat "$work/game/fe2.s.c"; printf 'int gamedata_test_extra;\n'; } >"$ci/core.c"
  accept "$ci" "[gap-ci] fe2.s.c plus one line where no game files are present" drop core.c
fi

cd "$work" || exit 2
echo "gamedata: $passed passed, $failed failed, $skipped skipped"
[ "$failed" -eq 0 ]
