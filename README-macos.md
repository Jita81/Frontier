# Frontier.app: a double-clickable macOS app

`tools/package-macos/package.sh` builds the game and wraps it in
`dist/Frontier.app`. This changes nothing in `src/` or `Makefile-C`: the app
is a host-side wrapper, and the game's 68000 core is untouched.

The app is **Apple silicon only** and needs **macOS 26 or later**. The
minimum is read from the libraries Homebrew supplies (all are built for 26.0);
`package.sh` writes the highest value into `LSMinimumSystemVersion`.

## Do not redistribute the app

The `frontier` binary inside the app *is* the translation of `fe2.s` to C. It
is a derivative of that copyrighted file, even though the app carries none of
the game's data files. Keep `dist/Frontier.app` on the machine that built it.
`dist/` is git-ignored, and nothing copyrighted is committed. Do not upload
the app, attach it to a release, or put it in a disk image for others.

## Build it

You need what the normal build needs (see `README`): Homebrew `sdl12-compat`,
`libvorbis` and `pkg-config`, the Xcode command line tools, and your own
`fe2.s` in the repository root.

```sh
tools/package-macos/package.sh
```

That runs `make -f Makefile-C -j4` and then assembles the bundle. The result
is `dist/Frontier.app` (about 7 MB). Options:

