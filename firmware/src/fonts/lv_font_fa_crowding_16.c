/*******************************************************************************
 * Size: 16 px
 * Bpp: 4
 * Opts: --font .pio/libdeps/cyd-3248S035R/lvgl/scripts/built_in_font/Montserrat-Medium.ttf -r 0x20 --font .pio/libdeps/cyd-3248S035R/lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff -r 0xF6C0,0xF183 --size 16 --bpp 4 --format lvgl --no-compress --force-fast-kern-format --lv-font-name lv_font_fa_crowding_16 -o src/fonts/lv_font_fa_crowding_16.c
 ******************************************************************************/

#include <lvgl.h>

#ifndef LV_FONT_FA_CROWDING_16
#define LV_FONT_FA_CROWDING_16 1
#endif

#if LV_FONT_FA_CROWDING_16

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */

    /* U+F183 "" */
    0x4, 0xee, 0x40, 0xe, 0xff, 0xe0, 0xe, 0xff,
    0xe0, 0x4, 0xee, 0x40, 0x27, 0x44, 0x72, 0xdf,
    0xff, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xdf, 0xff,
    0xfd, 0xc, 0xff, 0xc0, 0xc, 0xff, 0xc0, 0xc,
    0xff, 0xc0, 0xc, 0xff, 0xc0, 0x9, 0xff, 0x90,

    /* U+F6C0 "" */
    0x0, 0x0, 0x2b, 0xff, 0xff, 0xb2, 0x0, 0x0,
    0x0, 0x2, 0xef, 0xfb, 0x8f, 0xfe, 0x20, 0x0,
    0x0, 0xb, 0xf4, 0xf6, 0x1f, 0x7e, 0xb0, 0x0,
    0x0, 0xf, 0xa1, 0xf6, 0x1f, 0x69, 0xf0, 0x0,
    0x0, 0xf, 0x81, 0xf6, 0x1f, 0x68, 0xf0, 0x0,
    0x0, 0xf, 0x81, 0xf6, 0x1f, 0x68, 0xf0, 0x0,
    0x0, 0xf, 0x81, 0xf6, 0x1f, 0x68, 0xf0, 0x0,
    0x0, 0xf, 0x81, 0xf6, 0x1f, 0x68, 0xf0, 0x0,
    0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
    0x7, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x70,
    0xd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xd0,
    0xc, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xb0,
    0x0, 0xff, 0x0, 0x0, 0x0, 0x0, 0xff, 0x0,
    0x0, 0xff, 0x0, 0x0, 0x0, 0x0, 0xff, 0x0,
    0x0, 0xff, 0x0, 0x0, 0x0, 0x0, 0xff, 0x0,
    0x0, 0xee, 0x0, 0x0, 0x0, 0x0, 0xee, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 69, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 0, .adv_w = 96, .box_w = 6, .box_h = 16, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 48, .adv_w = 224, .box_w = 16, .box_h = 16, .ofs_x = -1, .ofs_y = -2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0xf163, 0xf6a0
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 63137, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 3, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t lv_font_fa_crowding_16 = {
#else
lv_font_t lv_font_fa_crowding_16 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 16,          /*The maximum line height required by the font*/
    .base_line = 2,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if LV_FONT_FA_CROWDING_16*/

