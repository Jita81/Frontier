# EXP-02: Can sound effects and music be replaced from the host side?

## Question

Which game events trigger which sound effects, and which situations trigger which music track? Can one effect and one track be swapped for new audio from outside `fe2.s`, behind a default-off flag, while the game sounds byte-identical with the flag off?

## Method

1. **Map the calls statically.** All sound requests in `fe2.s` go through three wrappers reached from the A5 jump table:
   - `PlayEffect`: `hcall` 0x12 `PlaySFX`, with d0 = sample and d1 = channel.
   - `StartMusic`: `hcall` 0x1d `PlayMusic`, with d0 = mode and d1:d2 = enabled-track mask.
   - `StopMusic`: `hcall` 0x1e.

   I listed every call site of the three with its enclosing label and the immediate sample or channel loaded just before it: 64 sites, in [`evidence/E02/sound-calls-static.txt`](evidence/E02/sound-calls-static.txt). Only labels and ids are listed; no `fe2.s` text is reproduced.
2. **Map the calls dynamically.**
   - The E1 trace harness gained `FE2_TRACE_ONLY=12,1d,1e,...`, a hex list that limits the trace to those host calls.
   - Four scripted runs exercised these events: the intro, starting the game, the F-keys, the cockpit icons, launching without clearance, laser fire, a contact click, the undercarriage and the galactic map.
   - The scripts carry `mark` lines, so each sound can be tied to the action that caused it: [`evidence/E02/sound-events-dynamic.txt`](evidence/E02/sound-events-dynamic.txt).
3. **Name the music tracks.** The track names come from the game's own Options screen ("[fe2.s:49293]"). I then read where each `StartMusic` site gets its mode, and which option variable guards it.
4. **Add the hook** (`src/audio.c`, about 40 lines), with two flags:
   - `FE2_AUDIO_DIR=<dir>`: when `<dir>/sfx_NN.wav` or `<dir>/NN.ogg` exists, it is loaded in place of `sfx/sfx_NN.wav` or `music/NN.ogg`. Any file that is not there falls back to the original, so a pack can replace one sound or all of them.
   - `FE2_AUDIO_CAPTURE=<file>` is for measurement only. It appends every mixed audio buffer to `<file>` as raw 16-bit stereo, and prints a djb2 hash of every sample after it is loaded and converted.
5. **Make new audio.** [`tools/e02_gen_audio.py`](tools/e02_gen_audio.py) synthesises two files from sine maths, with no third-party input:
   - `sfx_00.wav`: a 90 ms rising chirp, replacing sample 0, the UI click.
   - `01.wav`, converted to `01.ogg` with ffmpeg's built-in vorbis encoder: a 10 s A-minor arpeggio, replacing track 1, the main theme played on the title screen.

   Neither file is committed; the script regenerates them.
6. **Identity and effect.** The same script ([`evidence/E02/capture-script.txt`](evidence/E02/capture-script.txt)) was run four times:

   | Run | Binary | Flags |
   |---|---|---|
   | A | Pre-E2 binary (b38e56f) | none |
   | B | E2 binary | none |
   | C | E2 binary | capture only |
   | D | E2 binary | `FE2_AUDIO_DIR` plus capture |

   I compared the sound-call sequence, the shown frames, the game's screen buffers and the full 866,823-line host-call trace. Separately, a Python decoder applied the same U8→S16 rule to the original WAVs, and its hash of each sample was compared with the hashes the binary printed. Finally I searched each capture for bit-exact copies of the old and new click.

## Result

**PROVED.** Every sound reaches the host as a sample or track id, with no audio data in `fe2.s`. A default-off flag, `FE2_AUDIO_DIR`, replaced sample 0 and music track 1 with self-generated audio, and the new click appears bit-exact in the mixed output where the original was. With the flag off, the binary loads the same 33 samples bit for bit, issues the identical 66-call sound sequence, and produces identical frames.

### Sound effects (sample id → event)

Rows marked "seen" were triggered in a scripted run. The rest are static: the id is loaded next to that call site, and the event is inferred from the routine.

