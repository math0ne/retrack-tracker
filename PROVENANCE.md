# Provenance

Copied from OpenTracker as found in math0ne/opentracker-firmware commit
`861f574` (a fork of https://github.com/sandlbn/polyend-tracker-re). Copies are
unmodified unless noted. Third-party code keeps its own licence
(`third_party/teensy3/LICENSE.txt`, `third_party/fatfs/`, `third_party/fonts/`,
`sim/third_party/*LICENSE*`).

Licence note: OpenTracker's README says its own code is MIT, "see LICENSE at
the repo root", but neither the upstream repository (branch `v2`, checked
2026-10-26) nor the fork ships that file. Before publishing this repo, get the
MIT notice (copyright line) from the OpenTracker author and add it here.

| Here | OpenTracker path |
|---|---|
| `src/main.cpp` | `src/main.cpp` |
| `src/hal/*` | `src/hal/*` |
| `src/platform/hal_api.h` | `src/core/hal_api.h` |
| `src/platform/formats/crc32.*` | `src/core/formats/crc32.*` |
| `src/platform/ui/fonts/*` | `src/core/ui/fonts/*` |
| `src/platform/ui/buttons.h` | `src/core/ui/buttons.h` |
| `src/platform/app/input_gain.h` | `src/core/app/input_gain.h` |
| `third_party/teensy3/`, `third_party/fatfs/` | same (PJRC Teensy core, FatFs) |
| `third_party/fonts/*.txt` | licence files only (Roboto Apache 2.0, Roboto Mono OFL) |
| `tools/check_image.py` | same |
| `sim/src/*` except `main.cpp` | same |
| `sim/third_party/*` | same (DejaVu ROM-font stand-ins, stb_image) |

`src/looper/clouds/` comes from the same fork's `src/core/audio/clouds/`
(its port of Mutable Instruments Clouds, MIT, copyright 2014 Emilie Gillet):
`stmlib_compat.*`, `resources.*`, `diffuser.h` unmodified but for include
paths; `granular_sample_player.h` with a 128-sample block; `grain.h` reads
mono buffers too. `clouds_fx.*` is new (the fork's `granular_cloud.*`
rewritten: stereo in, freeze, continuous resampling, a working feedback
high-pass).

New in this repo: `src/looper/*`, `src/device_main.cpp`, `sim/src/main.cpp`,
`sim/scripts/*`, both Makefiles (adapted from OpenTracker's).

Not copied: OpenTracker's app (`src/app_main.cpp`, the rest of `src/core`:
sequencer, sample engine, UI screens and widgets) and its tests.
