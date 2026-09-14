"""
Works around rzeldent/esp32-smartdisplay#295 ("'esp_lcd_panel_t' has no
member named 'disp_off'"), a confirmed, currently-open upstream bug: every
esp32_smartdisplay@2.1.1 panel driver that isn't backed by an ESP-IDF
built-in component (ST7796, ILI9341, GC9A01, AXS15231B, ST7701 - i.e.
everything this project's 3.5"/2.8"/2.4" boards use except the 2-USB
board's ST7789) assigns its on/off handler to `esp_lcd_panel_t::disp_off`,
a field that does not exist under that name in any Arduino-ESP32 core 3.x
this project tried (ESP-IDF 5.4.0 via pioarduino tag 54.03.21-2, and
ESP-IDF 5.5.5 via pioarduino's `stable`) - the real field is
`disp_on_off(panel, bool on_off)`. This is not just a compile error: the
library's own panel-init code (lvgl_panel_st7796_spi.c etc.) calls
`esp_lcd_panel_disp_on_off()`, which reads that exact field, so leaving
this unpatched would (if it somehow compiled) call a null function
pointer at boot on every affected board.

A second, unrelated compile error in the same library - a bare `uint`
instead of `uint32_t` in esp32_smartdisplay.h/.c - is also patched here
(already fixed on their unreleased `main`, not in the 2.1.1 tag).

A third: lvgl_touch_xpt2046_spi.c (used by every XPT2046-resistive board
in this project) includes the private ESP-IDF header
<driver/spi_common_internal.h> for spi_bus_get_attr() - moved to
esp_driver_spi/include/esp_private/spi_common_internal.h by ESP-IDF's
"driver component split", which had already happened in IDF 5.3.2 (the
oldest core 3.x pioarduino offers). Same function/signature, just a
relocated header - patched here too.

A fourth, hit in every *_spi.c/*_qspi.c panel/touch driver: they build an
esp_lcd_panel_io_spi_config_t with a `.dc_as_cmd_phase = ...` initializer,
but that struct's `flags` member doesn't have that field on this ESP-IDF
version (superseded by dc_high_on_cmd/dc_low_on_data/dc_low_on_param,
checked directly against esp_lcd_io_spi.h) - and every board JSON in
firmware/boards/ sets the corresponding macro to `false` anyway, so the
line is just dropped rather than remapped to something else.

See firmware/platformio.ini's build-notes comment for the full
investigation (issue link, which ESP-IDF versions were checked, why an
older core didn't dodge the disp_off bug, and why the board JSONs in
firmware/boards/ are pinned to the exact commit esp32-smartdisplay's own
"boards" git submodule points at rather than the latest one).

CAVEAT - this is a `pre:` extra_script, which PlatformIO runs *before* it
resolves/downloads lib_deps (confirmed against platformio's own
builder/main.py: env.SConscript(GetExtraScripts("pre")) happens before
"$BUILD_SCRIPT", and library installation happens inside the latter).
So on an environment's very first build ever - nothing under
.pio/libdeps/<env>/esp32_smartdisplay yet - this script has nothing to
patch and the build will fail with the disp_off error again. Simply run
the same `pio run -e <env>` a second time: the library is on disk by
then (PlatformIO cached it during the failed first attempt), this script
patches it, and the build succeeds. Subsequent builds (same or clean
$PROJECT_BUILD_DIR, libdeps untouched) are unaffected - only a brand new
libdeps fetch needs the extra run.
"""

import os
import re

Import("env")  # noqa: F821 - injected by PlatformIO/SCons

AFFECTED_FILES = [
    "esp_panel_st7796.c",
    "esp_panel_ili9341.c",
    "esp_panel_gc9a01.c",
    "esp_panel_axs15231b.c",
    "esp_panel_st7701.c",
]

MARKER = "/* patched by firmware/extra_scripts/patch_esp32_smartdisplay.py: disp_on_off shim */"