| id (hex) | ch | Event | Evidence |
|---|---|---|---|
| 0 | 0 | UI click: an F-key or a cockpit icon | seen at every F1/F2/icon press |
| 1 | 1 | Laser fire, first entry of the laser-sound table (`L45f1e`) | seen: Space in Front View |
| 2 | 1 | Second laser sound in the same table, repeated every frame | seen after icon 7 in flight, while the view was External; probably sustained fire (unconfirmed) |
| 5 | 2 | — | static only |
| 8, 10 | 2 | Two cases in the same routine as 17/18 (`L44d3e`) | static only |
| 0x0b (11) | — | — | static only |
| 0x10 (16) | — | — | static only |
| 0x11, 0x12 (17, 18) | 1 | Weapon or impact pair (`L44c96`, `L44d3e`) | static only |
| 0x13 (19) | 2 | Hyperspace; the only sample that loops (from 0.5 s in) | static: called from the hyperspace routine `L814dc`; it is also loop-special in `audio.c` |
| 0x14 (20) | 2 | Hyperspace-related (`L63362`, which also starts the hyperspace music) | static only |
| 0x15 (21) | 1 | Police warning, played with the fine messages | seen: "launching without clearance" and "unlawful discharge of weapon" |
| 0x16 (22) | 1 | Played next to 25 | static only |
| 0x17 (23) | — | Noise loop (it loops from 0) | `audio.c` loop rule |
| 0x18 (24) | 1 | — | static only (`L45cd4`) |
| 0x19 (25) | 1 | Battle-music trigger; played with the 0x98ad/0x98ac messages | static (`L4607e`; `L75606` starts the battle music) |
| 0x1a (26) | 2 | Undercarriage (icon 9) | seen |
| 0x1c (28) | 0 | Left-click in the 3D view: selecting a target or contact | seen |
| 0x1d (29) | 0 | Message beep: a station or police message arrives | seen, with every fine message |
| 0x1f (31) | 1 | — | static only (`L45d40`) |
| 0x20 (32) | — | Played with the 0x98ef/0x98ee messages | static only |

Of the 33 sample ids, 7 were triggered live, 15 more appear as an immediate id at a static call site, and 11 (3, 4, 6, 7, 9, 12–15, 27, 30) are only reached through computed ids, such as the laser table and the distance-driven `SFX_Thing`. Mapping the rest needs combat, docking and hyperspace runs: in the scripted runs, clicking a star on the galactic map did not select a hyperspace target. The trace filter and `mark` tool make this a short follow-up.

### Music (track → situation)

Music is requested by mode, not always by track:
- mode −1: random track, continuous;
- mode −2: random track, played once, never 6 or 7;
- mode 0–7: that track, played once.

| Track | Name (from the Options screen) | When `fe2.s` asks for it |
|---|---|---|
| 0 | Grieg – In the Hall of the Mountain King | random only |
| 1 | Lowe – Frontier Main Theme | title screen: `PlayMusic(1)` is requested while `IsMusicPlaying` returns 0 |
| 2 | Mussorgsky – Baba Yaga | random only |
| 3 | Mussorgsky – Night on the Bare Mountain | random only |
| 4 | Lowe – Frontier Second Theme | random only |
| 5 | Wagner – Ride of the Valkyries | random only |
| 6 | Strauss – Blue Danube | docking, if "docking music" is on |
| 7 | Mussorgsky – Great Gates of Kiev | mission or reward screens |
| mode −2 | random, not 6 or 7 | hyperspace (if music on and "hyperspace music" on), battle (if "battle music" on), plus one more message-driven site |
| mode −1 | random, continuous | when "continuous music" is on |

The game sends a mask of the tracks the player enabled; the host decodes it into one bit per track. `StopMusic` is called at game start, on several screen changes, and when the police fine you.

### The OGG_MUSIC path

1. `Makefile-C` always defines `OGG_MUSIC` and links libvorbisfile.
2. `Call_PlayMusic` (`src/audio.c`) reads the mode and mask, then calls `play_music(track)`. Under `SDL_LockAudio`, that opens `music/NN.ogg` with `ov_open`.
3. The SDL callback `Audio_CallBack` `ov_read`s the music straight into the output buffer. It assumes 22,050 Hz 16-bit stereo, and no resampling is done.
4. It then adds up to four sample channels on top, as a plain `short` sum.
5. At the end of a stream:
   - mode −1 picks another random track;
   - any other mode stops.
6. `IsMusicPlaying` (hcall 0x21) reports `music_playing`.
7. If the file is missing, `play_music` simply leaves `music_playing` false.

## Evidence

All in `docs/experiments/evidence/E02/`. The summary file is [`identity.txt`](evidence/E02/identity.txt).

- **Loaded samples, flag off.** All 33 hashes match the independent Python decode. Sample 0 is `f1de2cee`, 3,914 bytes. With the flag on, only sample 0 changes, to `133daf76` (3,968 bytes), which matches the generated file.
- **Sound calls.** Runs A, B and C each make 66 calls, with the same md5 (`91598c24…`). Run D makes 7. Its only difference is that the title screen requests track 1 once instead of 60 times, because `IsMusicPlaying` now returns 1. Every PlaySFX and StopMusic is identical.
- **Frames.** Three shots, both the shown frame and the game's own 320×200 buffer, are md5-identical across A, B and C.
- **Full trace.** Ignoring the Idle spin count, A and B differ in 14 lines out of 866,823. All of them are the six input-poll calls that run inside the 20 ms VBL interrupt in frame 122. Two runs of the same binary (B and C) differ in exactly the same lines, so this is timer noise, not code.
- **What you hear.** In capture C, the non-silent part of the original click appears once, bit-exact, at 6.28 s, where the first `PlaySFX(0)` fires (F271), and the new chirp does not appear. In capture D, the chirp appears once, bit-exact, at 6.22 s, and the original does not. Capture D has signal at 1–3 s, which is the generated track 1 on the title screen. Capture C is silent there, because this worktree has no `music/` folder.
- **Log.** Run D prints `FE2_AUDIO_DIR: sfx 00 replaced` and `music 01 replaced`. Run B prints nothing.
- **Flag-on screen drift.** Shots l2 and l3 in run D differ by one digit of the launch speed readout (10.0 vs 11.0 km/h). The host-call histograms match through frame 593. In frame 594 the input-poll loop ran three times, because the VBL interrupt landed differently with the music decoder running. The game steps on that wall-clock timer, which is the Phase 1 pacing issue. Game data is unchanged.

