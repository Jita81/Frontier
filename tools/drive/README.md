# tools/drive — scripted input for an unmodified Frontier build

`drive.py` starts a built `frontier` with `libfe2drive.dylib` injected. It then
plays a `.drv` script of key presses, mouse moves and waits, and takes a
screenshot of the game window at each `shot` step. The game and its 68000
core are not changed or rebuilt; the library only interposes `SDL_PollEvent`.

This is a test convenience, not an oracle. Steps are timed by the host clock
on a second thread, and a shot is taken while the game keeps running, so two
runs agree on what is on screen, not on frame counts or exact game time. For
a deterministic check of the core, use the replay tapes in `tools/oracle`.

    make -C tools/drive                  # builds libfe2drive.dylib
    make -C tools/drive check-layout     # prints "layout: PASS"
    python3 tools/drive/drive.py --bin ./frontier \
        --script tools/drive/scripts/start-docked.drv --out /tmp/run-a --timeout 60
    python3 tools/drive/drive.py --compare /tmp/run-a /tmp/run-b

Needs macOS, `cc`, `python3` and `ffmpeg`. `check-layout` also needs
`sdl-config`. The terminal that runs `drive.py` needs the Screen Recording
permission; without it every shot is black, and the manifest says so.

## Game screens are never committed

Shots are pictures of the game. Keep run directories outside the repository,
or under `tools/drive/out/`, which is ignored. The pre-commit guard
(`tools/check-no-game-data.sh`) refuses any image not listed in
`.githooks/allowed-images`; never list a shot there.

## Running

The game runs with its own directory as the working directory, so it reads
and writes that directory's `savs/`. `fi07-save.drv` leaves a save named
`drvsave` there.

| Option | Default | Meaning |
|---|---|---|
| `--bin` | `./frontier` at the repo root | game binary |
| `--script` | | the `.drv` script |
| `--out` | | run directory, created if missing |
| `--timeout` | 120 | wall-clock limit in seconds; then SIGTERM, then SIGKILL, to the game's PID only |
| `--args` | | extra game arguments, one string |
| `--pos` | `1300,640` | `SDL_VIDEO_WINDOW_POS` |
| `--quiet` | | do not echo the game log |
| `--lib` | `libfe2drive.dylib` here | the driver library |

Audio is off (`SDL_AUDIODRIVER=dummy`). Keyboard and mouse events from the
desktop are dropped while a script runs, so touching the window does not move
the game's pointer.

The run directory holds:

- one `<name>.png` per `shot` step, scaled to the window's point size
  (640x480 by default) and kept under 150 KB;
- `game.log`: the game's stdout and stderr, including the driver's
  `drive: [seconds] ...` line for every step;
- `manifest.json`: the binary, script and library hashes, the environment,
  start and end times, the exit code, and per shot its file, hash, size and
  mean/max brightness (an all-black shot gets a `warning`).

`drive.py` exits with the game's exit code, 124 if the timeout fired, or 125
if any shot failed. The game exits with the driver's code when the driver
stops it:

| Code | Cause |
|---|---|
| 3 | script error: the log names the line and why |
| 4 | the binary does not export a symbol the driver needs |
| 6 | the screenshot service failed or gave no answer within 60 s |
| 7 | the game was still running 10 s after `quit` |

A script that ends without `quit` leaves the game running, so the run ends
at the timeout with 124.

`--compare RUN_A RUN_B` computes the SSIM of each pair of same-named shots
and exits 0 only if both runs have the same shot names and the worst pair is
at or above `--ssim` (0.90). `--ikbd RUN_DIR`
lists the ST scancodes that each `sdlkey` step produced, from `game.log`.

## Script grammar

One step per line. `#` starts a comment unless it is inside double quotes.
Blank lines are skipped, and a step has at most 8 words. A double-quoted
string is one word and may use `\n` and `\t`.

