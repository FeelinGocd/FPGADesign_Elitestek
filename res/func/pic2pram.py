#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pic2pram.py —— 把 res/pic 里的图片转成 2D 加速器【素材源】要用的初值文件

干什么
    res/pic 里的图片  ──▶  8bpp 索引 + 一张 256 色调色板 + 素材描述表
    产物全部落在 res/func/output/，给 RTL 的初值文件用（生成 bit 的阶段烧进 BRAM）：
        assets.hex       素材索引数据（PRAM 初值），一行 = 一个 32 位字 = 4 个索引
        palette.hex      调色板，一行 = 一项 AARRGGBB（★ α 在高字节，低 24 位就是原来的 RRGGBB）
        assets_desc.vh   素材描述表（Verilog include，给 PaletteSource 查表用）
        assets_desc.h    同一张表的 C 头文件（软件算坐标/调试时用）
        preview_*.png    量化后的预览（肉眼确认硬件会画成什么样）
        report.txt       这次转换的账（编号/尺寸/跨距/占用/基址/调色板用量）

为什么这么做
    · 一张调色板要服务所有素材 ⇒ 先把所有图缩略图拼起来量化出【共用】256 色，
      再把这张调色板套用到每张原图上（dither 关掉，索引才是纯查表）。
    · 每行跨距向上对齐到 PIX_PER_WORD（4）⇒ 每一行都从"字边界"开始，
      源侧一拍直接取一整字 = 4 个连续像素，不需要移位器。
    · 索引数据、调色板、描述表【同一处生成】⇒ 图/表/颜色永远不会对不上。

怎么用
    python res/func/pic2pram.py                 # 用下面的默认参数
    python res/func/pic2pram.py --max 64        # 单张素材最大边缩到 64
    python res/func/pic2pram.py --budget 8192   # 只看 32KB 预算够不够（不写文件）

约定（RTL 那边按这个来）
    字 = 32 位 = 4 个 8bpp 索引；字里【低位字节 = 最左边的像素】
    调色板一项 = AARRGGBB（α 在高字节）—— 硬件把它拆成：
        低 24 位 → 像素流（一个字都不改，字节序照旧）
        高 8 位  → α 旁路 → 合成器做 alpha 混合（见 docs/spec/硬件_加速器.md §5.3）
    描述表每项 = {base_word[15:0], stride_word[15:0], width[15:0], height[15:0]}

★ 一个索引 = 一种 (颜色, α) —— 但【同一个颜色可以占多个索引】
    8bpp 只有一个索引平面，"α 是像素属性"这件事在这里就落成：α 挂在索引上。
    索引是【按 RGB】量化出来的，所以同一个颜色天然落到同一个索引；
    如果同一个颜色画了不止一档 α，本脚本会把它【拆成多个索引】
    （RGB 相同、α 各一档；见下面 main() 里的"第 2 遍"）—— 这正是
    "PRAM 只存索引、α 放调色板"这个设计的意义：同色多档 α 用多占几个索引换。
    索引 0 是 ck 的键色（钉死成纯白），它的 α 强制为 0：ck 抠掉的和 α=0 的是同一批像素。
