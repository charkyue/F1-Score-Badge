#!/usr/bin/env python3
# 官方队标 -> RGB565 位图（240x240，黑底，压暗水印）
# 用法：python3 gen_logos_official.py   （缺少的队标会自动从 formula1.com 下载到 logos_src/）
from PIL import Image
import os, urllib.request

WS = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(WS, 'logos_src')
SIZE = 240
DIM = 0.32  # 亮度系数（水印低调，但又清晰可见）

# F1 官网 Cloudinary 队标地址（512px 透明底）
DOWNLOAD = {
    '2026alpine.png':       'https://media.formula1.com/image/upload/w_512/f_png/v1740000001/common/f1/2026/alpine/2026alpinelogowhite.png',
    '2026astonmartin.png':  'https://media.formula1.com/image/upload/w_512/f_png/v1740000001/common/f1/2026/astonmartin/2026astonmartinlogowhite.png',
    '2026audi.png':         'https://media.formula1.com/image/upload/w_512/f_png/v1740000001/common/f1/2026/audi/2026audilogowhite.png',
    '2026cadillac.png':     'https://media.formula1.com/image/upload/w_512/f_png/v1740000001/common/f1/2026/cadillac/2026cadillaclogowhite.png',
    '2026ferrari.png':      'https://media.formula1.com/image/upload/w_512/f_png/v1740000001/common/f1/2026/ferrari/2026ferrarilogowhite.png',
    '2026haasf1team.png':   'https://media.formula1.com/image/upload/w_512/f_png/v1740000001/common/f1/2026/haasf1team/2026haasf1teamlogowhite.png',
    '2026mclaren.png':      'https://media.formula1.com/image/upload/w_512/f_png/v1740000001/common/f1/2026/mclaren/2026mclarenlogowhite.png',
    '2026mercedes.png':     'https://media.formula1.com/image/upload/w_512/f_png/v1740000001/common/f1/2026/mercedes/2026mercedeslogowhite.png',
    '2026racingbulls.png':  'https://media.formula1.com/image/upload/w_512/f_png/v1740000001/common/f1/2026/racingbulls/2026racingbullslogowhite.png',
    '2026redbullracing.png':'https://media.formula1.com/image/upload/w_512/f_png/v1740000001/common/f1/2026/redbullracing/2026redbullracinglogowhite.png',
    '2026williams.png':     'https://media.formula1.com/image/upload/w_512/f_png/v1740000001/common/f1/2026/williams/2026williamslogowhite.png',
    'f1_logo.png':          'https://www.formula1.com/assets/driverTeam/_next/static/media/f1-logo-180.1db9e85b.png',
}

# LOGO_TABLE 顺序（必须与 F1Score.ino 的 logoForTeamName 一致）
ORDER = [
    ('LOGO_MCLAREN',     '2026mclaren.png'),
    ('LOGO_FERRARI',     '2026ferrari.png'),
    ('LOGO_RED_BULL',    '2026redbullracing.png'),
    ('LOGO_MERCEDES',    '2026mercedes.png'),
    ('LOGO_ASTON_MARTIN','2026astonmartin.png'),
    ('LOGO_ALPINE',      '2026alpine.png'),
    ('LOGO_WILLIAMS',    '2026williams.png'),
    ('LOGO_RB',          '2026racingbulls.png'),
    ('LOGO_AUDI',        '2026audi.png'),
    ('LOGO_HAAS',        '2026haasf1team.png'),
    ('LOGO_CADILLAC',    '2026cadillac.png'),
    ('LOGO_F1',          'f1_logo.png'),
]

def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def ensure_sources():
    os.makedirs(SRC, exist_ok=True)
    for fname, url in DOWNLOAD.items():
        path = os.path.join(SRC, fname)
        if not os.path.exists(path):
            print(f'下载 {fname} ...')
            req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
            with urllib.request.urlopen(req, timeout=30) as r, open(path, 'wb') as f:
                f.write(r.read())

def process(fname):
    im = Image.open(os.path.join(SRC, fname)).convert('RGBA')
    bbox = im.getchannel('A').getbbox()
    if bbox:
        im = im.crop(bbox)
    # 等比缩放，内容完整放入 SIZE x SIZE
    im.thumbnail((SIZE, SIZE), Image.LANCZOS)
    canvas = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
    canvas.paste(im, ((SIZE - im.width) // 2, (SIZE - im.height) // 2), im)
    # 压暗并合成到黑底
    px = canvas.load()
    out = Image.new('RGB', (SIZE, SIZE), (0, 0, 0))
    op = out.load()
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b, a = px[x, y]
            op[x, y] = (int(r * DIM * a / 255),
                        int(g * DIM * a / 255),
                        int(b * DIM * a / 255))
    return out

def to_c_array(img):
    px = img.load()
    vals = []
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b = px[x, y]
            vals.append(rgb565(r, g, b))
    return vals

# ---- 生成 logos.h ----
ensure_sources()
lines = []
lines.append('// 自动生成：F1 官方车队队标水印（RGB565，黑底，32% 亮度），240x240')
lines.append('#pragma once')
lines.append('#include <Arduino.h>')
lines.append('')

previews = []
for name, fname in ORDER:
    img = process(fname)
    previews.append((name, img))
    vals = to_c_array(img)
    lines.append(f'static const uint16_t {name}[{SIZE}*{SIZE}] PROGMEM = {{')
    for i in range(0, len(vals), 16):
        lines.append('  ' + ','.join(f'0x{v:04X}' for v in vals[i:i+16]) + ',')
    lines.append('};')
    lines.append('')
    print(f'{name:20s} <- {fname}  非零像素 {sum(1 for v in vals if v != 0)}')

lines.append('// 名字 -> 位图表指针')
lines.append('static const uint16_t *const LOGO_TABLE[] = {')
for name, _ in ORDER:
    lines.append(f'  {name},')
lines.append('};')
lines.append('')

out_path = os.path.join(WS, 'logos.h')
with open(out_path, 'w') as f:
    f.write('\n'.join(lines))
print(f'\nlogos.h 已生成，{os.path.getsize(out_path)/1e6:.2f} MB')

# ---- 预览图：黑底 4 列网格，模拟屏幕观感 ----
COLS, ROWS = 4, 3
CELL, PAD, LABEL = 260, 30, 34
W = COLS * (CELL + PAD) + PAD
H = ROWS * (CELL + LABEL + PAD) + PAD
grid = Image.new('RGB', (W, H), (24, 24, 28))
from PIL import ImageDraw
d = ImageDraw.Draw(grid)
for i, (name, img) in enumerate(previews):
    cx = PAD + (i % COLS) * (CELL + PAD)
    cy = PAD + (i // COLS) * (CELL + LABEL + PAD)
    grid.paste(img, (cx, cy))
    d.text((cx + 6, cy + CELL + 8), name.replace('LOGO_', ''), fill=(220, 220, 220))
grid.save(os.path.join(WS, 'logo_official_preview.png'))
print('预览图已生成 logo_official_preview.png', grid.size)