## Hook needed

The hook is host side only, in `src/audio.c`, and `fe2.s` is untouched:
- one `getenv`;
- a directory lookup before each `sfx/sfx_NN.wav` load and each `music/NN.ogg` open;
- an optional capture `fwrite` at the end of the audio callback.

With the flags off, the added work is one `getenv` at start-up and a NULL test per sample load and per track open.

A production version needs four more things:
1. A pack manifest keyed by sample id and track id.
2. Resampling: today a WAV that is not 22,050 Hz is rejected, and a stereo WAV would be played as mono at double length, because `check_sample_format` checks the rate and format but not the channel count. An Ogg at another rate plays at the wrong speed.
3. A saturating mixer.
4. Optionally, per-event mapping. The trace shows that events are coarse: the message beep, sample 0x1d, was used for both station and police messages in the runs. Different sounds for different messages would need the string id passed alongside, which d2 already carries at the fine sites (`9e03`, `9e05`).

## Oracle implications

- **Sound is outside the 68k state.** Swapping a sample changes no `fe2.s` memory. Requests are fully determined by the game (id, channel, mode, mask), so a parity oracle should compare the request stream (hcalls 0x12/0x1d/0x1e), not the audio heard.
- **Music choice is not reproducible.** Under mode −1/−2, the track is picked by the host's `rand()`, which is seeded from the wall clock. The oracle must check the mode and mask the game sent, never which track played.
- **Playing music changes the request stream.** With a real track playing, `IsMusicPlaying` returns 1 and the title loop stops re-requesting it, so the PlayMusic count differs. Treat `PlayMusic` repeats while nothing is playing as equal to one request.
- **Timing caveat.** Extra audio work can move the VBL interrupt and with it in-flight physics readouts. Oracles that compare frames during flight need the Phase 1 deterministic pacing first.
- **Fidelity check** (standing rule, 2026-10-25):
  - A replacement must be selected only by the game's own sample id or track id. Its classification must equal the game's record: the hyperspace loop stays a loop (19), the noise stays a loop (23), and music still honours the player's enabled-track mask and the docking, battle and hyperspace options.
  - Two arrivals at the same event must pick the same replacement. With a pack loaded, sample 0 hashed to `133daf76` on every load.
  - Restyling the sound is allowed; changing when a sound plays, or which event it signals, is not.

## Legal flags

- The 33 effects in `sfx/` and the recordings in `music/` are Frontier Developments copyright. They were read locally for hashing and never committed: the evidence holds only hashes and timings.
- Tracks 1 and 4 are David Lowe's own compositions and recordings, so both the composition and the recording are protected.
- Tracks 0, 2, 3, 5, 6 and 7 are public-domain compositions (Grieg, Mussorgsky, Wagner, Strauss). David Lowe's arrangements and recordings of them are still protected. A free replacement must be a new performance or synthesis from the score, or a recording under a clear licence (CC0 or CC-BY).
- The generated chirp and arpeggio are original, produced by the committed script.
- Third-party sound packs should ship as separate downloads. They should be loaded via `FE2_AUDIO_DIR`, never bundled with FD material.

## Effort to productionise

**S** for the code: a manifest, resampling via `SDL_ConvertAudio` and `ov_read` to the device rate, a saturating mixer, and a channel bounds check. **M** overall once a complete, licensed sound pack is counted (33 effects and 8 tracks): that is content work, not engineering.

Phase 1 issues seen and not fixed:
1. `L3e05c_SFX_Thing` passes `63 − distance` as the channel, and `Call_PlaySFX` indexes `wav_channels[4]` with no bounds check, so this is a potential out-of-bounds write.
2. The `short` sample sum in `Audio_CallBack` wraps instead of clipping.
3. With `music/` absent, the title screen calls `PlayMusic` every frame.
4. `SDL_LoadWAV` overwrites the global `desiredAudioSpec` with each file's spec. This is harmless after `SDL_OpenAudio`, but surprising.
