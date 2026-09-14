# Project fonts

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
