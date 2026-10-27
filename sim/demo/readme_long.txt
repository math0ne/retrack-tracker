# The README's long-take screenshot (tools/readme_shots.sh runs this from sim/):
# 24 s of the drum loop on track 1 (past its ~19.5 s of RAM: its tail is on the
# card), the bass and the pad as ordinary loops, track 1 overdubbed once with
# the chords for the waveform.
wait 300
opt all start 0
input_wav ../build/demo/drums.wav
tap 0
wait 23970
tap 0
wait 30
input_wav ../build/demo/bass.wav
tap 12
wait 3950
tap 12
wait 4030
input_wav ../build/demo/pad.wav
tap 36
wait 3950
tap 36
wait 4030
input_wav off
expect_stream 1 long 1
expect_len 1 24.0
# The chords over the first half of track 1 (an overdub on a long take).
input_wav ../build/demo/chords.wav
tap 0
wait 12000
tap 0
input_wav off
wait 15000
screen ../docs/img/long.png
