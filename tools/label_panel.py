#!/usr/bin/env python3
"""The README's controls picture: a picture of the Tracker with the home screen
in its display, each key it uses tagged with what it does in the looper, the
unused ones dimmed, and the pad columns named under the pads.

    tools/label_panel.py docs/img/src/tracker.png docs/img/home.png docs/img/controls.png
"""
import sys
from PIL import Image, ImageDraw, ImageFont

# Where things are in tracker.png (1361 x 1011).
SCREEN = (96, 127, 802, 520)
KEY_X, KEY_W = [854, 942, 1030, 1119, 1207], 72
KEY_Y, KEY_H = [122, 210, 298, 478, 566], 70
FKEY_X0, FKEY_PX, FKEY_Y = 104, 88.3, 566
PAD_X0, PAD_PX, PAD_W, PAD_Y = 97, 59.6, 47, [703, 763, 822, 882]
ENC = (1068, 815)

SEL, TEXT, DIM = (0xD7, 0xCD, 0x64), (0xE8, 0xE8, 0xE8), (0x90, 0x90, 0x96)
BADGE = (16, 16, 18, 225)

# The key grid, row by row: what the looper does with each (None: unused,
# "": its own arrow says it).
KEYS = [
    [None, "Perform", "Projects", "Settings", "Mixer"],
    ["Samples", None, "Editor", None, "Manual"],
    ["Editor", "Perform", "FX 1", "FX 2", "Home"],
    ["Play all", "Duplicate", "Undo", "Track", "Clear"],
    ["Rec track", "Shift", "", "Track", ""],
]
PADS = ["Rec", "Play", "Mute", "Clear"] + [f"Jump {i}" for i in range(1, 9)]


def font(size, bold=True):
    name = "DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"
    return ImageFont.truetype(f"/usr/share/fonts/truetype/dejavu/{name}", size)


def badge(d, cx, cy, text, f, fill=SEL):
    l, t, r, b = d.multiline_textbbox((cx, cy), text, font=f, anchor="mm", align="center", spacing=1)
    d.rounded_rectangle([l - 5, t - 3, r + 5, b + 3], radius=4, fill=BADGE)
    d.multiline_text((cx, cy), text, font=f, fill=fill, anchor="mm", align="center", spacing=1)


def main(photo, screen, dst):
    im = Image.open(photo).convert("RGBA")
    over = Image.new("RGBA", im.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(over)
    f, fs, fn = font(12), font(13, False), font(14)

    # The home screen in the display, 800 x 480 fitted to its height.
    sx0, sy0, sx1, sy1 = SCREEN
    h = sy1 - sy0
    w = round(h * 800 / 480)
    shot = Image.open(screen).convert("RGBA").resize((w, h), Image.LANCZOS)
    im.paste(shot, (sx0 + (sx1 - sx0 - w) // 2, sy0))

    for r, row in enumerate(KEYS):
        for c, label in enumerate(row):
            x, y = KEY_X[c], KEY_Y[r]
            if label is None:   # unused: dimmed
                d.rounded_rectangle([x, y, x + KEY_W, y + KEY_H], radius=6, fill=(0, 0, 0, 170))
            elif label:
                # Up / Down: under the arrow; the rest over the key's middle.
                cy = y + KEY_H - 14 if label == "Track" else y + KEY_H / 2 - (0 if r == 3 else 8)
                badge(d, x + KEY_W / 2, cy, label.replace(" track", "\ntrack"), f)

    for i in range(8):   # the screen keys
        badge(d, FKEY_X0 + i * FKEY_PX + 35, FKEY_Y + 35, f"F{i + 1}", f, TEXT)
    d.text(((sx0 + sx1) / 2, FKEY_Y - 18), "Screen keys: what the label over each says",
           font=fs, fill=DIM, anchor="mm")

    d.text(((PAD_X0 + PAD_X0 + 11 * PAD_PX + PAD_W) / 2, 672), "Pads: one row per track, 1–4 from the top",
           font=fs, fill=DIM, anchor="mm")
    for c, label in enumerate(PADS):
        d.text((PAD_X0 + c * PAD_PX + PAD_W / 2, PAD_Y[-1] + 46 + 14), label, font=f,
               fill=SEL if c < 4 else TEXT, anchor="ms")

    ex, ey = ENC
    badge(d, ex, ey + 128, "Encoder: volume, values\nPush: tap tempo", fn, SEL)

    # Full colour (a palette bands the panel), keeping the transparent surround.
    Image.alpha_composite(im, over).save(dst, optimize=True)


if __name__ == "__main__":
    main(*sys.argv[1:4])
