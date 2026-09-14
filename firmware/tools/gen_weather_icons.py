#!/usr/bin/env python3
"""Renders the header's colour weather icons (src/icons/weather_icons.{c,h}) and a preview sheet.

Flat-colour 24x24 pictograms drawn with Pillow at 6x and downsampled, exported as LVGL 9 image
descriptors in RGB565A8 (RGB565 little-endian plane, then an A8 plane; stride = w*2). Run from
firmware/:  python3 tools/gen_weather_icons.py   (needs Pillow). weather_service.cpp maps WMO codes
to these names (WeatherIcon enum); main_screen.cpp draws them with lv_image.
"""
from PIL import Image, ImageDraw
import math, struct, os, sys
S = 24; SS = 6; W = S * SS
HERE = os.path.dirname(os.path.abspath(__file__)); OUT = os.path.join(HERE, '..', 'src', 'icons')
SUN = (255, 193, 7, 255); SUN_O = (245, 140, 0, 255); MOON = (236, 232, 200, 255); MOON_O = (200, 190, 140, 255)
CLOUD = (214, 221, 228, 255); CLOUD_O = (140, 152, 165, 255); DARK = (120, 132, 148, 255); DARK_O = (80, 90, 104, 255)
BLUE = (64, 160, 255, 255); SNOW = (130, 190, 240, 255); FOG = (170, 180, 190, 255); BOLT = (255, 214, 0, 255); BOLT_O = (230, 150, 0, 255)
def canvas(): return Image.new('RGBA', (W, W), (0, 0, 0, 0))
def circle(d, cx, cy, r, fill, outline=None, ow=0): d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=fill, outline=outline, width=ow)
def sun(d, cx, cy, r):
    for i in range(8):
        a = i * math.pi / 4
        d.line([cx + math.cos(a) * r * 1.35, cy + math.sin(a) * r * 1.35, cx + math.cos(a) * r * 1.85, cy + math.sin(a) * r * 1.85], fill=SUN_O, width=int(SS * 1.6))
    circle(d, cx, cy, r, SUN, SUN_O, int(SS * 0.9))
def moon_img(cx, cy, r):
    im = canvas(); d = ImageDraw.Draw(im); circle(d, cx, cy, r, MOON, MOON_O, int(SS * 0.9))
    mask = Image.new('L', (W, W), 255); md = ImageDraw.Draw(mask)
    md.ellipse([cx + r * 0.35 - r * 0.85, cy - r * 0.55 - r * 0.85, cx + r * 0.35 + r * 0.85, cy - r * 0.55 + r * 0.85], fill=0)
    im.putalpha(Image.composite(im.split()[3], Image.new('L', (W, W), 0), mask)); return im
def cloud(d, cx, cy, w, fill=CLOUD, outline=CLOUD_O):
    h = w * 0.55; ow = int(SS * 0.9)
    parts = [(cx - w * 0.22, cy - h * 0.15, w * 0.30), (cx + w * 0.12, cy - h * 0.35, w * 0.36), (cx + w * 0.32, cy + h * 0.05, w * 0.26)]
    for (x, y, r) in parts: d.ellipse([x - r, y - r, x + r, y + r], fill=fill, outline=outline, width=ow)
    d.rounded_rectangle([cx - w * 0.5, cy - h * 0.05, cx + w * 0.5, cy + h * 0.5], radius=h * 0.3, fill=fill, outline=outline, width=ow)
    for (x, y, r) in parts: d.ellipse([x - r + ow, y - r + ow, x + r - ow, y + r - ow], fill=fill)
    d.rounded_rectangle([cx - w * 0.5 + ow, cy - h * 0.05 + ow, cx + w * 0.5 - ow, cy + h * 0.5 - ow], radius=h * 0.3, fill=fill)
def drops(d, cx, cy, n, spread, color=BLUE):
    for i in range(n):
        x = cx + (i - (n - 1) / 2) * spread; d.line([x + SS * 0.6, cy, x - SS * 0.6, cy + SS * 3.2], fill=color, width=int(SS * 1.5))
def flakes(d, cx, cy, n, spread):
    for i in range(n):
        x = cx + (i - (n - 1) / 2) * spread; r = SS * 1.5
        for a in (0, math.pi / 3, 2 * math.pi / 3):
            d.line([x - math.cos(a) * r, cy - math.sin(a) * r, x + math.cos(a) * r, cy + math.sin(a) * r], fill=SNOW, width=int(SS * 1.0))
def bolt(d, cx, cy, h):
    pts = [(cx + SS * 1.2, cy - h * 0.5), (cx - SS * 1.6, cy + h * 0.1), (cx + SS * 0.2, cy + h * 0.1), (cx - SS * 1.0, cy + h * 0.6), (cx + SS * 2.2, cy - h * 0.15), (cx + SS * 0.4, cy - h * 0.15)]
    d.polygon(pts, fill=BOLT, outline=BOLT_O)
