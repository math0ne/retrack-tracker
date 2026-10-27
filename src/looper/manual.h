// The on-device manual (Song key): seven chapters on F1..F7, each a list of
// lines. "# " starts a heading; "" is a blank line. Lines fit the screen in
// the small font (under ~88 characters).
#pragma once

namespace looper {

struct ManualChapter {
    const char* key;     // F-key label
    const char* title;
    const char* const* lines;
    int count;
};

namespace manual {

constexpr const char* kKeys[] = {
    "# The home page: four lanes, one per track; F1..F8 act on the selected one",
    "F1 REC      record / finish / overdub / stop overdub (Shift: replace)",
    "F2 PLAY     stop: back to its start; play: from its start on the next bar",
    "F3 MUTE     mute on / off (it keeps running in time)",
    "F4 TEMPO    tap: metronome on / off; hold + encoder: tempo;",
    "            Shift + hold + encoder: click level",
    "F5 MODE     tap: the mode bar; hold on a Granular track: the grain menu",
    "F6 INPUT    tap: the Input page; hold + encoder: input gain",
    "F7 VOLUME   hold + encoder: the track's volume (the encoder alone does too)",
    "F8 UNDO     undo the last overdub or replace; then REDO brings it back",
    "",
    "# Shift layer (hold Shift on the home page; F5..F8 act on the selected track)",
    "F1 Replace (punch in)   F2 Play / stop all   F3 Solo   F4 Click level",
    "F5 Stop: tap free / on bar / on beat; hold + encoder: a fixed length in bars",
    "F6 Start: tap now / count-in / on sound; hold + encoder: the sound threshold",
    "F7 After rec: play / overdub / stop        F8 Tempo: Tape / Stretch",
    "",
    "# Other keys",
    "Rec          record / overdub the selected track (Shift: replace)",
    "Play         every track from bar 1, or all stopped (Shift: metronome)",
    "Delete       clear the selected track (Shift: all of them)",
    "Up / Down    select a track (any pad selects its row too)",
    "Encoder      volume of the selected track (Shift: input gain)",
    "Enc. push    tap tempo: three taps or more",
    "Insert       undo the last overdub (Shift: redo)",
    "Note         home, from any page",
    "Perform      performance mode (tap: on / off, hold: while held); Pattern too",
    "Master       mixer         Instr   loop editor       Song   this manual",
    "FX1 / FX2    the effect slots",
    "File         projects, WAV export        Config   settings, firmware, MIDI",
    "Shift+Config the load / frame-rate readout    Shift+File   the stress preset",
    "Power        hold 1.5 s to switch off",
};

constexpr const char* kPads[] = {
    "# Pads: four rows = four tracks, twelve columns",
    "Column 1     record / overdub (Shift + pad: replace)",
    "Column 2     play / stop",
    "Column 3     mute",
    "Column 4     clear the track",
    "Columns 5-12 jump to that eighth of the loop (it stays in time)",
    "",
    "# Performance mode (Perform or Pattern)",
    "Columns 5-12 become eight effects on that row's track:",
    "  5 Repeat 1/8   6 Repeat 1/16   7 Low-pass   8 High-pass",
    "  9 Sweep down  10 Sweep up     11 Vibrato   12 Tape stop",
    "Hold a pad: the effect while held. Shift + pad: latch it (again: off).",
    "Hold a pad and turn the encoder: that effect's knob (shared by the tracks).",
    "Column 4, rows 1 and 2: Freeze for a Clouds slot in FX 1 / FX 2.",
    "Leaving performance mode turns every effect off.",
    "",
    "# Lights",
    "REC blinks fast while recording, slowly while overdubbing.",
    "PLAY is lit while the track plays. The pads show brightness only.",
};

constexpr const char* kModes[] = {
    "# Playback modes (F5 opens the bar; F8 in the bar turns the page)",
    "Page 1, playback:",
    "  F1 Forward     F2 Reverse      F3 Ping-pong    F4 Half speed",
    "  F5 Double      F6 Half rev     F7 Double rev   F8 More",
    "Page 2, grains:",
    "  F1 Half time   F2 Octave up    F3 Octave down  F4 Granular",
    "",
    "Forward, Reverse and Ping-pong can overdub. Half / Double speed play like",
    "tape (the pitch follows); Half time and the octaves keep the time.",
    "",
    "# Granular",
    "Grains read around the playhead, so the loop keeps its time.",
    "Hold F5 on a Granular track: the grain menu opens and stays. F1..F4 pick",
    "Spread, Size, Density, Pitch, F5 closes it (F6..F8 do nothing meanwhile);",
    "the encoder turns the one picked. The lane draws each grain where it reads:",
    "taller and brighter as it sounds louder, higher when panned left.",
    "",
    "# The playhead",
    "A fading trail behind it shows where it came from and how fast it goes:",
    "short at half speed, long at double, on the right when it goes backwards.",
};

constexpr const char* kRecord[] = {
    "# Recording",
    "The first take sets the bar. By default a take starts after a bar of",
    "count-in and stops on the next bar line after you press again.",
    "A take records a little past its end and blends it into its start: no click.",
    "",
    "# Long takes (with an SD card)",
    "Past ~19.5 s (39 s mono) a take goes on to the card, up to 5 min (10 mono).",
    "The lane says card. Everything works as in RAM (editor cut / erase take a",
    "few seconds); Granular's Spread stays within ~1 s of the playhead there.",
    "Format the card FAT32 with 32 KB clusters (more under Pages).",
    "",
    "# Overdub, replace, undo",
    "REC on a playing track: overdub (feedback on the Input page).",
    "Shift + REC: replace, recording over the loop instead of on top of it.",
    "F8 (or Insert) undoes the last overdub or replace; again: redo.",
    "Undo works for loops up to ~10.8 s stereo in RAM, and for long takes.",
    "",
    "# Track options (Shift + F5..F8, for the selected track)",
    "Stop       free, on bar or on beat        Length   hold F5 + encoder: bars, or off",
    "Start      now, count-in or on sound      Threshold  hold F6 + encoder",
    "After rec  play, overdub or stop          Tempo    Tape or Stretch",
    "Channels (stereo / mono) and stop fade are on the Input page (F6).",
    "",
    "# Tempo",
    "Before a loop the tempo sets the bar. After, it is tape speed (50-200 %):",
    "every loop speeds up or slows down. Stretch tracks keep their pitch.",
    "Stop fade: Stop fades the track over 1 beat, 1 bar or 2 bars first.",
};

constexpr const char* kFx[] = {
    "# Effect slots (FX1 / FX2)",
    "Delay, Ping-pong, Reverb, Chorus, Flanger, Clouds.",
    "F1 opens the type bar; F2..F5 pick a knob, the encoder turns it;",
    "F6 the return; F7 the other slot; F8 home.",
    "Clouds has eight knobs: Shift + F2..F5 picks the second four (Texture,",
    "Feedback, Spread, Freeze) and keeps them; tap Shift for the first four.",
    "",
    "# Clouds",
    "Position: how far back the grains read. Size: grain length. Pitch: +-24 st.",
    "Density: few grains at noon, more either way (random above, even below).",
    "Texture: the grain window (diffused at the top). Freeze holds the buffer.",
    "",
    "# Mixer (Master)",
    "F1..F7 pick a strip, Up / Down a row, the encoder sets it (Shift: fine).",
    "Tracks: To FX 1, To FX 2 (sends, before the track's level), Level.",
    "FX strips: a knob, To FX 2 / To FX 1 (this effect into the other one,",
    "e.g. Clouds into the reverb), Return. Master: the output level.",
};

constexpr const char* kPages[] = {
    "# Input page (F6)",
    "Source, Gain, the track's channels (while it is empty), Monitor,",
    "Monitor FX (hear the input through the effects), Overdub feedback;",
    "the tempo card: metronome, click level, tempo, beats per bar, stop fade.",
    "",
    "# Loop editor (Instr)",
    "F1 start / F2 end marker (the encoder moves it)   F3 snap",
    "F4 trim (keep the selection)   F5 cut (twice)   F6 erase (twice)",
    "F7 undo the last trim",
    "",
    "# Projects (File)",
    "F1 save   F2 save as new   F3 load (twice; Up / Down pick)",
    "F5 export the track as a WAV   F6 export every track",
    "",
    "# Settings (Config)",
    "Display and pad brightness, headphones, input, gain, monitor.",
    "F1 Firmware: flash an image from the card's Firmware folder (F1 twice).",
    "F2 MIDI: clock and controls.",
    "F3 Latency: patch an output into the input, F3: it measures the round trip",
    "and sets Rec latency, so takes and overdubs land where you played them.",
    "",
    "# The SD card",
    "FAT32 with 32 KB clusters (the SD Card Formatter's default for SDHC).",
    "Small clusters make long takes too slow; exFAT cards are not read.",
};

constexpr const char* kMidi[] = {
    "# MIDI (Config, F2)",
    "Clock: Off, Send or Follow (DIN and USB).",
    "Send: 24 ticks a beat with Start and Stop; it follows the tempo.",
    "Follow: Start / Stop run the loops; the tempo follows the clock.",
    "",
    "# Controls",
    "Eight actions to play from a footswitch or a controller:",
    "Rec / dub, Play / stop, Undo, Redo, Replace, Next track, Prev track,",
    "Play all.",
    "Pick one, F1 Learn, then press the pedal or key: a note, or a",
    "controller (a footswitch sends 127). F2 clears it. They are saved.",
};

}  // namespace manual

#define LOOPER_CHAPTER(key, title, arr) {key, title, manual::arr, int(sizeof manual::arr / sizeof manual::arr[0])}
constexpr ManualChapter kManual[] = {
    LOOPER_CHAPTER("Keys", "Keys", kKeys),
    LOOPER_CHAPTER("Pads", "Pads and performance", kPads),
    LOOPER_CHAPTER("Modes", "Playback modes", kModes),
    LOOPER_CHAPTER("Record", "Recording, undo, tempo", kRecord),
    LOOPER_CHAPTER("FX", "Effects and mixer", kFx),
    LOOPER_CHAPTER("Pages", "Pages", kPages),
    LOOPER_CHAPTER("MIDI", "MIDI", kMidi),
};
#undef LOOPER_CHAPTER
constexpr int kManualChapters = int(sizeof kManual / sizeof kManual[0]);

}  // namespace looper
