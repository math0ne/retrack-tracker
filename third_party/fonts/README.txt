Open-licensed fonts rendered at build-generation time by tools/fonts/gen_fonts.py
(`make fonts`) into src/core/ui/fonts/font_data.* (spec 20 §2, font sets 0 and 1).
The TTFs themselves are not linked into the firmware.

  RobotoMono-Regular/Medium/Bold.ttf  Roboto Mono v3 (googlefonts/RobotoMono),
                                      SIL Open Font License 1.1: RobotoMono-OFL.txt
  Roboto-Light/Regular/Medium/Bold.ttf Roboto 2.138 (googlefonts/roboto, unhinted),
                                      Apache License 2.0: Roboto-LICENSE-Apache-2.0.txt

Set 0 "Original": Roboto Mono Regular (fonts 1, 2), Medium (3, 5), Bold (4).
Set 1 "New":      Roboto Light (fonts 1, 5), Regular (2), Medium (3), Bold (4).
Symbol glyphs (font 4 codes 24..31, font 5 codes 17..31) and the icon sheet are
our own drawings in gen_fonts.py.
