# Loops on the SD card: exploration

Today every loop lives in the 16 MB SDRAM: about 22 s stereo (44 s mono) per
track. This note looks at reading and writing loops straight from the card
instead, so a loop can be minutes long.

## What the hardware layer already does

From `src/hal/sdhc.cpp`, `src/hal/board.h`, `src/hal/sd_storage.*` (OpenTracker's):

- **Bus**: 4-bit SDHC, high speed after CMD6 at **45 MHz** (default speed
  22.5 MHz). That is ~22 MB/s on the wire; real cards manage maybe 8–15 MB/s
  sequential reads and 3–10 MB/s sequential writes.
- **Transfers**: ADMA2 multi-block (CMD18 / CMD25 with auto CMD12), up to
  8 x 32 KB = 256 KB per command. Errors retry once, then step the clock
  down, last resort PIO.
- **SDRAM**: `kSdDmaToSdram = false` — *unverified* whether the SDHC DMA
  master can reach the SDRAM controller, so SDRAM buffers **bounce** through
  an 8 KB SRAM buffer with a memcpy each way. That works today (projects
  load and save through it); it just costs a copy (~0.3 ms per 8 KB into the
  uncached 16-bit SDRAM, roughly).
- **Blocking**: every transfer is polled to completion on the calling
  thread. The looper only touches the card from the main loop, so the audio
  interrupt is never blocked; the UI frame waits instead.
- **FatFs**: `FF_USE_EXPAND 0`, `FF_USE_FASTSEEK 0`, no exFAT (FAT32: 4 GB
  per file, enough).
- **Benchmark**: `SdStorage::benchmark()` already writes / reads / verifies a
  temp file and times both — the selftest uses it.

## What streaming needs

Per stereo track, 44.1 kHz 16-bit = **176 KB/s**.

| Situation | Card traffic |
|---|---|
| 4 tracks playing | 0.7 MB/s read |
| 4 tracks overdubbing (read + write) | 0.7 read + 0.7 write = 1.4 MB/s |
| 1 recording + 3 playing | 0.18 write + 0.53 read |

So the bandwidth is fine, a fraction of what the bus does. **Latency** is
the problem:

- **Write busy**: after a write the card may hold DAT0 busy for its flash
  management. Typical 1–40 ms, but the SD spec allows up to **250 ms** for
  SDHC, and cheap cards do hit 100 ms+ now and then.
- **Our own main loop**: a pass can already take ~20 ms (a waveform upload
  is a blocking ~18 ms SPI write), and a card stall sits on top of that.
- **FAT work**: allocating clusters or updating the FAT during a take means
  extra reads and writes at unpredictable moments.

Hence: big buffers in SDRAM, no FAT work while streaming, and reading ahead
of the playhead.

## Design

**Files.** Each track gets a contiguous, pre-allocated file
(`f_expand`, `FF_USE_EXPAND 1`) of, say, 5 minutes (53 MB stereo) when the
project is created or the card is prepared. Once it is allocated, the
looper streams **raw sectors** (`disk_read` / `disk_write` at the file's
first sector + offset) — no FatFs in the hot path, no FAT updates during a
take. The file's real length is written at the end of the take. If the card
is too fragmented for `f_expand`, the track falls back to RAM length.

**Ring buffers.** Per track in SDRAM: a read-ahead ring (1 s, 176 KB) and a
write-behind ring (1 s). Four tracks: 1.4 MB, out of 16 MB. The audio
interrupt reads and writes only the rings; a main-loop job keeps the read
ring at least ~0.5 s ahead and drains the write ring in 64–256 KB commands.
Ring underrun = the track goes silent for that block (counted, logged),
never a crash.

**Hybrid, not all-or-nothing.** Loops that fit in RAM stay in RAM exactly
as now (instant everything). Only a take that grows past its RAM share
spills to the card: the head stays in RAM, the rest streams. Most of the
live-looping use (bars, not minutes) never touches the card.

**What breaks on a streamed track**, and the answers:

| Feature | Problem | Answer |
|---|---|---|
| Forward / tape modes | none | read ahead in order |
| Reverse, ping-pong | reads backwards | read-ahead runs in the play direction |
| Jumps (pads 5–12) | random access | keep the first ~0.25 s of each eighth in RAM (8 x 44 KB), jump plays from there while the stream catches up |
| Beat repeat, tape stop | a short window | the slice is in the read ring already |
| Granular Spread | grains anywhere in the loop | limit Spread to the read ring on streamed tracks |
| Overdub | read-modify-write | mix in the ring, write back — or write to a second file and keep the old one: **that is undo / redo for free** |
| Waveforms, editor | peaks | peaks stay in RAM (2 bytes per 256 frames: 5 min = 100 KB per track) |
| Trim | none | it is just start / end |
| Cut / erase | moves audio | a card-to-card copy job (slow for minutes of audio; progress shown) |
| Save / export | — | the files are already on the card: save becomes "write the settings" |
| Tempo / Stretch | read rate up to 2x | 2x the bandwidth, still small |

**Undo on the card.** Each overdub pass writes into the other of two files
per track (A/B); undo flips back to the previous one, redo flips forward.
RAM-only loops can do the same with an undo copy while there is SDRAM to
spare (a loop under half its share).

## Risks, in the order to check them

1. **Card latency on this hardware** — measure it first (below). If the
   worst write stall on a decent card is under ~300 ms, 1 s rings are plenty.
2. **SDHC DMA into SDRAM** (`kSdDmaToSdram`) — try it with a verify pass;
   if it works, the bounce copy goes away. Not needed for correctness.
3. **Main-loop stalls** — a blocking 256 KB write is ~25–80 ms of frozen UI.
   Commands of 32–64 KB keep the UI smooth at a few percent of bus time.
4. **Card removal mid-take** — the rings run dry, the track goes silent,
   the card watcher unmounts; the RAM part survives.

## First step: measure the card

Done with an SD test screen (Config > F3 in builds up to v0.1, since removed). It wrote a 16 MB
file once (256 KB commands, allocating it), then overwrote it in place and
read it back in 32 / 64 / 256 KB commands, every word verified, and left
`Looper/sdtest.txt`. It went through FatFs and the SDRAM bounce buffer, as
streaming would today; `kSdDmaToSdram` is not tried yet (a compile-time HAL
constant, and a wrong guess there could corrupt or hang).

The original plan: a **Config > SD test** screen: sequential write and read in 32 / 64 / 256 KB
commands to a pre-allocated file for ~20 s, reporting MB/s, the average and
**worst** command time, and a histogram of stalls (> 10, 50, 100, 250 ms). It
uses the existing `benchmark()` and raw transfers, and it can also try
`kSdDmaToSdram` with a verify pass. A few minutes with your cards gives the
numbers the ring sizes depend on, before any of the streaming is built.

## Measured (the SD test, one 8 GB card)

| Card format | Overwrite in place, 256 KB requests | Read | Worst overwrite |
|---|---|---|---|
| FAT32, 4 KB clusters | 1.7 MB/s (1.9 with direct DMA) | 5.1 MB/s (6.3) | 460 ms |
| FAT32, 32 KB clusters | 3.0 MB/s (**8.0** with direct DMA) | 7.0 MB/s (**15.9**) | 253 ms (166 with DMA) |

FatFs sends at most one cluster per card command, so a card formatted with
small clusters is slow: format with 32 KB clusters (the SD Card Formatter's
default for SDHC). Direct DMA to the SDRAM worked in every run (thousands of
transfers, no errors, every word checked). Growing a file while writing is
4x slower with stalls up to 3.9 s: stream files are allocated before use.

## Phase 1: long takes (built)

The time limit goes from ~22 s to 5 minutes stereo (10 mono) per track.

- **Files.** `Looper/Stream/trackN.raw`, 5 minutes of stereo each (53 MB),
  allocated contiguously with `f_expand` (`IStorage::preallocate`) once the
  card is ready, and kept. No card: the old limit.
- **Head + ring.** A take records into RAM as before. When it reaches
  `head` = its RAM minus 2 s, the track becomes a long take: frames from
  `head` on go to a 2 s **ring** (that last 2 s of its RAM) and from there to
  the file at `(frame - head)`. The audio thread only ever touches RAM.
- **Recording.** The main loop's `Streamer` drains the ring to the file in
  32 KB commands (direct DMA). If the card falls 2 s behind, the take ends
  there (an overrun, counted). At the end of the take the file gets zeros
  for the silent rest of the last bar.
- **Playback.** The loop's first `head` frames play from RAM; the rest from
  the ring, which the `Streamer` keeps filled from the file 1.5 s ahead of
  the playhead and 0.5 s behind it (beat repeats read a little behind). A
  frame the ring does not hold plays as silence (an underrun, counted) — a
  jump into the streamed part does that for a moment.
- **Works on a long take:** Forward and the forward reading modes (Half /
  Double speed, Half time, Octave up / down), tempo changes (tape or
  stretch), mute, stop, jumps, the performance effects, sends, the
  waveform (peaks for the whole take stay in RAM: 100 KB per track).
- **Not yet** (refused, with a hint): overdub and replace, the reverse
  modes, Ping-pong and Granular, the loop editor, multiply, saving or
  exporting a project with a long take.

## Phase 2: overdub, undo, every direction (built)

- **The files hold the whole take**, from frame 0: every take with a stream
  file ready is mirrored to file A as it records (a short one stops being
  mirrored when it ends), so the RAM part of a long take is on the card too.
- **Blocks.** The files are kept in 32 KB blocks (a card command each;
  `head` and the ring are whole blocks). A per-track map in the Streamer says
  which file, A or B, holds each block's current version.
- **Overdub / replace** mix into RAM or the ring (`Engine::dubFrame`) and
  mark the 32 KB blocks they change. A changed block goes back to the card
  once the playhead has left it; before the ring moves (a jump, the wrap)
  all of them go back, the ring empty meanwhile so the overdub stops
  writing it. A frame the ring does not hold is not overdubbed (counted).
- **Undo / redo.** A pass's first write of a block goes to the *other* file
  and flips the block in the map, so the version before the pass stays.
  Undo writes everything back, flips the pass's blocks back, reloads the RAM
  part's from the card, recomputes their peaks and refills the ring (the
  track silent meanwhile); redo flips them again. The next pass starts a new
  set (the undone version is then overwritten, as in RAM). One level, like
  the RAM undo. Fast seek (`FF_USE_FASTSEEK`, `IStorage::fastSeek`) keeps the
  seeks between the two files and back and forth cheap.
