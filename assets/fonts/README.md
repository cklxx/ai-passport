<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Fonts

Store reusable font files and generated font sources here.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

## Bundled device font

`main/lv_font_ai_passport_14.c` is generated from Source Han Sans SC Normal
2.005. It uses 1-bit compressed glyphs at 14 px and covers ASCII, common
Chinese punctuation, full-width forms, and the complete U+4E00-U+9FFF CJK
Unified Ideographs block. The source font is licensed under SIL OFL 1.1; the
license text is stored in `SourceHanSans-OFL.txt`.

Regenerate it with `lv_font_conv` 1.5.3 using ranges
`0x20-0x7e,0x2000-0x206f,0x2713,0x3000-0x303f,0x4e00-0x9fff,0xff00-0xffef`,
1 bpp, compression enabled, and kerning disabled. Keep the product metrics at
17 px line height and 4 px baseline after generation.
