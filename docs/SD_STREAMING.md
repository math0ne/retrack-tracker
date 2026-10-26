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

A **Config > SD test** screen: sequential write and read in 32 / 64 / 256 KB
commands to a pre-allocated file for ~20 s, reporting MB/s, the average and
**worst** command time, and a histogram of stalls (> 10, 50, 100, 250 ms). It
uses the existing `benchmark()` and raw transfers, and it can also try
`kSdDmaToSdram` with a verify pass. A few minutes with your cards gives the
numbers the ring sizes depend on, before any of the streaming is built.
