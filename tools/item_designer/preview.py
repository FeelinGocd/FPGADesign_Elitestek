#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
preview.py -- 道具预览/CSV→PNG/C 片段（designer 的一部分）

读 tools/item_designer/items.csv，生成一张道具预览图（默认 docs/spec/道具预览.png），
并可用 --emit-c 打印可直接贴回 game_item.c 的 g_item_defs[] 初始化片段。

只改 CSV / 运行本脚本不会改 C 代码；C 里的 g_item_defs 仍是最终权威。
"""

import argparse
import csv
import json
import math
import re
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("需要 Pillow：python -m pip install pillow")

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CSV = Path(__file__).resolve().with_name("items.csv")
DEFAULT_OUT = ROOT / "docs" / "spec" / "道具预览.png"
ICON_DIR = ROOT / "res" / "pic" / "accessory"
BASE_PNG = ICON_DIR / "accessory_base.png"
HEART_PNG = ROOT / "res" / "pic" / "base" / "specialEffect" / "SpecialEffect_heart.png"
ASSETS_H = ROOT / "code" / "peripheral" / "APB0" / "gpu2d" / "assets_desc.h"

CARD_W, CARD_H, GAP, MARGIN, COLS = 300, 270, 14, 18, 3
BG = (10, 12, 18, 255)
CARD = (20, 24, 32, 255)
CARD_EDGE = (70, 80, 96, 255)
CAND_EDGE = (210, 140, 40, 255)
TEXT = (230, 235, 245, 255)
DIM = (150, 160, 175, 255)
ACCENT = (120, 210, 255, 255)
WARN = (255, 170, 80, 255)


def font(size, bold=False, cjk=False):
    if cjk:
        names = ["msyh.ttc", "simhei.ttf", "simsun.ttc"]
    else:
        names = ["consola.ttf", "segoeui.ttf", "arial.ttf"]
        if bold:
            names = ["consolab.ttf", "segoeuib.ttf", "arialbd.ttf"] + names
    for n in names:
        p = Path("C:/Windows/Fonts") / n
        if p.exists():
            try:
                return ImageFont.truetype(str(p), size)
            except Exception:
                pass
    return ImageFont.load_default()


def wrap(draw, text, fnt, width):
    words = str(text).split()
    if not words:
        return [""]
    lines, cur = [], words[0]
    for w in words[1:]:
        cand = cur + " " + w
        if draw.textlength(cand, font=fnt) <= width:
            cur = cand
        else:
            lines.append(cur)
            cur = w
    lines.append(cur)
    return lines


def draw_wrapped(draw, xy, text, fnt, fill, width, max_lines=3, line_gap=2):
    x, y = xy
    lines = wrap(draw, text, fnt, width)
    if len(lines) > max_lines:
        lines = lines[:max_lines]
        lines[-1] = lines[-1][:max(0, len(lines[-1]) - 1)] + "..."
    for line in lines:
        draw.text((x, y), line, font=fnt, fill=fill)
        y += fnt.size + line_gap
    return y


def effect_text(row):
    parts = []
    bullet = (row.get("bullet") or "").strip()
    bullet_name = {"CHILI": "辣椒", "URANUS": "天王星"}
    if bullet and bullet != "-":
        parts.append("弹种 " + bullet_name.get(bullet, bullet))
    if int(row.get("triple") or 0):
        parts.append("增加弹幕")
    if int(row.get("lives") or 0):
        parts.append("加命 +%d" % int(row.get("lives") or 0))
    if int(row.get("hp") or 0):
        parts.append("回血 +%d" % int(row.get("hp") or 0))
    if int(row.get("hp_max") or 0):
        parts.append("生命上限 +%d" % int(row.get("hp_max") or 0))
    if int(row.get("coins") or 0):
        parts.append("金币 +%d" % int(row.get("coins") or 0))
    for key, label in (("move_v8", "移速"), ("fire_every", "射速"),
                       ("bullet_speed", "弹速"), ("range", "射程"), ("damage", "伤害")):
        v = int(row.get(key) or 0)
        if v:
            parts.append("%s %+d" % (label, v))
    return " / ".join(parts) if parts else "无属性变化"


def parse_header_assets():
    names = set()
    if ASSETS_H.exists():
        text = ASSETS_H.read_text(encoding="ascii", errors="ignore")
        names = set(re.findall(r"\bASSET_[A-Za-z0-9_]+\s*=", text))
        names = {n.split("=")[0].strip() for n in names}
    return names


def parse_features(value):
    if isinstance(value, list):
        return [str(x).strip() for x in value if str(x).strip()]
    s = str(value or "").strip()
    if not s:
        return []
    try:
        arr = json.loads(s)
        if isinstance(arr, list):
            return [str(x).strip() for x in arr if str(x).strip()]
    except Exception:
        pass
    return [s]


def load_sprite(path, scale):
    if not path.exists():
        return None
    im = Image.open(path).convert("RGBA")
    return im.resize((im.width * scale, im.height * scale), Image.NEAREST)


def add_card(page, x, y, row, base_img, icon_img, enabled, header_assets):
    d = ImageDraw.Draw(page)
    edge = CARD_EDGE if enabled else CAND_EDGE
    d.rounded_rectangle([x, y, x + CARD_W - 1, y + CARD_H - 1], radius=8,
                        fill=CARD, outline=edge, width=2)

    # 2x composite: icon floats above the shared base, like the game
    sx, sy = x + 18, y + 120
    if icon_img is not None:
        page.alpha_composite(icon_img, (sx, sy - 66))
    if base_img is not None:
        page.alpha_composite(base_img, (sx, sy))

    tx = x + 100
    tw = CARD_W - 112
    f_key = font(15, True)
    f_desc = font(13, False)
    f_eff = font(12, False, cjk=True)
    f_meta = font(11, False, cjk=True)
    f_note = font(10, False, cjk=True)

    key = row.get("key", "?")
    d.text((tx, y + 12), key, font=f_key, fill=ACCENT)
    try:
        lv = int(row.get("level") or 0)
    except ValueError:
        lv = 0
    qnames = ["普通", "稀有", "史诗", "传说"]
    qcolors = [DIM, (90, 169, 255, 255), (192, 123, 255, 255), WARN]
    if lv < 0 or lv > 3:
        lv = 0
    d.text((x + CARD_W - 64, y + 14), qnames[lv], font=f_meta, fill=qcolors[lv])
    if not enabled:
        d.text((x + CARD_W - 64, y + 28), "候选", font=f_meta, fill=WARN)
    if str(row.get("pool", "1")) != "1":
        d.text((x + CARD_W - 64, y + 42), "不入池", font=f_meta, fill=DIM)
    if str(row.get("life_plus", "0")) == "1":
        d.text((x + CARD_W - 64, y + 56), "生命保险", font=f_meta, fill=(255, 155, 106, 255))

    yy = y + 38
    yy = draw_wrapped(d, (tx, yy), row.get("desc", ""), f_desc, TEXT, tw, max_lines=2)
    yy += 8
    d.text((tx, yy), "效果", font=f_meta, fill=DIM); yy += 14
    yy = draw_wrapped(d, (tx, yy), effect_text(row), f_eff, TEXT, tw, max_lines=3)
    yy += 6
    meta = "权重 2>1  商店 15" if enabled else "权重 -  商店 -"
    d.text((tx, yy), meta, font=f_meta, fill=DIM)

    feats = parse_features(row.get("features", ""))
    if feats:
        d.text((tx, y + 188), "功能", font=f_meta, fill=DIM)
        draw_wrapped(d, (tx, y + 200), " / ".join(feats), f_note, TEXT, tw, max_lines=2)

    note = row.get("note", "")
    if note:
        draw_wrapped(d, (tx, y + CARD_H - 38), note, f_note, DIM, tw, max_lines=2, line_gap=1)

    # warning if the icon asset is not in the generated PRAM table
    asset = row.get("icon_asset", "")
    if enabled and header_assets and asset not in header_assets:
        d.text((tx, y + CARD_H - 20), "素材未进 assets_desc.h", font=f_note, fill=WARN)


def render(rows, out_path, emit_c=False):
    header_assets = parse_header_assets()
    enabled_rows = [r for r in rows if int(r.get("enabled") or 0)]
    cards = rows + [{"key": "SHOP_HEART", "desc": "商店回血", "enabled": "1",
                     "note": "回 1 格血 / 价格 5 币 / 不进道具池 / 24x24"}]
    nrows = math.ceil(len(cards) / COLS)
    W = MARGIN * 2 + COLS * CARD_W + (COLS - 1) * GAP
    H = 80 + nrows * CARD_H + (nrows - 1) * GAP + 52
    page = Image.new("RGBA", (W, H), BG)
    d = ImageDraw.Draw(page)

    d.text((MARGIN, 16), "道具规范预览", font=font(24, True, cjk=True), fill=TEXT)
    d.text((MARGIN, 48), "编辑: tools/item_designer/items.csv    运行: python tools/item_designer/preview.py",
           font=font(11, cjk=True), fill=DIM)
    d.text((W - 130, 18), "由 CSV 生成", font=font(10, cjk=True), fill=DIM)

    base_img = load_sprite(BASE_PNG, 2)
    for i, row in enumerate(cards):
        cx = MARGIN + (i % COLS) * (CARD_W + GAP)
        cy = 80 + (i // COLS) * (CARD_H + GAP)
        if row.get("key") == "SHOP_HEART":
            # heart card: no base, 24x24 icon at 3x
            d.rounded_rectangle([cx, cy, cx + CARD_W - 1, cy + CARD_H - 1], radius=8,
                                fill=CARD, outline=CARD_EDGE, width=2)
            heart = load_sprite(HEART_PNG, 3)
            if heart is not None:
                page.alpha_composite(heart, (cx + 18, cy + 92))
            d.text((cx + 100, cy + 12), "商店回血", font=font(15, True, cjk=True), fill=ACCENT)
            d.text((cx + 100, cy + 38), "回 1 格血", font=font(13, cjk=True), fill=TEXT)
            d.text((cx + 100, cy + 62), "效果", font=font(10, cjk=True), fill=DIM)
            d.text((cx + 100, cy + 76), "价格 5 币", font=font(11, cjk=True), fill=TEXT)
            d.text((cx + 100, cy + 94), "只回 1 格血", font=font(11, cjk=True), fill=TEXT)
            d.text((cx + 100, cy + 120), "不进道具池", font=font(10, cjk=True), fill=DIM)
            d.text((cx + 100, cy + 136), "无底座 / 无浮动", font=font(10, cjk=True), fill=DIM)
            continue

        enabled = bool(int(row.get("enabled") or 0))
        icon_file = row.get("icon_file", "")
        icon = load_sprite(ICON_DIR / icon_file, 2) if icon_file else None
        add_card(page, cx, cy, row, base_img, icon, enabled, header_assets)

    d.text((MARGIN, H - 34),
           "实装 = 在 g_item_defs 中；候选 = PRAM 有图但未进 ITEM_*；CSV 是预览/规划，C 表仍是最终权威。",
           font=font(10, cjk=True), fill=DIM)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    page.convert("RGB").save(out_path, "PNG")
    print("[OK] preview:", out_path)
    print("[INFO] enabled items:", len(enabled_rows), "/ rows:", len(rows))

    # ---- validation ----
    ids = sorted(int(r.get("id") or 0) for r in enabled_rows)
    if ids != list(range(len(ids))):
        print("[WARN] enabled ids must be continuous 0..N-1:", ids)
    keys = [r.get("key") for r in enabled_rows]
    if len(set(keys)) != len(keys):
        print("[WARN] duplicate item key in enabled rows")
    for r in enabled_rows:
        desc = (r.get("desc") or "")
        if any(not (c.isupper() or c == " ") for c in desc):
            print("[WARN] desc must be A-Z/space only:", r.get("key"), repr(desc))
        asset = (r.get("icon_asset") or "").strip()
        if header_assets and asset not in header_assets:
            print("[WARN] icon_asset not in assets_desc.h:", r.get("key"), asset)
        icon = (r.get("icon_file") or "").strip()
        if icon and not (ICON_DIR / icon).exists():
            print("[WARN] icon_file missing:", r.get("key"), icon)
        if int(r.get("triple") or 0) and r.get("key") != "ITEM_EYES":
            print("[WARN] game_item.c currently derives fire_triple from ITEM_EYES only;",
                  "a new triple item also needs that line changed.")
    if emit_c:
        bullet_map = {"-": "-1", "": "-1", "A": "BULLET_A",
                      "CHILI": "BULLET_CHILI", "URANUS": "BULLET_URANUS"}
        print("\n/* ---- generated from tools/item_designer/items.csv ---- */")
        print("/* also update ITEM_* enum / ITEM_COUNT in game.h if the list changed */")
        print("const item_def_t g_item_defs[ITEM_COUNT] = {")
        for r in sorted(enabled_rows, key=lambda z: int(z.get("id") or 0)):
            key = r.get("key", "?")
            desc = r.get("desc", "")
            icon = r.get("icon_asset", "ASSET_?")
            b = (r.get("bullet") or "-").strip()
            bt = bullet_map.get(b, b)
            vals = [int(r.get("hp") or 0), int(r.get("hp_max") or 0), int(r.get("lives") or 0),
                    int(r.get("move_v8") or 0), int(r.get("fire_every") or 0),
                    int(r.get("bullet_speed") or 0), int(r.get("range") or 0),
                    int(r.get("damage") or 0), bt, int(r.get("triple") or 0),
                    int(r.get("coins") or 0)]
            lv = int(r.get("level") or 0)
            pool = int(r.get("pool") or 1)
            flags = "ITEM_FLAG_LIFE_PLUS" if str(r.get("life_plus", "0")) == "1" else "0"
            print('    /* %-14s */ { "%s", %s, %d, %d, %s,' % (key, desc, icon, lv, pool, flags))
            print("        { %s } }," % ", ".join(str(v) for v in vals))
        print("};")
        print("/* ---- end generated ---- */")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--items", default=str(DEFAULT_CSV))
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--emit-c", action="store_true")
    args = ap.parse_args()
    path = Path(args.items)
    if not path.is_absolute():
        path = ROOT / path
    if not path.exists() and path == DEFAULT_CSV:
        example = path.with_name("items.example.csv")
        if example.exists():
            path = example
    if not path.exists():
        sys.exit("items.csv not found: " + str(path))
    with open(path, encoding="utf-8-sig", newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        sys.exit("items.csv is empty")
    render(rows, Path(args.out), args.emit_c)


if __name__ == "__main__":
    main()
