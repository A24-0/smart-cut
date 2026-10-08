# Генератор GFX-шрифта с кириллицей из Arial Bold (запуск: python3 tools/gen_font.py)
from PIL import Image, ImageDraw, ImageFont

TTF = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
FIRST, LAST = 0x20, 0x44F
RANGES = [(0x20, 0x7E), (0x410, 0x44F)]

def build(name, size):
    font = ImageFont.truetype(TTF, size)
    bitmap = bytearray()
    glyphs = []
    present = {c for a, b in RANGES for c in range(a, b + 1)}
    for code in range(FIRST, LAST + 1):
        if code not in present:
            glyphs.append((0, 0, 0, 0, 0, 0))
            continue
        ch = chr(code)
        adv = int(round(font.getlength(ch)))
        img = Image.new("L", (size * 2, size * 2), 0)
        ImageDraw.Draw(img).text((size // 2, size), ch, font=font, fill=255, anchor="ls")
        img = img.point(lambda v: 255 if v > 110 else 0)
        bbox = img.getbbox()
        if not bbox:
            glyphs.append((0, 0, 0, adv, 0, 0))
            continue
        x0, y0, x1, y1 = bbox
        w, h = x1 - x0, y1 - y0
        off = len(bitmap)
        bits = []
        for yy in range(y0, y1):
            for xx in range(x0, x1):
                bits.append(1 if img.getpixel((xx, yy)) else 0)
        for i in range(0, len(bits), 8):
            chunk = bits[i:i + 8] + [0] * (8 - len(bits[i:i + 8]))
            bitmap.append(int("".join(map(str, chunk)), 2))
        glyphs.append((off, w, h, adv, x0 - size // 2, y0 - size))
    out = [f"const uint8_t {name}Bitmaps[] PROGMEM = {{"]
    out.append(",".join(str(b) for b in bitmap))
    out.append("};")
    out.append(f"const GFXglyph {name}Glyphs[] PROGMEM = {{")
    out.append(",\n".join("{%d,%d,%d,%d,%d,%d}" % g for g in glyphs))
    out.append("};")
    out.append(f"const GFXfont {name} PROGMEM = {{(uint8_t*){name}Bitmaps,(GFXglyph*){name}Glyphs,0x{FIRST:X},0x{LAST:X},{int(size*1.25)}}};")
    return "\n".join(out)

with open("include/font_ru.h", "w") as f:
    f.write("#pragma once\n// Сгенерировано tools/gen_font.py (Arial Bold, ASCII + кириллица)\n#include <Arduino.h>\n#include <gfxfont.h>\n\n")
    f.write(build("FontRu20", 20) + "\n\n")
    f.write(build("FontRu30", 30) + "\n")
