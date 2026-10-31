#!/usr/bin/env python3
# TTF -> Adafruit GFXfont 位图字体生成器（1bit，行内 MSB 优先）
from PIL import Image, ImageDraw, ImageFont
import os, glob

WS = os.path.dirname(os.path.abspath(__file__))

def find_bold_ttf():
    """在常见位置找一款粗体 TTF（DejaVu Sans Bold 优先）"""
    candidates = []
    try:
        import matplotlib
        candidates += glob.glob(os.path.join(os.path.dirname(matplotlib.__file__),
                                             'mpl-data', 'fonts', 'ttf', 'DejaVuSans-Bold.ttf'))
    except ImportError:
        pass
    candidates += [
        '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',          # Linux
        '/Library/Fonts/Arial Bold.ttf',                                   # macOS
        '/System/Library/Fonts/Supplemental/Arial Bold.ttf',               # macOS
        'C:/Windows/Fonts/arialbd.ttf',                                    # Windows
    ]
    for p in candidates:
        if os.path.exists(p):
            return p
    raise SystemExit('找不到粗体 TTF 字体，请把 DejaVuSans-Bold.ttf 路径填入本脚本 TTF 变量')

TTF = find_bold_ttf()
print('使用字体:', TTF)

FIRST, LAST = 0x20, 0xFF   # 覆盖拉丁扩展（é=0xE9 ü=0xFC）

def gen_font(name, size):
    font = ImageFont.truetype(TTF, size)
    ascent, descent = font.getmetrics()
    yadv = ascent + descent

    bitmap = bytearray()
    glyphs = []          # (bitmapOffset, w, h, xOff, yOff, xAdv)
    widths_check = {}
    for code in range(FIRST, LAST + 1):
        ch = chr(code)
        bbox = font.getbbox(ch)          # 相对基线锚点 (x0,y0,x1,y1)
        adv = int(round(font.getlength(ch)))
        if bbox is None:
            glyphs.append((len(bitmap), 0, 0, 0, 0, adv))
            continue
        x0, y0, x1, y1 = bbox
        w, h = x1 - x0, y1 - y0
        img = Image.new('1', (max(w, 1), max(h, 1)), 0)
        d = ImageDraw.Draw(img)
        d.text((-x0, -y0), ch, font=font, fill=1)
        # 实际可能有空列，按真实像素再裁一次
        real = img.getbbox()
        if real is None or w == 0 or h == 0:
            glyphs.append((len(bitmap), 0, 0, 0, 0, adv))
            continue
        off0 = len(bitmap)
        px = img.load()
        byte = 0
        bits = 0
        for yy in range(h):
            for xx in range(w):
                byte <<= 1
                if px[xx, yy]:
                    byte |= 1
                bits += 1
                if bits == 8:
                    bitmap.append(byte)
                    byte = 0
                    bits = 0
        if bits:                     # 字形末尾不足 1 字节才补零
            bitmap.append(byte << (8 - bits))
        glyphs.append((off0, w, h, x0, y0 - ascent, adv))

    lines = [f'// {name}: DejaVu Sans Bold {size}px, 字符 0x{FIRST:02X}-0x{LAST:02X}', '']
    lines.append(f'static const uint8_t {name}_BITMAP[] PROGMEM = {{')
    for i in range(0, len(bitmap), 16):
        lines.append('  ' + ','.join(f'0x{b:02X}' for b in bitmap[i:i+16]) + ',')
    lines.append('};')
    lines.append('')
    lines.append(f'static const GFXglyph {name}_GLYPHS[] PROGMEM = {{')
    for off0, w, h, xo, yo, adv in glyphs:
        # 本库 GFXglyph 顺序: bitmapOffset, width, height, xAdvance, xOffset, yOffset
        lines.append('  { %d, %d, %d, %d, (int8_t)%d, (int8_t)%d },' % (off0, w, h, adv, xo, yo))
    lines.append('};')
    lines.append('')
    lines.append(f'static const GFXfont {name} PROGMEM = {{ (uint8_t *){name}_BITMAP, '
                 f'(GFXglyph *){name}_GLYPHS, 0x{FIRST:02X}, 0x{LAST:02X}, {yadv} }};')
    lines.append('')
    # 打印几个关键字符宽度，供布局参考
    for s in ['Andrea Kimi Antonelli', 'Max Verstappen', 'Hulkenberg', 'PTS', '+25.5']:
        w = sum(glyphs[ord(c) - FIRST][5] for c in s if FIRST <= ord(c) <= LAST)
        widths_check[s] = w
    print(f'{name}: yAdvance={yadv}, bitmap={len(bitmap)}B, 测试宽度: ' +
          ', '.join(f'"{k}"={v}px' for k, v in widths_check.items()))
    return '\n'.join(lines)

header = ('// 自动生成：DejaVuSans-Bold GFX 实体字体（覆盖 0x20-0xFF，含 é ü）\n'
          '#pragma once\n#include <Arduino_GFX.h>\n\n')
body = '\n'.join([
    gen_font('FONT_NAME', 26),   # 车手姓名
    gen_font('FONT_TEAM', 15),   # 车队名
    gen_font('FONT_NUM',  40),   # 大数字（年度积分/场次得分）
    gen_font('FONT_MID',  18),   # 详情页文字
])
with open(os.path.join(WS, 'fonts_custom.h'), 'w') as f:
    f.write(header + body)
sz = os.path.getsize(os.path.join(WS, 'fonts_custom.h'))
print(f'\nfonts_custom.h 已生成，{sz/1024:.1f} KB')
