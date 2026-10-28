# The README screenshots (tools/readme_shots.sh runs this from sim/): four
# example loops recorded on bar lines, then each page.
wait 300
opt all start 0

# Takes of different lengths (2, 1, 4 and 3 bars at 120 bpm), so the lanes'
# playheads sit in different places. Each take starts on a bar line, the stop
# press lands just before the line N bars on (on-bar stops record to it). Each
# starts 30 ms past its bar line, clear of block rounding.
# Track 1: drums, 2 bars.
input_wav ../build/demo/drums.wav
tap 0
wait 3950
tap 0
wait 4030
# Track 2: bass, 1 bar, from 8.0 s.
input_wav ../build/demo/bass.wav
tap 12
wait 1950
tap 12
wait 6030
# Track 3: chords, 4 bars, from 16.0 s; a shot in its fourth bar (the cue).
input_wav ../build/demo/chords.wav
tap 24
wait 6970
screen ../docs/img/recording.png
wait 980
tap 24
wait 2030
# Track 4: the pad, 3 bars, from 26.0 s.
input_wav ../build/demo/pad.wav
tap 36
wait 5950
tap 36
wait 4030
input_wav off
expect_state 4 PLAY
expect_len 1 4.0
expect_len 2 2.0
expect_len 3 8.0
expect_len 4 6.0

# Home: four loops playing, the playheads about 15, 30, 58 and 77 % through.
wait 600
screen ../docs/img/home.png

# The Shift layer.
hold SHIFT
wait 50
screen ../docs/img/shift.png
release SHIFT

# Holds: tempo (F4), volume (F7). Longer than a tap (400 ms), or the release
# would toggle the metronome / open the mixer.
hold F4
wait 500
screen ../docs/img/tempo.png
release F4
hold F7
wait 500
screen ../docs/img/volume.png
release F7

# The mode bar, both pages.
press F5
wait 50
screen ../docs/img/modes.png
press F8
wait 50
screen ../docs/img/modes2.png
# Granular on track 4 (selected: it was recorded last), then its knobs.
press F4
wait 100
hold F5
wait 300
press F1
enc 35
press F3
enc 25
press F1
wait 100
screen ../docs/img/granular.png
release F5
# Track 2 in reverse at half speed: the playhead trail. Two taps on its
# mute pad select it and leave it unmuted.
tap 14
tap 14
press F5
press F6
wait 1200
screen ../docs/img/home_modes.png

# Effects: FX 1 a reverb, FX 2 Clouds chained into it; sends from the tracks.
press FX1
press F1
press F3
wait 600
screen ../docs/img/fx_reverb.png
press FX2
press F1
press F6
wait 1500
mix 3 send2 70
mix 4 send1 40
mix 1 send1 20
mix 0 chain2 60
wait 2000
screen ../docs/img/fx_clouds.png
press MASTER
wait 50
screen ../docs/img/mixer.png
press PATTERN

# Performance mode, a filter sweep held on track 1.
press PERFORMANCE
down 8
wait 400
screen ../docs/img/perform.png
up 8
press PERFORMANCE

# Pages: Input, the loop editor, the manual, projects, settings, MIDI,
# firmware.
press F6
wait 50
screen ../docs/img/input.png
press PATTERN
tap 2
tap 2
press SAMPLEEDITOR
wait 100
press F1
enc 1
press F2
enc -2
wait 100
screen ../docs/img/editor.png
press PATTERN
press SONG
wait 50
screen ../docs/img/manual.png
press PATTERN
press FILE
wait 50
press F1
wait 6000
screen ../docs/img/file.png
press PATTERN
press CONFIG
wait 50
screen ../docs/img/config.png
press F2
wait 50
screen ../docs/img/midi.png
press PATTERN
sd_file /Firmware/PolyendTracker_1.9.2.ptf 4096
sd_file /Firmware/PolyendTracLooperv01.ptf 4096
press CONFIG
press F1
wait 100
screen ../docs/img/firmware.png
press PATTERN

# Track 4 cleared (its pad in column 4), then armed to start on sound.
tap 39
wait 300
expect_state 4 EMPTY
opt 4 start 2
tap 36
wait 400
expect_state 4 ARMED
screen ../docs/img/armed.png
tap 36
