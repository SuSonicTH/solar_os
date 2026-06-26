# SolarOS Font Generation

This directory holds source font faces and offline tooling for generating
SolarOS bitmap fonts. The firmware should consume generated U8g2 C arrays, not
runtime TTF rendering.

The generator expects these exact source face filenames in this directory:

- `Regular.ttf`
- `Italic.ttf`
- `Bold.ttf`
- `BoldItalic.ttf`

Generate BDF files and a preview image:

```sh
python3 fonts/generate_u8g2_fonts.py
```

The default output is written below `fonts/build/`:

- `bdf/` - monochrome fixed-cell BDF files
- `preview/default_preview.png` - enlarged bitmap preview
- `manifest.json` - generated metrics and filenames

Build the vendored `bdfconv` tool:

```sh
make -C fonts/tools/bdfconv
```

With `bdfconv` built, the same script can generate U8g2 C arrays:

```sh
python3 fonts/generate_u8g2_fonts.py
```

The generated C files will be placed in `fonts/build/u8g2/`. They are meant to
be inspected first. Firmware builds automatically compile the generated default
font arrays when `fonts/build/u8g2/u8g2_font_solar_os_default_r_14_tf.c`
exists.

An external `bdfconv` can still be used explicitly:

```sh
python3 fonts/generate_u8g2_fonts.py --bdfconv /path/to/bdfconv
```

Defaults:

- Sizes: `12, 14, 16, 18, 20`
- Styles: regular, bold, italic, bold italic
- Glyphs: ASCII plus Latin-1 supplement, `0x20-0x7e` and `0xa0-0xff`
- Rendering: monochrome FreeType output, no antialiasing

Useful options:

```sh
python3 fonts/generate_u8g2_fonts.py --sizes 14,18
python3 fonts/generate_u8g2_fonts.py --glyph-ranges 0x20-0x7e
python3 fonts/generate_u8g2_fonts.py --antialias --threshold 160
python3 fonts/generate_u8g2_fonts.py --no-u8g2
```

