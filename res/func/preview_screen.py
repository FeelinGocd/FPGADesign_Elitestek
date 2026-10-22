# -*- coding: utf-8 -*-
"""按【真实生成的 assets.hex + palette.hex】离线渲染 demo_glass 会画出来的画面。
产物:
  output/alpha_demo_tile.png   一块 32x32 玻璃: alpha 混合 vs ck 硬边, 放大 8 倍
  output/alpha_demo_screen.png 全高 1x 长条: 左 256 像素 = ck 条, 右 = alpha 区
"""
import sys
from pathlib import Path
from PIL import Image

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
OUT = Path(__file__).resolve().parents[2] / "res" / "func" / "output"

GLASS_BASE, GLASS_STRIDE, TILE = 6726, 8, 32
BAND_H, BANDS = 270, [(0x18,0x20,0x28), (0x2A,0x34,0x40), (0x3E,0x4A,0x58), (0x55,0x63,0x6F)]
REF_W = 256

pal = []
for line in (OUT / "palette.hex").read_text().split():
    v = int(line, 16)
    pal.append(((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, (v >> 24) & 0xFF))
words = [int(x, 16) for x in (OUT / "assets.hex").read_text().split()]

def tile_px(dx, dy):
    w = words[GLASS_BASE + dy * GLASS_STRIDE + (dx >> 2)]
    return pal[(w >> ((dx % 4) * 8)) & 0xFF]

def base_at(x, y):
    return BANDS[min(y // BAND_H, len(BANDS) - 1)]

def blend(fg, bg, a):
    out = []
    for f, b in zip(fg, bg):
        d = f - b
        sign = 1 if d > 0 else -1
        p = abs(d) * a
        out.append(max(0, min(255, b + sign * ((p + 1 + (p >> 8)) >> 8))))
    return tuple(out)

def px_alpha(x, y):
    b = base_at(x, y)
    r, g, bl, a = tile_px(x % TILE, y % TILE)
    return blend((r, g, bl), b, a)

def px_ck(x, y):
    b = base_at(x, y)
    r, g, bl, a = tile_px(x % TILE, y % TILE)
    return b if (r, g, bl) == (255, 255, 255) else (r, g, bl)

# ---- ① 一块玻璃的 A/B 放大 8 倍（底色取中间那条带）----
Z = 8
im = Image.new("RGB", (TILE * Z * 2 + 24, TILE * Z), (24, 24, 28))
for dy in range(TILE):
    for dx in range(TILE):
        b = BANDS[1]
        r, g, bl, a = tile_px(dx, dy)
        im.paste(blend((r, g, bl), b, a), (dx * Z, dy * Z, dx * Z + Z, dy * Z + Z))
        im.paste((b if (r, g, bl) == (255, 255, 255) else (r, g, bl)),
                 (TILE * Z + 24 + dx * Z, dy * Z, TILE * Z + 24 + dx * Z + Z, dy * Z + Z))
im.save(OUT / "alpha_demo_tile.png")
print("写出:", OUT / "alpha_demo_tile.png", im.size, " 左=alpha混合  右=ck硬边")

# ---- ② 全高 1x 长条（含 4 条色带）----
W, H = 512, 1080
im2 = Image.new("RGB", (W, H * 2 + 8), (24, 24, 28))
for y in range(H):
    for x in range(W):
        im2.putpixel((x, y), px_alpha(x, y) if x >= REF_W else px_ck(x, y))
        im2.putpixel((x, H + 8 + y), px_ck(x, y))
im2.save(OUT / "alpha_demo_screen.png")
print("写出:", OUT / "alpha_demo_screen.png", im2.size, " 上=左ck+右alpha  下=全ck")

# ---- 统计：一格里有几个像素"两种画法不同"（= 半透像素）----
n = sum(1 for dy in range(TILE) for dx in range(TILE) if tile_px(dx, dy)[3] not in (0, 255))
print("一格 32x32 里半透明(0<a<255)的像素数 =", n)