# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Retrack: a from-scratch 4-track looper firmware for the original Polyend Tracker
(Teensy-class MK66FX1M0, FT81x display, SGTL5000 codec, external SDRAM). It
reuses OpenTracker's board/HAL layer; everything under `src/looper/` is new.
`PROVENANCE.md` lists which files were copied from OpenTracker and which are
unmodified — keep it accurate when you touch a copied file.

## Commands

```bash
make -j$(nproc)                 # firmware: build/looper.hex, build/PolyendTracLooperv<XY>.ptf,
                                # and build/sdcard/Firmware/ (image + ot_restore.txt)
make VERSION=1.1                # versioned image; VERSION must be X.Y, one digit each
make SELFTEST=1                 # OpenTracker hardware selftest image
make -C sim -j$(nproc)          # desktop simulator: build/sim/looper-sim (needs SDL2)
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy make -C sim check   # every sim/scripts/*.txt, headless
make -C sim asan                # simulator with ASan + UBSan (build/sim-asan)
make -C sim soak SEEDS="1 2" MINUTES=10   # random long sessions on the asan build
```

Run one script (from `sim/`, after building):

```bash
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ../build/sim/looper-sim --script scripts/modes.txt --tone 330 --sd ../build/sim/check/sd_modes --shot ../build/sim/check/modes.png
```

Each `expect_*` line prints PASS or FAIL with the actual value; grep the output
for `FAIL`. Interactive: `build/sim/looper-sim` (keyboard rows are the pads,
F1..F8, Enter = Rec, Space = Play, Left Shift = Shift, `[ ]` or wheel = encoder).

The firmware toolchain is `arm-none-eabi-gcc` + newlib; there is no linter or
formatter configured. CI (`.github/workflows/build.yml`) builds only on `vX.Y`
tags (or by hand): firmware, `make -C sim check`, then a GitHub release.

## Sim scripts (the test suite)

`sim/scripts/*.txt` drive the real app headlessly in virtual time. The full
command list is in the header comment of `sim/src/main.cpp` (wait, tap PAD,
press/hold/release NAME, enc N, tone, opt, expect_state/len/opt/mode/pitch/
audio/perf/grain/fx/stream, shot/screen, …). Things that trip new scripts:

- The default start mode is a count-in, so a take doesn't start on the press;
  most scripts begin with `opt all start 0`.
- Each script runs on a fresh SD folder (`build/sim/check/sd_<name>`).
- Screenshots land in `build/sim/check/`; look at them when changing UI.
- `sim/demo/*.txt` + `tools/readme_shots.sh` regenerate the README images in `docs/img`.

## Architecture

**Layers.** `src/main.cpp` (OpenTracker, unchanged) brings the board up and calls
`otAppMain`; `src/device_main.cpp` fills a `looper::Platform` (`src/looper/platform.h`)
from the HAL and lays out SDRAM (loop buffers, FX lines, waveform staging).
`sim/src/main.cpp` fills the same `Platform` from SDL/headless stand-ins.
Nothing in `src/looper/` may include a `hal/` header — only `Platform` and
`src/platform/hal_api.h` interfaces — so the same app code runs in both.

**Engine (`engine.h/.cpp`) — audio thread.** `render()` runs in the audio
callback (I2S ISR on device, SDL thread in sim). The UI talks to it only via
`engine_.post(Engine::kCmd, track, arg)` — a lock-free single-producer queue
drained at the start of each block — and reads published `TrackView`/meter
atomics. Consequence: a value read right after a `post()` is still the old
one. Options go through `kOpt` with `arg = (OptParam << 8) | value`, mixer
through `kMix`, etc. Slow work (clearing megabytes of SDRAM, cut/erase,
undo) is done from the main loop's `service()` in chunks.

Timing model: one global frame counter; the first take sets the master bar
length, every other track is a multiple of it, and a track's phase is a pure
function of the counter — that's why every playback mode stays in time.
`PlayMode` values are stored in project files: **add new modes at the end**.

**App (`app.h/.cpp`) — main loop.** Front-panel input, pad LEDs and drawing.
Pages and overlays are boolean flags (`mixer_`, `fxPage_`, `edit_`, `inPage_`,
`modeBar_`, `grainMenu_`, `manual_`, …); `goHome()` clears them all and the
`home` predicate in `onInput` is "none of them set". Home-page F-keys are tap/hold
keys: a `*Held_`/`*Turned_`/`*DownMs_` triple per key, tap = release within
`kTapMs` without an encoder turn, hold + encoder = a pop-up knob
(`drawPopupKnob`). Arrow keys auto-repeat (`PanelInput::serviceHolds` re-sends
key-down), so hold logic must ignore repeated downs. The Shift layer swaps the
home F-key labels and actions (`shiftLayer()`).

**Other modules.**
- `fx.*` — the two send-effect slots (delay, ping-pong, reverb, chorus,
  flanger, Clouds); `clouds/` is the Mutable Instruments Clouds port.
- `stream.*` — takes longer than RAM go to the SD card; the audio-thread half
  lives in Engine. Design: `docs/SD_STREAMING.md`.
- `soundboard.*` — the S.Play page's 48 WAV slots: each slot's start in
  SDRAM, the rest streamed per voice from the card; mixed in by the engine
  through `Engine::setSource` (into the input when "to loops" is on). Its
  SDRAM comes off the top of the loop memory in `App::init`.
- `files.*` — settings, projects, WAV export/load as incremental jobs stepped
  from `service()` so audio and UI never wait; saves write `.tmp` then swap.
- `midi.*` — clock in/out and learned footswitch actions.
- `manual.h` — the on-device manual text (Song key).

## When changing controls

Users read four places; keep them in step with any control change:
`src/looper/manual.h` (lines under ~88 chars), `docs/REFERENCE.md`, `README.md`,
and the header comment of `src/looper/app.h`. Add or update a sim script for
the new behaviour.

## Release / image constraints

- The image name must be exactly 24 characters (`PolyendTracLooperv<XY>.ptf`);
  the Makefile errors otherwise, hence one-digit `X.Y` versions.
- `tools/check_image.py` rejects images that would lock the chip or leave the
  stock flash range — never bypass it.
- `docs/DEVICE_TEST.md` is the on-device checklist, including the way back to
  stock firmware (`ot_restore.txt`).
