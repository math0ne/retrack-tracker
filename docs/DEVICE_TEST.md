# First device test

The looper has only run in the simulator. This is the checklist for the first
run on a real Tracker, in order: safety, then "does it work", then the
performance numbers.

## Before flashing

- SD card `/Firmware/` holds `PolyendTracker_1.9.2.ptf` (the way back) and
  `PolyendTracLooperv01.ptf` (from `make`: copy `build/sdcard/Firmware/` onto the card).
- Install: with the stock firmware or OpenTracker running, Config > Firmware
  update and pick `PolyendTracLooperv01.ptf`.
- The way back: Config > F1 Firmware, pick `PolyendTracker_1.9.2.ptf`, F1
  twice. If the looper hangs, write that name (or an OpenTracker image name)
  into `/Firmware/ot_restore.txt` on a computer and power-cycle: the check runs
  at boot before the looper starts. (Builds before the Firmware screen have
  only this route; `make` writes an `ot_restore.txt` into `build/sdcard/Firmware/`.)
- Open a serial terminal on the Tracker's USB serial port (any baud rate; it is
  USB CDC, e.g. PuTTY on the COM port, or `screen /dev/ttyACM0`). Boot
  diagnostics, crash reports and the telemetry lines appear only while a
  terminal is open.

## 1. Does it work (5 minutes)

Things the simulator could not check, because it maps them its own way:

- [ ] It boots to four empty lanes and the F-key bar (no selftest screen: that
      would mean the SDRAM test failed).
- [ ] **Pad layout:** pad row 1 = track 1, leftmost pad = REC. Press the top-left
      pad: track 1 should start recording. If a different pad does it, note
      which pad index the press arrives as (the layout in the app assumes
      index = row * 12 + column from the top left).
- [ ] F1..F8 act on the label above them; the encoder turns the right way.
- [ ] Line in: the INPUT cell's meter moves; a take plays back at the right
      pitch and level. Headphones are set to 0.7.
- [ ] Pad lights: REC blinks while recording, PLAY lit while playing.

## 2. Performance

Press **Help** for the on-screen readout, and keep the serial terminal open.
Once a second it prints a line like:

```
looper: cpu avg 18.3% max 24.1% | loop max 4120 us (2100/s) | 60 fps | dl 3480 B |
spi 680 KB/s, 12 waves/s | tracks PPPP perf 0c0c0c0c | i2c err 0/0 nak 0/0 | display faults 0
```

| Field | What it is | Good | Trouble |
|---|---|---|---|
| cpu avg / max | Audio interrupt time per 128-frame block, of the 2.9 ms budget | max under ~70 % | max near 100 %: clicks and dropouts |
| loop max | Longest main-loop pass: the worst wait before a pad press is read | under ~20 ms | over ~40 ms: pads feel laggy |
| fps | Screen frames per second (capped at ~60) | 30+ | under 20 |
| dl | Display-list bytes of the last frame | under 8192 | at 8192: the frame is cut short |
| spi, waves/s | Waveform uploads to the display (one per frame at most) | | |
| i2c err / nak | Keypad / pad-LED / codec bus errors | 0 | rising |
| display faults | Display coprocessor resets | 0 | rising |

Run these and note the lines:

1. **Idle**: nothing recorded.
2. **Normal**: four tracks playing, a couple of sends up.
3. **Recording**: record and overdub on several tracks at once (watch loop max
   and waves/s: the waveform uploads are the main-loop cost).
4. **Performance mode**: hold several effects at once, filter sweeps and
   repeats.
5. **Worst case**: record a loop on all four tracks, then **Shift + Help**.
   Every track goes to Granular (8 grains, an octave up) with both filters
   latched and full sends; FX 1 becomes a reverb and FX 2 Clouds at full
   density (24 grains); the metronome comes on. This is the highest load the
   app can make; its `cpu max` is the number that matters. Every grain reads
   the uncached 16-bit SDRAM, so this is the first number to check after the
   Clouds port: if it is over budget, lower `CloudsFx::kMaxGrains` and
   `kLoopGrains`. Tap Perform twice to drop the effects.

Listen for clicks or dropouts in each one; a click with `cpu max` well under
100 % points somewhere else (SDRAM, I2S), so note when it happens.

## If something goes wrong

- Hangs or crashes: the serial terminal shows the fault report. Copy it.
- No sound: check the headphone and line-out level first; Master key > master
  level.
- Back to stock: `ot_restore.txt` as above.