"""

import argparse
import shutil
import sys
from pathlib import Path

# 控制台按 UTF-8 输出（Windows 默认 GBK，中文和符号会炸）
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

try:
    from PIL import Image
except ImportError:
    sys.exit("需要 Pillow：请用带 Pillow 的 python 跑（本机 E:\\Program Flies\\python\\python.exe 有 10.3.0）")

# ---------------------------------------------------------------- 配置
ROOT        = Path(__file__).resolve().parents[2]      # 工程根目录
PIC_DIR     = ROOT / "res" / "pic"                     # 输入图片根目录
#   ★★ 默认扫【哪几个目录】—— 不是 res/pic 根目录！
#     老版本默认扫根目录 ⇒ 会把根目录下的非素材图（以前那张实拍照片）也打进去，
#     调色板被带偏、素材全错（踩过一次）。
#   ★ 顺序 = 素材编号顺序：**新素材一律加在最后**，这样已有素材的编号不动。
#   ★ res/pic/_原始图/ 放的是没缩过的原图，**故意不列在这里**（不参与打包）。
PIC_DIRS_DEFAULT = [
    PIC_DIR / "character",          # 0, 1   角色跑动帧
    PIC_DIR / "base" / "num",       # 2..11  数字 0-9（HUD / 菜单）
    PIC_DIR / "alpha",              # 12     带 α 的素材（玻璃等）
    PIC_DIR / "logo",               # 13     比赛 logo（256×92，带透明）
]
OUT_DIR     = ROOT / "res" / "func" / "output"         # 产物（归档 + 给 C 用）
#   ★ Efinity 综合时，$readmemh 的相对路径是按【工程目录】解析的 —— 工程里 SoC 自己
#     那些 .bin 初值就是直接躺在工程目录里的。所以 assets.hex / palette.hex
#     除了放 output 归档，还要在【每个】Efinity 工程那边再放一份，
#     否则综合报 VERI-1012 cannot open file。
#   ★ 放【工程根】还是放【工程下的子目录】，由 RTL 里的参数决定：
#       GPU_2D.v 的 INDEX_INIT_FILE / PALETTE_INIT_FILE 写 "assets.hex"   ⇒ 放工程根；
#       写 "hex/assets.hex"                                            ⇒ 放 <工程>/hex/。
#   ★★ 每新建一个 Efinity 工程，把它的目标目录加进下面这个列表（这个坑踩过一次）。
EFX_HEX_DIRS = [
    ROOT / "project" / "verified" / "06_SoC_2DGPU" / "project",   # 已验证工程（RTL 参数 = "assets.hex"）
    ROOT / "project" / "GamingPlayUnit" / "hex",                    # 发布工程（RTL 参数 = "hex/assets.hex"）
]
EFX_PROJ_DIR = EFX_HEX_DIRS[0]        # 兼容旧代码：指"开发工程目录"
#   素材表（assets_desc.h）要给固件用，而固件的 include 路径里有 peripheral/APB0/gpu2d
#   ⇒ 也在那儿放一份（和 user_gpu2d.h 并排），这样 C 里直接 #include "assets_desc.h"
#   每个 C 工程各放一份（哪个目录不存在就跳过并 WARN）
SW_G2D_DIRS = [
    EFX_PROJ_DIR / "embedded_sw" / "SoC_RISCV32" / "software" / "standalone" \
                 / "project" / "peripheral" / "APB0" / "gpu2d",          # 已验证工程
    ROOT / "project" / "GamingPlayUnit" / "embedded_sw" / "SoC_RISCV32"
                 / "software" / "standalone" / "GamingPlayUnit"
                 / "peripheral" / "APB0" / "gpu2d",                        # 发布工程
    ROOT / "code" / "peripheral" / "APB0" / "gpu2d",                        # 仓库归档
]
SW_G2D_DIR  = SW_G2D_DIRS[0]
#   ★ 素材的"信息"（基址/宽/高/跨距）走 C 这边：软件发命令时把 src=基址、w、h 带下去，
#     硬件不查表 ⇒ 不需要生成 Verilog 的描述表（.vh 已删除）。
PRAM_WORDS  = 32768        # PRAM = 128KB = 32768 字 × 4 字节（BlockRAM: 32 位 × 2^15）
PIX_PER_WORD = 4           # 一个字装 4 个索引（32 位 / 8 位）
PAL_ENTRIES = 256          # 调色板项数（8bpp）
PAD_INDEX   = 0            # 行尾补齐用什么索引（0 = 调色板第 0 项 = 白且 α=0，天然透明）
TRANS_TH    = 128          # 原图 α 小于它就当"透明像素"：落成索引 0（白 + α=0）
THUMB       = 64           # 训练调色板时每张图缩到多大（越小越快，够用就行）

IMG_EXT = {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp"}


def fit(im: Image.Image, max_w: int, max_h: int) -> Image.Image:
    """按比例缩到 max_w × max_h 以内（不放大）。"""
    w, h = im.size
    s = min(max_w / w, max_h / h, 1.0)
    if s < 1.0:
        im = im.resize((max(1, int(w * s)), max(1, int(h * s))), Image.LANCZOS)
    return im


def build_palette(images):
    """所有素材共用一张 256 色调色板：缩略图拼一张大图再量化。"""
    thumbs = [fit(im, THUMB, THUMB) for im in images]
    W = sum(t.size[0] for t in thumbs) + len(thumbs) + 1
    H = max(t.size[1] for t in thumbs) + 2
    mosaic = Image.new("RGB", (W, H), (255, 0, 255))       # 分隔色，避免被当成素材颜色
    x = 1
    for t in thumbs:
        mosaic.paste(t, (x, 1))
        x += t.size[0] + 1
    pal_img = mosaic.quantize(colors=PAL_ENTRIES, method=Image.MEDIANCUT, dither=Image.NONE)
    return pal_img


def to_indices(im: Image.Image, pal_img: Image.Image):
    """套用共用调色板 → 8bpp 索引。"""
    return im.quantize(palette=pal_img, dither=Image.NONE)


def pack_asset(q: Image.Image):
    """把索引图打包成 32 位字：低位字节 = 最左像素；行跨距对齐到 PIX_PER_WORD。

    ★ 返回两个跨距，别混：
        stride_px    —— 行跨距（像素），打包时按它补齐（向上对齐到 4 像素）
        stride_word  —— 行跨距（字）= stride_px / 4  ← 描述表里要的是【这个】
      硬件每行加的跨距单位是【字】（一个字 = 4 个索引）。
      写错的表现：给成像素跨距 ⇒ 源每行多跳 4 倍 ⇒ 图在 y 方向被压扁到 1/4。
    """
    w, h = q.size
    stride = ((w + PIX_PER_WORD - 1) // PIX_PER_WORD) * PIX_PER_WORD   # 向上对齐到 4（像素）
    px = list(q.getdata())
    words = []
    for y in range(h):
        row = px[y * w:(y + 1) * w]
        row = row + [PAD_INDEX] * (stride - w)                         # 行尾补齐
        for i in range(0, stride, PIX_PER_WORD):
            b0, b1, b2, b3 = row[i], row[i + 1], row[i + 2], row[i + 3]
            words.append((b3 << 24) | (b2 << 16) | (b1 << 8) | b0)     # 低位 = 最左
    return words, stride, stride // PIX_PER_WORD


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max", type=int, default=256,
                    help="单张素材最大边（默认 256）。★ 素材尺寸最好在绘图工具里就定好："
                         "这里一旦触发缩放，α 会被 LANCZOS 弄成连续值，"
                         "而硬件是\"一个索引 = 一种 (颜色, α)\" ⇒ 调色板项会被拆爆。")
    ap.add_argument("--budget", type=int, default=PRAM_WORDS, help="PRAM 字数上限（默认 16384 = 64KB）")
    ap.add_argument("--dry", action="store_true", help="只算账，不写文件")
    ap.add_argument("--dir", default=None, nargs="+",
                    help="输入图片目录（★ 可以给多个，按命令行顺序编号。"
                         "例：--dir res/pic/character res/pic/base/num res/pic/alpha）。"
                         "默认 = PIC_DIRS_DEFAULT 那三个目录（★ 不是 res/pic 根目录）；相对路径按工程根算。")
    args = ap.parse_args()

    if args.dir:
        pic_dirs = [Path(d) if Path(d).is_absolute() else (ROOT / d) for d in args.dir]
    else:
        pic_dirs = [d for d in PIC_DIRS_DEFAULT if d.is_dir()]
        if not pic_dirs:
            sys.exit("默认素材目录一个都不存在：" + ", ".join(str(d) for d in PIC_DIRS_DEFAULT)
                     + "\n（用 --dir 显式指定，或检查 res/pic 是不是被挪走了）")

    for d in pic_dirs:
        if not d.is_dir():
            sys.exit(f"输入目录不存在：{d}")

    #   ★ 多目录：按【命令行给的顺序】依次编号 —— 先给的目录里的图编号在前。
    #     这样"跑步帧"永远是 0/1，数字素材跟在后面，C 那边的下标不受影响。
    #   ★ 每次运行都会【整个重写】assets.hex/palette.hex ⇒ 所有素材必须一次跑完,
    #     不能分几次跑(后一次会把前一次覆盖掉)。
    files = []
    for d in pic_dirs:
        files += sorted(p for p in d.iterdir() if p.suffix.lower() in IMG_EXT)
    if not files:
        sys.exit("这些目录里没有图片（支持 " + ", ".join(sorted(IMG_EXT)) + "）："
                 + ", ".join(str(d) for d in pic_dirs))
    print("输入目录：" + ", ".join(str(d) for d in pic_dirs))

    raw = []
    for p in files:
        im = Image.open(p)
        if im.mode in ("RGBA", "LA") or (im.mode == "P" and "transparency" in im.info):
            im = im.convert("RGBA")
            alpha = im.getchannel("A")
            rgb = im.convert("RGB")
        else:
            im = im.convert("RGB")
            alpha = Image.new("L", im.size, 255)
            rgb = im
        #   ★ 颜色键控(ck)的约定是【白 = 透明】，所以透明像素必须先落成白 ——
        #     原来直接 convert("RGB") 是把 alpha 丢掉，那要看原图透明处的 RGB
        #     恰好是不是白，不保证。
        #   ★ 但【半透明】的地方不能再混到白底上：那会把颜色冲淡，硬件再拿 α 去混
        #     就会发白（α 档位就白设计了）。所以只有 α < TRANS_TH 的像素落成白，
        #     其余【保留原色】，透明度交给调色板里的 α 去表达。
        #     （纯硬边素材 α 只有 0/255 ⇒ 这里和"整体混到白底"结果一模一样，无副作用。）
        tmask = alpha.point(lambda v: 255 if v < TRANS_TH else 0)
        im = Image.composite(Image.new("RGB", im.size, (255, 255, 255)), rgb, tmask)
        im.load()
        alpha.load()
        raw.append((p.name, im, alpha))

    pal_img = build_palette([im for _, im, _ in raw])
    #   ★ ck 是"比纯白"，所以调色板第 0 项【钉死成 ffffff】，透明像素全部指到它。
    #     量化器不保证这一点（它可能把白分成好几档：fffffa / fbffff ... 都不是纯白），
    #     钉死之后"透明"和"键色"就永远对得上。
    pal_list = pal_img.getpalette()
    pal_list[0:3] = [255, 255, 255]
    pal_img.putpalette(pal_list)
    pal_rgb = pal_img.getpalette()[:PAL_ENTRIES * 3]
    pal_rgb += [0] * (PAL_ENTRIES * 3 - len(pal_rgb))

    #── 第 1 遍：收下每张图的【索引 + α】（先不打包 —— 打包前要先定调色板怎么分项）
    assets = []
    for name, im, alpha in raw:
        im2 = fit(im, args.max, args.max)
        a2 = fit(alpha, args.max, args.max)
        q = to_indices(im2, pal_img)

        #   ★ 透明像素（alpha < TRANS_TH）一律改成索引 0 = 纯白 ⇒ ck 一定能抠掉它，
        #     而且索引 0 的 α 也是 0 ⇒ 不开 ck 只开 alpha 时也照样透明。
        #     不看原图透明处的 RGB，也不用指望量化结果。
        qd = list(q.getdata())
        ad = list(a2.getdata())
        for k in range(len(qd)):
            if ad[k] < TRANS_TH:
                qd[k] = 0

        #   ★ ck 的键色是【白】：不透明区里只要有一个纯白像素，它就会被当成透明抠掉。
        #     这里数出来报给用户 —— 要么改素材，要么以后把键色做成 APB 可配。
        n_white = 0
        for k, ix in enumerate(qd):
            if ad[k] >= TRANS_TH and pal_rgb[ix*3] == 255 and pal_rgb[ix*3+1] == 255 and pal_rgb[ix*3+2] == 255:
                n_white += 1
        n_trans = sum(1 for v in ad if v < TRANS_TH)
        safe = "".join(ch if (ch.isascii() and (ch.isalnum() or ch in "-_")) else "_" for ch in Path(name).stem)
        if not any(c.isalnum() for c in safe):        # 中文名会被压成 "___"，退回编号
            safe = f"asset{len(assets)}"
        assets.append({
            "name": name,
            "safe": safe,
            "w": q.size[0], "h": q.size[1],
            "q": q, "qd": qd, "ad": ad,
            "n_white": n_white,     # 不透明区里的纯白像素数（会被 ck 抠掉，必须为 0）
            "n_trans": n_trans,     # 透明像素数（= 会被 ck 抠掉的量）
        })

    #── 第 2 遍：同一个颜色有多个 α ⇒ 拆成多个调色板项 ────────────────────────
    #   ★ 为什么要拆：索引是【按 RGB】量化出来的（同一个颜色天然落到同一个索引），
    #     而 α 挂在索引上（一项 = 一种 (颜色, α)）⇒ 不拆的话同一颜色的两档 α 只能留一个，
    #     半透就没了。硬件本来就支持"同色多档 α"，只要它占多个索引 —— 256 项就是干这个的。
    #   ★ 怎么拆：最大那档 α 留在原索引（单档时 = 老行为，一个字节都不变），
    #     其余每档各占一个【没用过的】索引：RGB 抄原来的，α 换成自己那档。
    lvl = {}
    for a in assets:
        for ix, av in zip(a["qd"], a["ad"]):
            lvl.setdefault(ix, set()).add(av)
    lvl.pop(PAD_INDEX, None)                # 索引 0 = ck 键色(白)，α 钉死 0，不参与拆分
    used_pre = set(lvl.keys())
    free = [i for i in range(PAL_ENTRIES) if (i not in used_pre) and (i != PAD_INDEX)]

    pal_alpha = [0] * PAL_ENTRIES
    remap = {}                              # (原索引, α) -> 最终索引
    splits = []                             # (原索引, 新索引, α) —— 只为报告
    for ix, s in lvl.items():
        keep = max(s)
        pal_alpha[ix] = keep
        remap[(ix, keep)] = ix
        for av in sorted(s):
            if av == keep:
                continue
            if not free:
                #   ★ 报错时把"谁在拆"也说出来：多半是某张图被缩放过、α 成了连续值
                culprits = []
                for ix2, s2 in sorted(lvl.items(), key=lambda kv: -len(kv[1]))[:5]:
                    culprits.append(f"索引 {ix2} 有 {len(s2)} 档 α")
                sys.exit(f"[ERR] 调色板放不下：拆\"同色多档 α\"要多占项 —— 已用 {len(used_pre)} 项、"
                         f"还要拆 {len(splits)} 项。把同色的 α 档位减少一些，或调大 PAL_ENTRIES\n"
                         "      α 档位最多的几项：\n        " + "\n        ".join(culprits) +
                         "\n      多半是某张素材的 α 是连续值（被缩放/抗锯齿过）。"
                         "素材请在绘图工具里做成硬边（α 只有 0/128/255 这种档位），或别让 --max 触发缩放。")
            ni = free.pop(0)
            pal_rgb[ni*3    ] = pal_rgb[ix*3    ]       # RGB 抄原来的
            pal_rgb[ni*3 + 1] = pal_rgb[ix*3 + 1]
            pal_rgb[ni*3 + 2] = pal_rgb[ix*3 + 2]
            pal_alpha[ni] = av
            remap[(ix, av)] = ni
            splits.append((ix, ni, av))
    pal_alpha[PAD_INDEX] = 0                # 索引 0：键色(白) + 永远全透明

    #── 第 3 遍：按最终索引重打包 ────────────────────────────────────────────
    base = 0
    for a in assets:
        q2 = a["q"].copy()
        q2.putdata([remap.get((ix, av), ix) for ix, av in zip(a["qd"], a["ad"])])
        words, stride, stride_word = pack_asset(q2)
        a["q"]           = q2
        a["words_data"]  = words
        a["words"]       = len(words)
        a["stride"]      = stride          # 像素（打包/对齐用）
        a["stride_word"] = stride_word     # 字 —— 描述表 + 驱动用这个
        a["base"]        = base
        base += len(words)

    total = base
    print(f"输入 {len(files)} 张，最大边 {args.max}，调色板 {PAL_ENTRIES} 色")
    print(f"{'#':>3}  {'文件':<22} {'尺寸':>10} {'跨距':>5} {'字数':>7} {'基址':>7}  {'占用':>8}")
    for i, a in enumerate(assets):
        print(f"{i:>3}  {a['name'][:20]:<22} {a['w']:>4}x{a['h']:<5} {a['stride_word']:>5} "
              f"{a['words']:>7} {a['base']:>7}  {a['words']*4/1024:>7.1f}KB")
    print(f"合计 {total} 字 = {total*4/1024:.1f} KB / 预算 {args.budget} 字 = {args.budget*4/1024:.1f} KB")

    used_idx = set(sum([list(a["q"].getdata()) for a in assets], []))
    used = len(used_idx)
    print(f"实际用到调色板项数：{used} / {PAL_ENTRIES}")

    #   ---- α 自检（alpha 混合的素材约定）----
    #   一个索引 = 一种 (颜色, α) —— 所以"半透明档位"必须数得出来：
    #   如果这里报出几百个半透明索引，说明素材用了连续渐变的透明度，256 项会不够用。
    lv      = sorted(set(pal_alpha[i] for i in used_idx))
    n_half  = sum(1 for i in used_idx if 0 < pal_alpha[i] < 255)
    print(f"alpha：α 档位 {lv}；半透明(0/255 之外)的索引 {n_half} 个")
    if n_half > 32:
        print(f"  [WARN] 半透明索引有 {n_half} 个 —— 素材大概用了连续渐变；"
              f"建议改成固定档位（0/128/255）")
    if splits:
        print(f"同色多档 α：拆了 {len(splits)} 项（同一个颜色占了多个索引）")
        for ix, ni, av in splits[:12]:
            print(f"    #{pal_rgb[ix*3]:02x}{pal_rgb[ix*3+1]:02x}{pal_rgb[ix*3+2]:02x}"
                  f"  索引 {ix} → {ni}   α={av}")
        if len(splits) > 12:
            print(f"    ...（还有 {len(splits) - 12} 项）")

    #   ---- 颜色键控(ck)自检：键色 = 白 ----
    print("ck(键色=白) 自检：")
    bad_white = 0
    for i, a in enumerate(assets):
        flag = "" if a["n_white"] == 0 else "   <<< 不透明区里有纯白, 会被 ck 抠掉!"
        if a["n_white"]:
            bad_white += 1
        print(f"  {i:>3} {a['name'][:20]:<22} 透明 {a['n_trans']:>6} 像素, 不透明纯白 {a['n_white']:>6} 像素{flag}")
    if bad_white:
        print(f"  [WARN] {bad_white} 张素材的不透明区里有纯白 —— 上板会看到那些地方被抠掉。")

    if total > args.budget:
        sys.exit(f"[ERR] 放不下：需要 {total} 字，预算 {args.budget} 字 —— 把 --max 调小或加预算")
    if args.dry:
        print("(--dry：没有写文件)")
        return

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for old in OUT_DIR.glob("preview_*.png"):       # 清掉上次留下的预览, 免得看不出哪张是新的
        old.unlink()

    # ---- assets.hex：PRAM 初值 ----
    with open(OUT_DIR / "assets.hex", "w", encoding="ascii") as f:
        for a in assets:
            for wd in a["words_data"]:
                f.write(f"{wd:08x}\n")

    # ---- palette.hex：调色板（★ 一项 = AARRGGBB，α 在高字节）----
    #   低 24 位就是原来的 RRGGBB ⇒ "低位字节 = B"这条全工程约定一点没变；
    #   高 8 位是 α，由 PaletteDecoder 拆出来走旁路给合成器（docs/spec/硬件_加速器.md §5.3）。
    #   ★ 索引 0 的 α 强制为 0：它就是 ck 的键色（纯白），
    #     "被 ck 抠掉"和"α=0 透明"因此是同一批像素。
    #   ★ 这项改动必须和 RTL 的 32 位调色板【同时上】：还是一行 6 位的话，
    #     $readmemh 会把高 8 位读成 0 ⇒ 所有像素 α=0、整屏透明（规格 §9 末尾那条警告）。
    pal = pal_img.getpalette()[:PAL_ENTRIES * 3]
    pal += [0] * (PAL_ENTRIES * 3 - len(pal))
    pal_alpha[PAD_INDEX] = 0
    with open(OUT_DIR / "palette.hex", "w", encoding="ascii") as f:
        for i in range(PAL_ENTRIES):
            r, g, b = pal[i*3], pal[i*3+1], pal[i*3+2]
            f.write(f"{pal_alpha[i]:02x}{r:02x}{g:02x}{b:02x}\n")

    # ---- assets_desc.h：素材信息交给 C（软件发命令时把 base/宽/高带下去）----
    #   硬件不查表 ⇒ 不再生成 Verilog 描述表（.vh）。
    #   ★ 生成文件里注释一律 ASCII：它要被 C 编译器直接吃，中文在不保证 UTF-8 的环境会乱。
    with open(OUT_DIR / "assets_desc.h", "w", encoding="ascii") as f:
        f.write("/* generated by res/func/pic2pram.py -- DO NOT EDIT */\n")
        f.write("#pragma once\n\n")
        f.write(f"#define ASSET_NUM   {len(assets)}\n\n")
        f.write("typedef struct {\n"
                "    unsigned int base_word;    /* first word in PRAM -> BLIT cmd src */\n"
                "    unsigned int stride_word;  /* words per row = (width+3)/4 -> BLIT color */\n"
                "    unsigned int width;        /* pixels -> BLIT cmd w */\n"
                "    unsigned int height;       /* pixels -> BLIT cmd h */\n"
                "    const char  *name;\n"
                "} asset_desc_t;\n\n")
        f.write("static const asset_desc_t g_assets[] = {\n")
        for a in assets:
            f.write(f"    {{ {a['base']:>6}u, {a['stride_word']:>4}u, {a['w']:>4}u, {a['h']:>4}u, "
                    f"\"{a['safe']}\" }},\n")
        f.write("};\n")

    # ---- 预览图 + 账本 ----
    for i, a in enumerate(assets):
        a["q"].convert("RGB").save(OUT_DIR / f"preview_{i}_{a['safe']}.png")

    with open(OUT_DIR / "report.txt", "w", encoding="utf-8-sig") as f:
        f.write("输入目录 : " + ", ".join(str(d) for d in pic_dirs) + "\n")
        f.write(f"输出目录 : {OUT_DIR}\n")
        f.write(f"最大边   : {args.max}   调色板: {PAL_ENTRIES} 色   实际用到: {used}\n")
        f.write(f"PRAM     : 每字 {PIX_PER_WORD} 个索引, 共 {total} 字 = {total*4/1024:.1f} KB\n\n")
        f.write(f"{'#':>3}  {'文件':<24} {'宽':>4} {'高':>4} {'跨距':>5} {'字数':>7} {'基址':>7}"
                f" {'透明':>7} {'不透明纯白':>10}\n")
        for i, a in enumerate(assets):
            f.write(f"{i:>3}  {a['name'][:22]:<24} {a['w']:>4} {a['h']:>4} {a['stride_word']:>5} "
                    f"{a['words']:>7} {a['base']:>7} {a['n_trans']:>7} {a['n_white']:>10}\n")
        f.write("\nck(键色=白): 不透明纯白必须为 0, 否则那些像素会被抠掉。\n")
        f.write("\nalpha 混合: palette.hex 一项 = AARRGGBB(α 在高字节),"
                " α 取该索引下所有像素的最大 α;\n"
                "            索引 0 是键色(白), 它的 α 强制为 0。\n")
        f.write(f"  α 档位: {lv}    半透明(0/255 之外)的索引: {n_half} 个\n")
        if splits:
            f.write(f"\n同一个颜色占了多个索引(拆出来的 {len(splits)} 项):\n")
            for ix, ni, av in splits:
                f.write(f"    #{pal_rgb[ix*3]:02x}{pal_rgb[ix*3+1]:02x}{pal_rgb[ix*3+2]:02x}"
                        f"   索引 {ix} → {ni}   α={av}\n")

    # ---- 调色板只有【一份】palette.hex ----
    #   PaletteLUT 的 4 个读口共用它：读口 → 输出 lane 的对应在 RTL 里是写死的,
    #   综合器没有重排的余地, 所以不需要"拆成 4 个文件名"去骗它。

    # ---- 固件要用的那一份：素材表放到 gpu2d 驱动目录（在 include 路径里）----
    for d in SW_G2D_DIRS:
        if d.is_dir():
            shutil.copyfile(OUT_DIR / "assets_desc.h", d / "assets_desc.h")
            print(f"[OK] 固件用素材表已放好：{d}")
        else:
            print(f"[WARN] 找不到固件目录 {d}，assets_desc.h 没放过去")

    # ---- 综合要用的那一份：每个 Efinity 工程都要放（放哪由 RTL 的 INIT_FILE 参数决定）----
    for d in EFX_HEX_DIRS:
        proj = d.parent                     # ★ 工程目录本身不存在就别建了（写错路径能立刻看出来）
        if not proj.is_dir():
            print(f"[WARN] 找不到工程目录 {proj}，{d.name}/ 下的 hex 没放过去")
            continue
        d.mkdir(parents=True, exist_ok=True)   # 目标子目录不存在就建（例如 <工程>/hex/）
        for n in ("assets.hex", "palette.hex"):
            shutil.copyfile(OUT_DIR / n, d / n)
        print(f"[OK] 综合用副本已放好：{d}")

    print(f"[OK] 产物写好了：{OUT_DIR}")
    for n in ("assets.hex", "palette.hex", "assets_desc.h", "report.txt"):
        print(f"    {n}")


if __name__ == "__main__":
    main()
