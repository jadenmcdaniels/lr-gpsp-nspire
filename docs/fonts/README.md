# The menus' type

One family: **IBM Plex Mono**, by IBM, in two weights, Regular and Medium.
The files used (`IBMPlexMono-Regular.ttf` and `IBMPlexMono-Medium.ttf`) are a
build whose outlines differ from Google Fonts' copy of the same family. SIL Open Font
License 1.1: free to redistribute, also inside a program; the licence is
`IBMPlexMono-OFL.txt` beside this file. "Plex" is a Reserved Font Name: the
bitmaps made from it are not offered as a font of that name; they are only
the program's glyphs, and the source is credited here.

One size everywhere, 11 px (the longest messages, 50 characters, must fit
the screen's width on one line). Medium for names and choices, Regular for
the rest. The four "big words" cuts the code still names (ui_font_d62 ...)
are the Medium at that same size, in capitals.

Only bitmaps go in the .tns (`nspire_ui_font.h`, 45,234 bytes: eight slots,
two positions between pixels, 4 bits a pixel), not the font files.

    python3 -I tools/ui_font.py FONTDIR > nspire_ui_font.h

(FONTDIR holds `IBMPlexMono-Regular.ttf` and `IBMPlexMono-Medium.ttf`; needs
Pillow and freetype-py, e.g. `pip install --target DIR freetype-py` and
`FREETYPE_PY=DIR`.)

The icons are drawn in `tools/ui_icons.py` (no font): `nspire_ui_icons.h`.
The design: `docs/ui/spec.txt`; every screen, through the real menu code:
`tests/host/ui_wired.sh OUTDIR`.