| Step | Meaning |
|---|---|
| `wait <ms>` | sleep |
| `key <st> [hold_ms]` | press an ST key, hold it (100 ms), release it |
| `keydown <st>` / `keyup <st>` | press or release only |
| `text "<ascii>"` | type with US layout; shifted characters hold left Shift; `\n` is Return and `\t` is Tab; 60 ms per press and 60 ms between keys |
| `mouse <x> <y>` | move the pointer to ST screen coordinates, x 0–319 and y 0–199, whatever the window size |
| `click [left\|right] [hold_ms]` | press and release a mouse button (left, 100 ms) |
| `sdlkey <name> [modifier...] [hold_ms]` | send an SDL key event through the port's keymap, as a Mac keyboard would; modifiers go down first and up last, 30 ms apart |
| `shot <name>` | screenshot to `<name>.png`; names use `[A-Za-z0-9._-]`; the script waits until the file is written |
| `log <text>` | write a note to `game.log` |
| `quit` | post `SDL_QUIT`; the game should exit 0 |

`key`, `keydown`, `keyup` and `text` hand an ST scancode straight to the
game's `Input_PressSTKey`, skipping the port's keymap. `sdlkey` goes through
the keymap, so it is the step for testing the keymap itself.

**ST key names** (the Atari ST IKBD set), or a scancode as `0x01`–`0x7f`:
`esc`, `0`–`9`, `a`–`z`, `minus`, `equals`, `backspace`, `tab`, `lbracket`,
`rbracket`, `return`, `control`, `semicolon`, `apostrophe`, `grave`,
`lshift`, `rshift`, `backslash`, `comma`, `period`, `slash`, `alternate`,
`space`, `capslock`, `f1`–`f10`, `clrhome`, `up`, `down`, `left`, `right`,
`insert`, `delete`, `iso`, `undo`, `help`, `kp0`–`kp9`, `kp_minus`,
`kp_plus`, `kp_lparen`, `kp_rparen`, `kp_slash`, `kp_star`, `kp_period`,
`kp_enter`.

**SDL key names**: `a`–`z`, `0`–`9`, `minus`, `equals`, `lbracket`,
`rbracket`, `return`, `apostrophe`, `semicolon`, `backslash`, `comma`,
`slash`, `period`, `tab`, `space`, `grave`, `backspace`, `esc`, `less`,
`f1`–`f15`, `up`, `down`, `left`, `right`, `insert`, `home`, `end`,
`pageup`, `pagedown`, `delete`, `kp0`–`kp9`, `kp_period`, `kp_divide`,
`kp_multiply`, `kp_minus`, `kp_plus`, `kp_enter`, `kp_equals`, `clear`,
`help`, `undo`, `numlock`, `capslock`, `lshift`, `rshift`, `lctrl`, `rctrl`,
`lalt`, `ralt`, `lmeta`, `rmeta`, `mode`.

**Modifiers** for `sdlkey`: `shift`, `rshift`, `ctrl`, `rctrl`, `alt` or
`option`, `ralt`, `cmd` or `lcmd`, `rcmd`, `mode`, and the lock states `num`
and `caps` (these set the flag without pressing a key).

## Scripts

Each script's header says what it covers (`Verifies:`), how to run it
(`Run:`) and what the shots should show (`Expect:`). Check the shots against
`Expect:`: exit 0 only means the script ran.

| Script | Covers | Shots |
|---|---|---|
| `start-docked.drv` | intro, start menu, a docked start | 3 |
| `fi07-save.drv` | the save dialog from Preferences; writes `savs/drvsave` | 3 |
| `fi12-main-keys.drv` | Space, digits, F1–F4 and Esc on the main screens | 11 |
| `fi15-fi16-stardreamer-pause.drv` | time acceleration 1x–10000x, pause and resume, read from the panel clock | 10 |
| `s19-menu-timeout.drv` | the start menu returns to the intro when left alone | 15 |

The time-acceleration script reads the clock 3 s after each key. A key that
lands while 10000x is still running can move the clock by minutes, so judge
each speed by whether the clock changes and roughly by how much, not by
exact numbers.

## Keeping the driver in step with the game

The driver needs these symbols from the binary: `Input_PressSTKey`, `input`,
`screen_w` and `screen_h`, plus `ShortCutKey` if present (it is only logged).
`drive.c` also hard-codes the SDL 1.2 event layout and the offsets of the
`INPUT` struct in `src/input.h`. After changing `src/input.h` or the SDL
headers, run `make -C tools/drive check-layout`.
