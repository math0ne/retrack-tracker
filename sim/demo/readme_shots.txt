# The README screenshots (tools/readme_shots.sh runs this from sim/): four
# example loops recorded on bar lines, then each page.
wait 300
opt all start 0

# Track 1: drums. Each take starts on a bar line, the stop press lands just
# before the line two bars on (on-bar stops record to it).
input_wav ../build/demo/drums.wav
tap 0
wait 3950
tap 0
wait 4010
# Track 2: bass, from 8.0 s.
input_wav ../build/demo/bass.wav
tap 12
wait 3950
tap 12
wait 4010
# Track 3: chords, from 16.0 s; a shot while it records.
input_wav ../build/demo/chords.wav
tap 24
wait 2500
screen ../docs/img/recording.png
wait 1450
tap 24
wait 4010
# Track 4: the pad, from 24.0 s.
input_wav ../build/demo/pad.wav
tap 36
wait 3950
tap 36
wait 4010
input_wav off
expect_state 4 PLAY
expect_len 4 4.0

# Home: four loops playing.
wait 1300
screen ../docs/img/home.png
shot ../docs/img/panel.png

# The Shift layer.
hold SHIFT
wait 50
screen ../docs/img/shift.png
release SHIFT

# Holds: tempo (F4), volume (F7).
hold F4
wait 300
screen ../docs/img/tempo.png
release F4
hold F7
wait 300
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
press NOTE

# Performance mode, a filter sweep held on track 1.
press PERFORMANCE
down 8
wait 400
screen ../docs/img/perform.png
up 8
press PERFORMANCE

# Pages: Input, the loop editor, the manual, projects, settings, MIDI.
press F6
wait 50
screen ../docs/img/input.png
press NOTE
tap 2
tap 2
press INSTR
wait 100
screen ../docs/img/editor.png
press NOTE
press SONG
wait 50
screen ../docs/img/manual.png
press NOTE
press FILE
wait 50
screen ../docs/img/file.png
press NOTE
press CONFIG
wait 50
screen ../docs/img/config.png
press F2
wait 50
screen ../docs/img/midi.png
press NOTE