| option | effect |
|---|---|
| `--no-build` | use the `./frontier` and `./fe2.s.bin` already built |
| `--install-data [DIR]` | also copy `fe2.s.bin`, `sfx/` and `music/` from this checkout into `DIR` (default: the app's data folder, below) |
| `JOBS=N` (environment) | `make -j` value; the default is 4 |
| `BUNDLE_ID=...` (environment) | `CFBundleIdentifier`; the default is `local.glfrontier.macos` |

What `package.sh` does, in order:

1. Builds the game (skipped with `--no-build`).
2. Copies the binary to `Contents/MacOS/frontier-bin`. The launcher script is
   `Contents/MacOS/Frontier`; the binary needs another name because APFS
   ignores case.
3. Copies the Homebrew dylibs the binary needs into `Contents/Frameworks` and
   re-points every reference with `install_name_tool` to `@rpath/...`. The
   binary finds `Frameworks/` through `@executable_path/../Frameworks`; each
   library finds its neighbours through `@loader_path`. Homebrew rpaths are
   removed. Six libraries end up there: `libSDL-1.2.0`, `libSDL2-2.0.0`,
   `libSDL3`, `libvorbis`, `libvorbisfile` and `libogg`. The two newer SDL
   libraries are not in `otool -L`: `sdl12-compat` loads `sdl2-compat`, and
   that loads SDL3, with `dlopen` at run time. Both look next to themselves
   first, so `package.sh` puts them there (`libSDL3.0.dylib` is copied as
   `libSDL3.dylib`, the name `sdl2-compat` asks for).
4. Draws a neutral icon with `tools/package-macos/make_icon.py` (plain Python,
   no game art: a dark tile with a ringed planet) and turns it into an
   `.icns` with `sips` and `iconutil`.
5. Fills in `Info.plist` from `tools/package-macos/Info.plist.in` (version from
   `git describe`) and checks it with `plutil -lint`.
6. Fails if any `/opt/homebrew` or `/usr/local` path is left in the binary or
   a library, in a load command or an rpath.
7. Signs every library and the binary ad hoc, then the bundle
   (`codesign --force --deep -s -`), then runs `codesign --verify --deep --strict`.

## Put the game data where the app looks for it

The app does **not** contain the game's data. It looks in:

```
~/Library/Application Support/Frontier
```

| file | needed? | what it is |
|---|---|---|
| `fe2.s.bin` | required | the game's data, made from your `fe2.s` when the game is built (the build leaves it in the repository root) |
| `sfx/` | optional | sound effects (`sfx_00.wav` ... `sfx_32.wav`); without it the game is silent |
| `music/` | optional | music tracks (`NN.ogg`); without it there is no music |
| `savs/` | created by the app | where the game saves |

The easiest way on the build machine:

```sh
tools/package-macos/package.sh --no-build --install-data
```

To use another folder, pass `--data-dir`, or set `FRONTIER_DATA_DIR`:

```sh
open dist/Frontier.app --args --data-dir /path/to/folder
FRONTIER_DATA_DIR=/path/to/folder dist/Frontier.app/Contents/MacOS/Frontier
```

Any other argument goes to the game unchanged (`--old-renderer`, `--size w h`,
`--nosound`, `--fullscreen`; `--help` prints the launcher's and the game's
options).

The window opens at the original's 4:3 shape, as large as fits in 85% of the
desktop (1600x1200 on a 3440x1440 screen), drawn at that resolution with 4x
multisampling.  `--size w h` picks another size.  The ship's panel and the
text are still the original 320x200 picture, scaled up.

How the launcher works, so nothing in `src/` has to change: the game opens
every file relative to the current directory, and a Finder launch starts in
`/`. The launcher therefore `cd`s to the data folder and then runs the real
binary. Before that it:

- shows a dialog (`osascript`) and exits with status 0 if `fe2.s.bin` is
  missing; the dialog says what to copy where and has an **Open Data Folder**
  button;
- checks `fe2.s.bin` against the SHA-256 recorded when the app was built, and
  shows a dialog and exits 0 if it differs (the compiled game and its data file
  are a matched pair, and a mismatch would abort inside the game without a
  message);
- creates `savs/` and copies `cursor.png` and `joystick.ini` from the app into
  the folder if they are not already there (your edits are kept).

## Run it

Double-click `dist/Frontier.app`, or:

```sh
open dist/Frontier.app
```

The game opens a window and plays its opening sequence. Closing the window
or pressing Cmd-Q goes through SDL's normal quit path; so far only a SIGTERM
has been exercised, and it ends the game with exit status 0 (see Limits).

A Finder launch has no console, so the game's messages go to
`~/Library/Logs/Frontier/frontier.log`. When you start the launcher from a
shell (or with `open --stderr FILE`), the messages go there instead and no log
file is written.

The window is not HiDPI on purpose. `Info.plist` sets `NSHighResolutionCapable`
to false, as for the bare `./frontier` binary: the renderer sizes its OpenGL
viewport and maps the mouse in pixels that equal window points. The bundled app
therefore draws exactly what `./frontier` draws, and macOS scales the window up
on a Retina display. Do not turn it on without changing and re-testing the
renderer.

## What is and is not signed

- Every Mach-O file and the bundle carry an **ad-hoc** signature
  (`codesign -s -`): no certificate, no Developer ID, no hardened runtime, no
  entitlements. `codesign --verify --deep --strict dist/Frontier.app` passes.
  Apple silicon will not run unsigned code, which is why the signature is there;
  it says nothing about who made the app.
- It is **not notarised** and has no stapled ticket. That needs a paid Apple
  Developer account, and was not attempted.
- An app built and run on the same Mac has no quarantine flag, so Gatekeeper does
  not stop it. A copy that arrives with a quarantine flag (a download, AirDrop,
  a browser) is blocked on first launch; control-click it and choose Open. As
  the app must not be redistributed, that is not the intended use.

## Check the bundle yourself

```sh
# no Homebrew path left in the binary or any library
for f in dist/Frontier.app/Contents/MacOS/frontier-bin dist/Frontier.app/Contents/Frameworks/*.dylib; do
  otool -L "$f" | tail -n +2 | grep -E '/opt/homebrew|/usr/local'
done        # prints nothing

# the signature
codesign --verify --deep --strict --verbose=2 dist/Frontier.app

# run with reads of /opt/homebrew and /usr/local denied: this works, while
# the unbundled ./frontier dies at load time under the same profile
sandbox-exec -p '(version 1)(allow default)(deny file-read* (subpath "/opt/homebrew") (subpath "/usr/local"))' \
  dist/Frontier.app/Contents/MacOS/Frontier
```

## Limits

- Apple silicon and macOS 26 or later only. An Intel Mac, or an older macOS,
  needs a different build of the libraries.
- The libraries are copied from Homebrew as they are today. After
  `brew upgrade` of SDL or vorbis, run `package.sh` again.
- Not tested: a clean Mac that has never had Homebrew (the sandbox check above
  stands in for it), joystick use from the app, quitting with the close button
  or Cmd-Q (SIGTERM was tested), and any other macOS version.
