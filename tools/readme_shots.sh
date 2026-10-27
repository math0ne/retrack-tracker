#!/bin/bash
# Regenerate the README screenshots in docs/img: synthesize the example loops,
# then run sim/demo/readme_shots.txt and readme_long.txt (a long take) in the
# headless simulator.
set -e
cd "$(dirname "$0")/.."
python3 tools/demo_sounds.py build/demo
make -C sim -s
mkdir -p docs/img
rm -rf build/demo/sd build/demo/sd_long
(cd sim && ../build/sim/looper-sim --script demo/readme_shots.txt --sd ../build/demo/sd)
(cd sim && ../build/sim/looper-sim --script demo/readme_long.txt --sd ../build/demo/sd_long)
# The simulator writes plain PNGs; a 256-colour palette keeps them small.
python3 - <<'EOF'
import glob
from PIL import Image
for p in glob.glob("docs/img/*.png"):
    Image.open(p).convert("RGB").quantize(256, method=Image.Quantize.MEDIANCUT).save(p, optimize=True)
EOF
