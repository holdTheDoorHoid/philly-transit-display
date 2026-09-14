# Project fonts

`lv_font_icons_16.c` — the 16 px icon font: `0123456789` and space from Montserrat Medium plus
FontAwesome 5 Free `chair` (U+F6C0), `user` (U+F007), `bicycle` (U+F206), `bolt` (U+F0E7),
`parking` (U+F540); 4 bpp, ~4.5 KB. Used by `crowdingIcons()` and `bikeCounts()` in
`ui_common.cpp` through LVGL's inline recolor (the header weather icons are colour bitmaps in
`src/icons/`, not glyphs) (`#rrggbb text#`; the space after the colour is
mandatory and is not drawn). Generated 2026-09-14 with

```
npx lv_font_conv --font <lvgl>/scripts/built_in_font/Montserrat-Medium.ttf -r 0x20,0x30-0x39 \
  --font "<lvgl>/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff" \
  -r 0xF6C0,0xF007,0xF206,0xF0E7,0xF540 --size 16 --bpp 4 --format lvgl --no-compress \
  --force-fast-kern-format --lv-font-name lv_font_icons_16 -o lv_font_icons_16.c
```

then the generated `#ifdef LV_LVGL_H_INCLUDE_SIMPLE` include block replaced by `#include <lvgl.h>`.
FontAwesome icons are CC BY 4.0 (LVGL ships the same file for its built-in symbols; see
`<lvgl>/scripts/built_in_font/font_license`).

`lv_font_montserrat_48_digits.c` — Montserrat Medium, 48 px, 4 bpp, only the characters the big
"minutes" label and the night clock can produce: `0123456789:apDueNow -`. Generated 2026-09-14 with

```
npx lv_font_conv --font <lvgl>/scripts/built_in_font/Montserrat-Medium.ttf --size 48 --bpp 4 \
  --format lvgl --no-compress --force-fast-kern-format --symbols "0123456789:apDueNow -" \
  -o lv_font_montserrat_48_digits.c
```

A full 48 px Montserrat would cost ~60 KB of flash; this subset is ~13 KB. If `ui_common.cpp`
ever renders another character in the big font, add it to `--symbols` and regenerate (LVGL draws
missing glyphs as a box, `LV_USE_FONT_PLACEHOLDER`). Declared with `LV_FONT_DECLARE` in
`ui_common.cpp`; used for `device.large_text` and the night clock (DESIGN.md §6, §8).
