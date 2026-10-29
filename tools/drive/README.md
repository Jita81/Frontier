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
  and kept under 150 KB.  The window is 640x480: the game would open as
  large as the desktop allows, so drive.py passes `--size 640 480` unless
  `--args` sets `--size`;
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
| `press [left\|right]` / `release [left\|right]` | press or release only (left); a button held across steps, e.g. the right button that steers in flight |
| `move <dx> <dy> [count] [every_ms]` | relative mouse motion, as the game reads from a real mouse: add `dx`,`dy` (each -100 to 100) to the motion the game has not read yet, `count` times (1), `every_ms` apart (20) |
| `sdlkey <name> [modifier...] [hold_ms]` | send an SDL key event through the port's keymap, as a Mac keyboard would; modifiers go down first and up last, 30 ms apart |
| `shot <name>` | screenshot to `<name>.png`; names use `[A-Za-z0-9._-]`; the script waits until the file is written |
| `log <text>` | write a note to `game.log` |
| `quit` | post `SDL_QUIT`; the game should exit 0 |

`mouse` sets the pointer's position; `move` does not move the pointer, it
only feeds the relative motion (`INPUT.motion_x`/`motion_y`) that the game
turns into steering while the right button is held. The game drops the
whole pending motion when it exceeds 100 in either axis before it is read
(`Call_GetMouseInput` in `src/input.c`), so prefer several small steps a
frame or more apart over one large one. Steering in flight, after a launch:

    press right          # hold the right button: the mouse steers
    move 8 0 50 20       # 50 steps of +8 in x, 20 ms apart: yaw for 1 s
    move 0 -8 50 20      # then pitch
    release right
    mouse 8 193          # the view icon
    press left           # press ... release is a click with a chosen hold
    wait 200
    release left

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
| `mouse-steer.drv` | icon clicks to launch, steering with `press right` + `move`, `press`/`release left` on the view icon | 7 |

The time-acceleration script reads the clock 3 s after each key. A key that
lands while 10000x is still running can move the clock by minutes, so judge
each speed by whether the clock changes and roughly by how much, not by
exact numbers.

## The Phase 2 capture tour: `tools/p2shots`

Every Phase 2 item is accepted against the same frames: the `p2-*.drv`
routes, played on the base build, the branch build and, when a flag keeps
the old look, the branch with that flag.

    make -C tools/drive
    tools/p2shots --base /path/to/base/frontier --branch ./frontier \
        --flags=--flat-hulls --out /path/outside/the/repo/tour
    tools/p2shots --repeat --branch ./frontier          # two runs of one build
    tools/p2shots --routes dock,orbit --base B --branch ./frontier

Each run gets its own stage directory, which holds links to the binary and its
data, with no `frontier.ini` and no saves, so every route starts a new game
from the defaults. The output directory, which defaults to the ignored
`tools/drive/out/p2shots/<time>/`, holds:

- `<route>/<shot>-<column>.jpg`, one per shot and column;
- `<route>-sheet.jpg`, one row per shot and one column per run;
- `report.json`, with the binaries' hashes, each run's status and each
  shot's difference.

For each shot, the table compares every column with the first column.
`SAME` means the decoded pixels below the title bar are identical. `mean` is
the mean grey difference (0-255). `chg` is the share of pixels that differ by
more than 16, and `view` is that share between the title bar and the console. `--repeat` turns that table into
the noise floor. Read a base-against-branch difference against that floor
before you call it a change. The landed shots repeat bit for bit, and the
docked page differs only in the clock. Flight shots do not repeat exactly:
the driver's waits are wall-clock time, so each input lands a few game frames
earlier or later, and a ship or the view sits slightly elsewhere. In the
traffic route this is under 1% of pixels; in the dock route, with the
autopilot turning the ship, it is up to about 9%. One run plays at a time (`--jobs 2` runs two).
A run that fails or loses a shot is played once more. `screencapture` now and
then cannot grab the window on a busy host, so p2shots puts a retrying
wrapper first on `PATH`. A base-against-branch tour of the four routes takes
about 8 minutes with `--jobs 2` and about 17 minutes one run at a time.

| Route | Start | Covers | Shots |
|---|---|---|---|
| `p2-landed.drv` | 2 | landed at an atmospheric port: the cockpit, then the next two views | 3 |
| `p2-orbit.drv` | 3 | buys an autopilot, flies to the start station's world (a world with sea), then three views from where the autopilot stops | 5 |
| `p2-dock.drv` | 3 | buys an autopilot, launches, targets the station, then the approach and the docked page | 5 |
| `p2-traffic.drv` | 3 | launches with no purchase, then the station and nearby ships in the rear and external views | 4 |

Not covered yet: no route with fixed keystrokes reaches these.

- A gas giant, an airless moon, an icy world and the star close up need the
  start-2 system. Start 2 can fit an autopilot (sell the laser, then fit it),
  but the flight out ended in the ship's loss at both 10000x and 1000x.
- A descent to a surface port and an airless port: the start-3 world has no
  surface port on its map.
- A close kill: the police come, but hitting one needs aiming, which a fixed
  route cannot do.

**Adding a route.** Write `tools/drive/scripts/p2-<name>.drv` with the usual
`Verifies:`, `Run:` and `Expect:` header. Start with `key space` and a start
position (`key 1`-`key 3`), and use in-game navigation only: no saves and no
`frontier.ini`. Hold every shot frame with the game's pause key, parking the
pointer off the controls first so no tip is drawn, then resuming at 1x:

    mouse 2 120
    wait 300
    sdlkey esc
    wait 500
    shot 03-orbit
    sdlkey f1 shift

Name shots `NN-what`. A later Phase 2 item adds a route and touches nothing
else. Routes and docs use keystrokes and generic descriptions only, such as
"the start station" or "a world with sea", never a place, ship or person
name. Shots are never committed. Keep `--out` outside the repository, or
under the ignored `tools/drive/out/`.

## Keeping the driver in step with the game

The driver needs these symbols from the binary: `Input_PressSTKey`, `input`,
`screen_w` and `screen_h`, plus `ShortCutKey` if present (it is only logged).
`drive.c` also hard-codes the SDL 1.2 event layout and the offsets of the
`INPUT` struct in `src/input.h`. After changing `src/input.h` or the SDL
headers, run `make -C tools/drive check-layout`.