c = W / 2
icons = {}
def make(name, fn):
    im = canvas(); d = ImageDraw.Draw(im); fn(im, d); icons[name] = im.resize((S, S), Image.LANCZOS)
make('sun', lambda im, d: sun(d, c, c, W * 0.22))
icons['moon'] = moon_img(c, c, W * 0.34).resize((S, S), Image.LANCZOS)
make('cloud_sun', lambda im, d: (sun(d, c + W * 0.15, c - W * 0.18, W * 0.16), cloud(d, c - W * 0.05, c + W * 0.15, W * 0.72)))
def cloud_moon(im, d):
    im.alpha_composite(moon_img(c + W * 0.2, c - W * 0.2, W * 0.26)); cloud(d, c - W * 0.08, c + W * 0.17, W * 0.68)
make('cloud_moon', cloud_moon)
make('cloud', lambda im, d: cloud(d, c, c, W * 0.82))
make('rain', lambda im, d: (cloud(d, c, c - W * 0.16, W * 0.78, DARK, DARK_O), drops(d, c, c + W * 0.2, 3, W * 0.2)))
make('showers', lambda im, d: (cloud(d, c, c - W * 0.16, W * 0.78, DARK, DARK_O), drops(d, c, c + W * 0.16, 4, W * 0.16), drops(d, c - W * 0.08, c + W * 0.32, 3, W * 0.16)))
make('snow', lambda im, d: (cloud(d, c, c - W * 0.16, W * 0.78), flakes(d, c, c + W * 0.3, 3, W * 0.24)))
def fog(im, d):
    cloud(d, c, c - W * 0.2, W * 0.7, FOG, (120, 130, 140, 255))
    for i, y in enumerate((0.16, 0.3, 0.44)):
        d.line([c - W * 0.38 + (i % 2) * W * 0.08, c + W * y, c + W * 0.38 - (i % 2) * W * 0.08, c + W * y], fill=FOG, width=int(SS * 1.4))
make('fog', fog)
make('storm', lambda im, d: (cloud(d, c, c - W * 0.16, W * 0.78, DARK, DARK_O), bolt(d, c, c + W * 0.22, W * 0.5)))
names = list(icons)
sheet = Image.new('RGBA', (len(names) * (S * 4 + 8) + 8, 2 * (S * 4 + 8) + 8), (0, 0, 0, 0)); sd = ImageDraw.Draw(sheet)
sd.rectangle([0, 0, sheet.width, S * 4 + 12], fill=(28, 33, 38, 255)); sd.rectangle([0, S * 4 + 12, sheet.width, sheet.height], fill=(255, 255, 255, 255))
for i, n in enumerate(names):
    big = icons[n].resize((S * 4, S * 4), Image.NEAREST); sheet.alpha_composite(big, (8 + i * (S * 4 + 8), 8)); sheet.alpha_composite(big, (8 + i * (S * 4 + 8), S * 4 + 16))
preview = sys.argv[1] if len(sys.argv) > 1 else os.path.join(OUT, 'preview.png')
sheet.save(preview)
out = ['// Generated by tools/gen_weather_icons.py - do not edit. 24x24 RGB565A8 flat-colour weather icons',
       '// for the header: LVGL image descriptors, an RGB565 little-endian plane then an A8 plane.', '#include <lvgl.h>', '']
for n in names:
    px = icons[n].load(); rgb = bytearray(); a = bytearray()
    for y in range(S):
        for x in range(S):
            r, g, b, al = px[x, y]
            if al == 0: r = g = b = 0
            rgb += struct.pack('<H', ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); a.append(al)
    data = bytes(rgb) + bytes(a)
    out.append(f'static const uint8_t wx_{n}_map[] LV_ATTRIBUTE_MEM_ALIGN = {{')
    for i in range(0, len(data), 24): out.append('  ' + ', '.join('0x%02x' % b for b in data[i:i + 24]) + ',')
    out += ['};', f'const lv_image_dsc_t wx_{n} = {{',
            f'  .header = {{.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8, .flags = 0, .w = {S}, .h = {S}, .stride = {S * 2}, .reserved_2 = 0}},',
            f'  .data_size = {len(data)},', f'  .data = wx_{n}_map,', '};', '']
open(os.path.join(OUT, 'weather_icons.c'), 'w').write('\n'.join(out))
hdr = ['// 24x24 colour weather icons for the header. Generated by tools/gen_weather_icons.py - do not edit.',
       '#pragma once', '#include <lvgl.h>', '', '#ifdef __cplusplus', 'extern "C" {', '#endif'] + [f'extern const lv_image_dsc_t wx_{n};' for n in names] + ['#ifdef __cplusplus', '}', '#endif', '']
open(os.path.join(OUT, 'weather_icons.h'), 'w').write('\n'.join(hdr))
print('wrote', len(names), 'icons ->', OUT, '| preview', preview)