- **Direction.** The engine publishes which way the playhead reads; the
  Streamer keeps the ring from 0.5 s behind it forwards, or from 0.5 s after
  it backwards (and, while a reverse loop is in its RAM part, the loop's end:
  it wraps there next; Ping-pong turns at the start, so forwards there).

## Phase 3: parity (built)

- **Buffer frames.** The ring, the dirty blocks and the file offsets count
  buffer frames (loop frame + start), so a trim just moves the loop over its
  buffer, as in RAM; its undo too.
- **Save / export** read a long take through `Streamer::read` (RAM, the ring
  where an overdub has not gone back yet, the current file of each block).
  **Load** puts a WAV longer than the track's RAM into RAM up to `head` and
  all of it into stream file A (`Streamer::write`), then installs it long
  (`kInstall` with the long bit).
- **Cut / erase** are Streamer jobs (`editStep`, the track silent): write
  everything back, then move the rest up over the cut (through the staging
  buffer, into RAM below `head` and file A) or zero the range block by
  block, then recompute the peaks; the overdub undo goes, a cut's map is all
  file A again.
- **Granular.** Each grain reads either the RAM part or the ring, wholly and
  without wrapping: it is kept off `head`, off the loop's end, and inside
  what the ring holds (with its own length to spare, since the ring moves on
  as it plays); one too long for the ring is shortened. So on the card part
  Spread reaches about the ring around the playhead, not the whole loop.
