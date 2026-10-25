# Git hooks

Install once per clone: `git config core.hooksPath .githooks`

- `pre-commit` runs `tools/check-no-game-data.sh --staged` and refuses any commit
  that stages fe2.s, anything derived from it (fe2.s.c, fe2.s.bin, fixups.h, gen/,
  the frontier binary), game assets (sfx/, music/, savs/, *.wav, *.ogg) or
  captures (*.fstr, captures/, oracle-out/).  Names match without regard to case.
  It also refuses any executable, any copy of a local game file under another
  name, a file sharing 10+ lines with a local fe2.s, and any image not listed
  in `allowed-images` (screenshots show game screens).
- CI runs the same script over the whole tree: `bash tools/check-no-game-data.sh --rev HEAD`.
