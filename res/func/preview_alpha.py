# -*- coding: utf-8 -*-
"""把一张素材按硬件那条公式叠在棋盘格上, 出几张不同 alpha 的对比预览。
用法: uv run res/func/preview_alpha.py <png> [alpha1 alpha2 ...]
公式和硬件一致: out = bg + (fg - bg) * a / 255   (每通道, 整数, 四舍五入用 (p+1+(p>>8))>>8 同款)
"""
import sys
from PIL import Image

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

src = sys.argv[1]
alphas = [int(x) for x in sys.argv[2:]] or [0, 128, 255]

im = Image.open(src).convert("RGBA")
w, h = im.size
px = list(im.getdata())

# 只把"原本不透明"的像素按给定 alpha 处理; 透明像素保持透明
colors = sorted({(r, g, b) for (r, g, b, a) in px if a >= 128})
print("原图不透明像素的颜色:", ["#%02x%02x%02x" % c for c in colors])

# 棋盘格背景(两种灰), 8 像素一格
BG = [(96, 96, 112), (208, 208, 216)]
GAP = 8
W = len(alphas) * (w + GAP) + GAP
H = h + 2 * GAP
out = Image.new("RGB", (W, H), (40, 40, 48))

def blend(fg, bg, a):
    d = fg - bg
    sign = 1 if d > 0 else -1
    p = abs(d) * a
    q = (p + 1 + (p >> 8)) >> 8
    v = bg + sign * q
    return max(0, min(255, v))

for k, a in enumerate(alphas):
    tile = Image.new("RGB", (w, h))
    tp = []
    for y in range(h):
        for x in range(w):
            r, g, b, pa = px[y * w + x]
            bg = BG[((x // 8) + (y // 8)) % 2]
            if pa < 128:
                tp.append(bg)                      # 原图透明 -> 背景原样
            else:
                aa = a                          # 这一格用统一 alpha 演示
                tp.append(tuple(blend(fg, bgc, aa) for fg, bgc in zip((r, g, b), bg)))
    tile.putdata(tp)
    out.paste(tile, (GAP + k * (w + GAP), GAP))
    print(f"  第 {k+1} 格: 不透明像素按 alpha={a} 混合")

dst = r"E:\project\FPGA_Project\FPGA_Elitestek\FPGADesign_Elitestek\res\func\output\alpha_demo_glass.png"
out = out.resize((W * 4, H * 4), Image.NEAREST)   # 放大 4 倍好肉眼看
out.save(dst)
print("写好:", dst, out.size)