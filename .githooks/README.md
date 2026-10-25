# Git hooks

Install once per clone: `git config core.hooksPath .githooks`

- `pre-commit` runs `tools/check-no-game-data.sh --staged` and refuses any commit
  that stages fe2.s, anything derived from it (fe2.s.c, fe2.s.bin, fixups.h, gen/,
  the frontier binary), game assets (sfx/, music/, savs/, *.wav, *.ogg) or
  captures (*.fstr, captures/, oracle-out/).
- CI runs the same script over the whole tree: `bash tools/check-no-game-data.sh --rev HEAD`.
