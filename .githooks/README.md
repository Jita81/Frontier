# Git hooks: the game-data guard

fe2.s, everything built from it and the game's assets are copyright Frontier
Developments.  They must never be committed.  `tools/check-no-game-data.sh`
refuses them.  The `pre-commit` hook in this directory runs it on what you
stage.

**After cloning, run once:**

    make -f Makefile-C install-hooks

This sets `core.hooksPath=.githooks` for this repository only, never in your
global configuration `[install-hooks]`.  In a linked worktree (`git worktree
add`) it writes the configuration that all worktrees of the repository share
`[install-hooks-worktree]`.

Every statement below is checked by `tests/gamedata/run.sh`; the tag in
brackets names the test case.  Run that test after changing the guard.

## Running the guard

| Command | Checks | Used by |
|---|---|---|
| `tools/check-no-game-data.sh --staged` | files added or changed in the index | the pre-commit hook |
| `tools/check-no-game-data.sh` | every file in the index | you, before pushing |
| `tools/check-no-game-data.sh --rev REV` | every file in commit REV | CI: `--rev HEAD` `[hash-rev]` |

It exits 0 when clean, 1 when it refuses something (each refusal names the
path and the rule), and 2 on a bad argument, a bad revision or outside a git
work tree `[usage]`.

## What it refuses

Each file is refused by the first rule it meets.

| Rule | Refuses | Tests |
|---|---|---|
| name | A path named `fe2.s`, `fe2.s.*`, `fixups.h` or `frontier`; a path through a `gen/`, `sfx/`, `savs/`, `music/`, `captures/` or `oracle-out/` directory; any `*.fstr`, `*.wav`, `*.ogg` or `*.sav` file; a path containing a newline.  Case is ignored (`FE2.S` and `Gen/` are refused). | `[name]`, `[name-newline]` |
| copy | A file identical to a local game file under any name: fe2.s, fe2.s.c, fe2.s.bin, fixups.h, frontier, `san/*/frontier`, `gen/*`, every file under sfx/, music/, savs/, captures/ and oracle-out/ (symbolic links followed), and every `*.wav`, `*.ogg`, `*.sav` or `*.fstr` file in the work tree. | `[copy]` |
| hash | A file over 64 KB whose SHA-256, CRs removed, is that of a known fe2.s or is listed in `tools/oracle/gamedata.sha256`.  Needs no local game files. | `[hash]`, `[hash-rev]` |
| image | A raster image, by extension or by PNG, JPEG, GIF or TIFF magic bytes, whose path is not listed in `allowed-images`.  Screenshots show game screens. | `[image]` |
| magic | A file that starts like an executable (Mach-O, ELF, PE, Atari ST), audio (RIFF/WAVE, OggS) or compressed or archive data (gzip, zip, bzip2, xz, zstd, 7z), unless its path is listed in `allowed-binaries`.  Compressed data is refused, not unpacked. | `[magic]` |
| oracle | A file over 1 MB under `tools/oracle/`. | `[oracle]` |
| near | A file within 1% of the size of the local fe2.s.bin, frontier or `san/*/frontier` that matches it at 98% or more of byte positions, or in 98% or more of its bytes counted by runs between NUL, CR and LF bytes.  The second test means one inserted byte, or a 4 KB header, does not hide a copy. | `[near]` |
| excerpt | A file that shares 10 or more distinct lines with the local fe2.s, fe2.s.c, fixups.h or `gen/*.c`.  Lines are split at LF, CR and NUL, then compared with all whitespace removed and case ignored.  A line counts only if 8 or more characters remain. | `[excerpt]` |
| excerpt (split) | Every file sharing any such line, once 40 or more are shared in all.  The total covers the files checked and, with `--staged`, every file already in the index, binary files included.  So an excerpt split across files or commits is refused by the commit that takes the tree to 40 lines.  The full check (no arguments) then also names the parts committed before. | `[excerpt-split]`, `[excerpt-commits]` |

The copy, near and excerpt rules need the local game files, so they do nothing
where those are absent.

The allow-lists, `allowed-images` and `allowed-binaries`, hold one repository
path per line.  The match is exact and case-sensitive.  A listed path is
exempt from the image or magic rule only; the other rules still apply
`[allow-binaries]`, `[near]`.  The guard reads the allow-lists from the index
(from REV with `--rev`), so an edit to them that is not staged exempts
nothing `[allow-unstaged]`.

## What it accepts

The test commits these through the hook, with the game files present, and
they must be accepted: a hand-written C file, the listed icon `cursor.png`, a
Markdown document and this repository's `tests/` directory `[benign]`.  Lines
such as an include of `host.h` also occur in the generated C.  This
repository's tracked files share a few such lines with the game files (12
when this was written); the test prints the count and fails at 40
`[shared-lines]`.

Speed: with the game files present, a 200 KB C file takes about one second
through the guard `[speed]`.  Most of that is reading the corpus once.

## What it does not catch

Each of these is committed by the test and is **accepted** today `[gap-*]`:

- Content changed beyond whitespace and case, such as each line prefixed with
  a comment marker `[gap-rewrite]`, or encoded, such as base64 or hex
  `[gap-encoding]`.
- An excerpt below the thresholds: up to 9 lines in a file `[gap-threshold]`,
  or fewer than 40 lines in the whole tree.
- A game binary embedded in a larger file, or cut down, so that its size
  differs by more than 1% and no magic rule applies `[gap-embedded]`.
- Audio without its RIFF or OggS header, unless an exact copy
  `[gap-raw-audio]`.
- Anything the copy, near and excerpt rules would catch, when the local game
  files are absent, as in CI `[gap-ci]`.
- A few `gen/*.c` files have fewer than 10 lines that count (5 of 535 when
  this was written; the test prints the count).  A modified copy of one of
  them passes the excerpt rule `[gap-small-gen]`.
- Data from a compressor, archiver or cipher it has no magic for, such as
  `compress` `[gap-unknown-compressor]`.
- Compressed data at a path in `allowed-binaries`: its contents are not
  inspected `[gap-allowed-contents]`.
- A path added to an allow-list in the same commit as the file
  `[gap-allow-same-commit]`.  Review changes to `allowed-images` and
  `allowed-binaries`.

And it cannot stop `git commit --no-verify`, or a commit in a clone where
`install-hooks` was never run.  The full check, or `--rev HEAD` in CI, finds
what such a commit added `[hash-rev]`, `[excerpt-commits]`.