ASSIGN_RE = re.compile(r"ph->base\.disp_off\s*=\s*(\w+);")
FUNC_DEF_RE_TMPL = r"(esp_err_t\s+{name}\s*\(\s*esp_lcd_panel_t\s*\*\s*panel\s*,\s*bool\s+off\s*\)\s*\n\{{)"


def patch_file(path):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()

    if MARKER in text:
        return False  # already patched

    m = ASSIGN_RE.search(text)
    if not m:
        print("patch_esp32_smartdisplay: %s has no 'ph->base.disp_off = ...;' to patch (already fixed upstream?)" % path)
        return False
    disp_off_fn = m.group(1)

    func_def_re = re.compile(FUNC_DEF_RE_TMPL.format(name=re.escape(disp_off_fn)))
    fm = func_def_re.search(text)
    if not fm:
        print("patch_esp32_smartdisplay: could not find definition of %s in %s, skipping" % (disp_off_fn, path))
        return False

    shim = (
        "%s\n"
        "static esp_err_t %s(esp_lcd_panel_t *panel, bool off);\n"
        "static esp_err_t %s_on_off(esp_lcd_panel_t *panel, bool on_off)\n"
        "{\n"
        "    /* old API: off=true means \"turn off\"; new disp_on_off: on_off=true means \"turn on\" - inverted. */\n"
        "    return %s(panel, !on_off);\n"
        "}\n\n"
    ) % (MARKER, disp_off_fn, disp_off_fn, disp_off_fn)

    text = text[: fm.start()] + shim + text[fm.start() :]
    text = ASSIGN_RE.sub(r"ph->base.disp_on_off = \1_on_off;", text)

    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print("patch_esp32_smartdisplay: patched %s (%s -> disp_on_off shim)" % (path, disp_off_fn))
    return True


DC_AS_CMD_PHASE_RE = re.compile(r"[ \t]*\.dc_as_cmd_phase\s*=\s*\w+,\n")
DC_AS_CMD_PHASE_LOG_FMT_RE = re.compile(r"dc_as_cmd_phase:%d, ")
DC_AS_CMD_PHASE_LOG_ARG_RE = re.compile(r"\w+\.flags\.dc_as_cmd_phase, ")


def patch_dc_as_cmd_phase(path):
    """Fourth bug: esp_lcd_panel_io_spi_config_t's `flags` sub-struct doesn't
    have a `dc_as_cmd_phase` member on this ESP-IDF version - it was split
    into dc_high_on_cmd/dc_low_on_data/dc_low_on_param at some point before
    IDF 5.3.2 (the oldest core 3.x pioarduino offers). Every board JSON in
    firmware/boards/ sets the corresponding *_DC_AS_CMD_PHASE macro to
    `false` (checked all six), so there is no behavior to preserve here -
    the initializer line is just deleted; the field it can't set anymore
    would have been zero/false by default anyway."""
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    patched, n = DC_AS_CMD_PHASE_RE.subn("", text)
    # The same files also mention the field inside log_d() format strings and
    # argument lists, which only get compiled at CORE_DEBUG_LEVEL >= 4 - strip
    # those too so verbose builds work.
    patched, n2 = DC_AS_CMD_PHASE_LOG_FMT_RE.subn("", patched)
    patched, n3 = DC_AS_CMD_PHASE_LOG_ARG_RE.subn("", patched)
    n += n2 + n3
    if n == 0:
        return False
    with open(path, "w", encoding="utf-8") as f:
        f.write(patched)
    print("patch_esp32_smartdisplay: patched %s (dropped dc_as_cmd_phase initializer, %d occurrence(s))" % (path, n))
    return True


def patch_xpt2046_internal_header(path):
    """Third bug: lvgl_touch_xpt2046_spi.c includes <driver/spi_common_internal.h>
    for spi_bus_get_attr() (used to skip re-initializing an SPI bus the TFT
    already set up, on boards where touch and display share one). ESP-IDF's
    "driver component split" (already done by IDF 5.3.2, the oldest core
    3.x pioarduino offers - checked directly) moved this private header to
    esp_driver_spi/include/esp_private/spi_common_internal.h. Same
    function, same signature, just a different path - not a version
    mismatch to route around, just a stale include."""
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    old = "#include <driver/spi_common_internal.h>"
    new = "#include <esp_private/spi_common_internal.h>"
    if old not in text:
        return False
    with open(path, "w", encoding="utf-8") as f:
        f.write(text.replace(old, new))
    print("patch_esp32_smartdisplay: patched %s (driver/ -> esp_private/ for spi_common_internal.h)" % path)
    return True


DRAW_BUF_SIZEOF_RE = re.compile(r"sizeof\(lv_color_t\) \* LVGL_BUFFER_PIXELS")


def patch_draw_buffer_pixel_size(path):
    """Fifth bug: every lvgl_panel_*.c sizes the LVGL draw buffer as
    sizeof(lv_color_t) * LVGL_BUFFER_PIXELS, but in LVGL 9 lv_color_t is the
    3-byte RGB888 struct regardless of LV_COLOR_DEPTH, while these panels
    render RGB565 (2 bytes). On a 320x480 board with the stock
    LVGL_BUFFER_PIXELS this asks for 115,200 bytes, which is larger than the
    biggest free block on a PSRAM-less ESP32 running Arduino core 3.x, so
    heap_caps_malloc() returns NULL and lv_display_set_buffers() asserts at
    boot (observed on the owner's ESP32-3248S035R, 2026-09-14). Use the
    2-byte lv_color16_t, which is what the flush callback actually writes."""
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    patched, n = DRAW_BUF_SIZEOF_RE.subn("sizeof(lv_color16_t) * LVGL_BUFFER_PIXELS", text)
    if n == 0:
        return False
    with open(path, "w", encoding="utf-8") as f:
        f.write(patched)
    print("patch_esp32_smartdisplay: patched %s (draw buffer sized with lv_color16_t)" % path)
    return True


def patch_uint(path):
    """Second, unrelated bug hit building against the same core: bare `uint`
    (not `uint32_t`) in a public header/its .c file, which doesn't resolve
    on this toolchain's libstdc++/newlib include chain. Already fixed on
    esp32-smartdisplay's unreleased `main` branch (checked 2026-09-13); not
    yet in the 2.1.1 tag this project pins."""
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    patched = text.replace(", uint interval", ", uint32_t interval")
    if patched == text:
        return False
    with open(path, "w", encoding="utf-8") as f:
        f.write(patched)
    print("patch_esp32_smartdisplay: patched %s (uint -> uint32_t)" % path)
    return True


def main():
    libdeps_dir = env.subst(os.path.join("$PROJECT_LIBDEPS_DIR", "$PIOENV"))  # noqa: F821
    lib_dir = os.path.join(libdeps_dir, "esp32_smartdisplay")
    src_dir = os.path.join(lib_dir, "src")
    if not os.path.isdir(src_dir):
        # Nothing installed yet this run - see the CAVEAT above.
        return
    for name in AFFECTED_FILES:
        path = os.path.join(src_dir, name)
        if os.path.isfile(path):
            patch_file(path)

    for rel in (os.path.join("include", "esp32_smartdisplay.h"), os.path.join("src", "esp32_smartdisplay.c")):
        path = os.path.join(lib_dir, rel)
        if os.path.isfile(path):
            patch_uint(path)

    xpt2046_path = os.path.join(src_dir, "lvgl_touch_xpt2046_spi.c")
    if os.path.isfile(xpt2046_path):
        patch_xpt2046_internal_header(xpt2046_path)

    # Scanned rather than a fixed file list: every *_spi.c/*_qspi.c panel or
    # touch driver builds an esp_lcd_panel_io_spi_config_t the same way, and
    # all of them break on dc_as_cmd_phase - easier to catch every instance
    # than to keep a list in sync with the library.
    for name in os.listdir(src_dir):
        if name.endswith(".c"):
            patch_dc_as_cmd_phase(os.path.join(src_dir, name))
            if name.startswith("lvgl_panel_"):
                patch_draw_buffer_pixel_size(os.path.join(src_dir, name))


main()
