#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
server.py -- 道具设计器本地网站

启动:
    python tools/item_designer/server.py

默认 http://127.0.0.1:8765

功能:
    - 浏览/编辑 tools/item_designer/items.csv
    - 保存时直接重生成 docs/spec/道具预览.png
    - 同步更新 docs/spec/道具_规范.md 里的道具表（ITEM_TABLE 标记区）
    - 提交时生成 tools/item_designer/pending_request.md（含 diff + C 片段 + 实现清单）
    - 不直接改 C；由 AI 读取 pending_request.md 后实现
"""

import argparse
import csv
import json
import os
import re
import sys
import time
import webbrowser
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

ROOT = Path(__file__).resolve().parents[2]
ITEM_DIR = ROOT / "tools" / "item_designer"
sys.path.insert(0, str(ITEM_DIR))
import preview  # noqa: E402

FIELDS = [
    "id", "key", "enabled", "desc", "icon_asset", "icon_file", "bullet",
    "triple", "hp", "lives", "move_v8", "fire_every", "bullet_speed",
    "range", "damage", "note", "features", "level", "pool", "life_plus",
]
INT_FIELDS = ["triple", "hp", "lives", "move_v8", "fire_every",
              "bullet_speed", "range", "damage", "level", "pool", "life_plus"]
FIELD_CN = {"triple": "增加弹幕", "hp": "回血", "lives": "加命",
            "move_v8": "移速", "fire_every": "射速", "bullet_speed": "弹速",
            "range": "射程", "damage": "伤害", "level": "品质", "pool": "道具池",
            "life_plus": "生命保险"}
BULLETS = {"-", "CHILI", "URANUS"}
QNAME = ["普通", "稀有", "史诗", "传说"]

CSV_PATH = ITEM_DIR / "items.csv"
BRIEF_PATH = ITEM_DIR / "design_brief.md"
PENDING_PATH = ITEM_DIR / "pending_request.md"
STATE_PATH = ITEM_DIR / "designer_state.json"
SPEC_PATH = ROOT / "docs" / "spec" / "道具_规范.md"
PREVIEW_PATH = ROOT / "docs" / "spec" / "道具预览.png"
ICON_DIR = ROOT / "res" / "pic" / "accessory"
HEART_DIR = ROOT / "res" / "pic" / "base" / "specialEffect"
HEADER = ROOT / "code" / "peripheral" / "APB0" / "gpu2d" / "assets_desc.h"
GAME_H = ROOT / "code" / "logic" / "game" / "game.h"
HTML_PATH = Path(__file__).resolve().with_name("index.html")

BT = chr(96)
TABLE_START = "<!-- ITEM_TABLE_START -->"
TABLE_END = "<!-- ITEM_TABLE_END -->"

EXAMPLE_CSV = ITEM_DIR / "items.example.csv"


def now_text():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def safe_asset(filename):
    stem = Path(filename).stem
    return "ASSET_" + re.sub(r"[^A-Za-z0-9_]", "_", stem)


def parse_features(value):
    """features 在 CSV 里是 JSON 数组字符串；API/前端传数组也接受。"""
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


def header_assets():
    if not HEADER.exists():
        return set()
    txt = HEADER.read_text(encoding="ascii", errors="ignore")
    return set(re.findall(r"\b(ASSET_[A-Za-z0-9_]+)\s*=", txt))


def implemented_keys():
    if not GAME_H.exists():
        return set()
    txt = GAME_H.read_text(encoding="utf-8", errors="ignore")
    m = re.search(r"enum\s*\{[^}]*ITEM_COUNT[^}]*\}", txt)
    if not m:
        return set()
    keys = re.findall(r"\b(ITEM_[A-Z0-9_]+)\b", m.group(0))
    return {k for k in keys if k != "ITEM_COUNT"}


def read_csv():
    # CSV 是唯一数据源。items.csv 缺失时只读同目录 items.example.csv 作为默认数据；
    # 不在 Python 里硬编码任何道具；两个文件都没有就返回空列表。保存时才写 items.csv。
    src = CSV_PATH if CSV_PATH.exists() else EXAMPLE_CSV
    if not src.exists():
        return []
    rows = []
    with src.open(encoding="utf-8-sig", newline="") as f:
        for raw in csv.DictReader(f):
            row = {k: str(raw.get(k) or "").strip() for k in FIELDS}
            row["level"] = row["level"] or "0"
            row["pool"] = row["pool"] or "1"
            row["life_plus"] = row["life_plus"] or "0"
            row["range"] = row["range"] or "0"
            rows.append(row)
    return rows


def atomic_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    with tmp.open("w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    os.replace(tmp, path)


def write_csv(rows):
    tmp = CSV_PATH.with_name(CSV_PATH.name + ".tmp")
    with tmp.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS, lineterminator="\n")
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k, "") for k in FIELDS})
    os.replace(tmp, CSV_PATH)


def read_brief():
    if BRIEF_PATH.exists():
        return BRIEF_PATH.read_text(encoding="utf-8").strip()
    return ""


def normalize(rows):
    clean = []
    for r in rows:
        feats = parse_features(r.get("features", ""))
        row = {k: str(r.get(k, "") or "").strip() for k in FIELDS}
        row["features"] = json.dumps(feats, ensure_ascii=False)
        if row["enabled"] not in ("0", "1"):
            row["enabled"] = "1" if row["enabled"].lower() in ("true", "yes", "on") else "0"
        if not row["icon_asset"] and row["icon_file"]:
            row["icon_asset"] = safe_asset(row["icon_file"])
        clean.append(row)
    enabled = [r for r in clean if r["enabled"] == "1"]
    disabled = [r for r in clean if r["enabled"] != "1"]
    for i, r in enumerate(enabled):
        r["id"] = str(i)
    for i, r in enumerate(disabled):
        r["id"] = str(len(enabled) + i)
    return enabled + disabled


def validate(rows):
    errors, warnings = [], []
    assets = header_assets()
    seen = set()
    for r in rows:
        key = r.get("key", "")
        where = key or ("第 " + str(r.get("id", "?")) + " 行")
        if not re.match(r"^[A-Z][A-Z0-9_]*$", key):
            errors.append(where + "：C 名称只能用 A-Z、0-9、_，且必须以字母开头")
        if key in seen:
            errors.append(where + "：C 名称重复")
        seen.add(key)
        enabled = r.get("enabled") == "1"
        active = enabled and r.get("pool") == "1"
        check_fields = INT_FIELDS if active else [f for f in INT_FIELDS if f not in ("level", "pool")]
        for f in check_fields:
            try:
                int(r.get(f, "0"))
            except ValueError:
                errors.append(where + "：" + FIELD_CN.get(f, f) + " 必须是整数")
        if active:
            try:
                lv = int(r.get("level", "0"))
                if lv < 0 or lv > 3:
                    errors.append(where + "：品质只能是 普通/稀有/史诗/传说")
            except ValueError:
                pass
            try:
                if int(r.get("pool", "1")) not in (0, 1):
                    errors.append(where + "：道具池只能是 入池/不入池")
            except ValueError:
                pass
            try:
                if int(r.get("life_plus", "0")) not in (0, 1):
                    errors.append(where + "：生命保险只能是 是/否")
            except ValueError:
                pass
        if r.get("bullet") not in BULLETS:
            errors.append(where + "：弹种只能是 无 / 辣椒 / 天王星")
        if r.get("enabled") == "1":
            if not re.match(r"^[A-Z ]+$", r.get("desc", "")):
                errors.append(where + "：字幕只能 A-Z 和空格（16×24 字模）")
            icon = r.get("icon_file", "")
            if not icon:
                errors.append(where + "：必须选图标")
            elif Path(icon).name != icon or not (ICON_DIR / icon).exists():
                errors.append(where + "：图标文件不存在（res/pic/accessory）：" + icon)
            asset = r.get("icon_asset", "")
            if assets and asset not in assets:
                warnings.append(where + "：素材编号不在 assets_desc.h（先跑 pic2pram）：" + asset)
        if not r.get("icon_asset") and r.get("icon_file"):
            warnings.append(where + "：素材编号为空；将使用 " + safe_asset(r.get("icon_file")))
    enabled_ids = [int(r["id"]) for r in rows if r.get("enabled") == "1"]
    if enabled_ids != list(range(len(enabled_ids))):
        errors.append("启用行编号必须从 0 连续")
    return errors, warnings


def effect_cn(r):
    parts = []
    b = (r.get("bullet") or "-").strip()
    if b and b != "-":
        parts.append("弹种 " + b)
    if int(r.get("triple") or 0):
        parts.append("增加弹幕")
    if int(r.get("hp") or 0):
        parts.append("回血 +" + str(int(r.get("hp") or 0)))
    if int(r.get("lives") or 0):
        parts.append("命数 +" + str(int(r.get("lives") or 0)))
    labels = (("move_v8", "移速"), ("fire_every", "射速"),
              ("bullet_speed", "弹速"), ("range", "射程"), ("damage", "伤害"))
    for f, label in labels:
        v = int(r.get(f) or 0)
        if v:
            parts.append(label + (" +" if v > 0 else " ") + str(v))
    return " / ".join(parts) if parts else "无属性变化"


def code_cell(s):
    return BT + str(s) + BT


def md_cell(s):
    return str(s).replace("|", "\\|").replace("\n", " ")


def table_md(rows):
    enabled = [r for r in rows if r.get("enabled") == "1"]
    disabled = [r for r in rows if r.get("enabled") != "1"]
    impl = implemented_keys()
    out = ["| ID | key | 字幕 desc | 图标 | 效果 | 品质 | 池 | 生命 | 功能标签 | 状态 |",
           "|---:|---|---|---|---|---|---|---|---|---|"]
    for r in enabled:
        icon = code_cell(r.get("icon_asset", "")) + " / " + md_cell(r.get("icon_file", ""))
        status = "已实装" if r.get("key") in impl else "已启用 / 待实现"
        feats = parse_features(r.get("features", ""))
        fcell = md_cell(" / ".join(feats)) if feats else "—"
        try:
            lv = int(r.get("level", "0"))
        except ValueError:
            lv = 0
        qcell = QNAME[lv] if 0 <= lv < 4 else "普通"
        pcell = "入池" if str(r.get("pool", "1")) == "1" else "不入池"
        lcell = "保险" if str(r.get("life_plus", "0")) == "1" else "—"
        out.append("| " + r["id"] + " | " + code_cell(r.get("key", "")) + " | " +
                   md_cell(r.get("desc", "")) + " | " + icon + " | " +
                   effect_cn(r) + " | " + qcell + " | " + pcell + " | " + lcell + " | " +
                   fcell + " | " + status + " |")
    out.append("| — | " + code_cell("SHOP_HEART") + " | — | " +
               code_cell("ASSET_SpecialEffect_heart") + " / SpecialEffect_heart.png | " +
               "回 1 格血 / 5 币 / 不进道具池 | — | — | — | — | 已实装 |")
    for r in disabled:
        icon = code_cell(r.get("icon_asset", "")) + " / " + md_cell(r.get("icon_file", ""))
        feats = parse_features(r.get("features", ""))
        fcell = md_cell(" / ".join(feats)) if feats else "—"
        try:
            lv = int(r.get("level", "0"))
        except ValueError:
            lv = 0
        qcell = QNAME[lv] if 0 <= lv < 4 else "普通"
        pcell = "入池" if str(r.get("pool", "1")) == "1" else "不入池"
        lcell = "保险" if str(r.get("life_plus", "0")) == "1" else "—"
        out.append("| " + r["id"] + " | " + code_cell(r.get("key", "")) + " | " +
                   md_cell(r.get("desc", "")) + " | " + icon + " | " +
                   effect_cn(r) + " | " + qcell + " | " + pcell + " | " + lcell + " | " +
                   fcell + " | 候选（未进 ITEM_COUNT） |")
    return "\n".join(out)


def update_spec_table(rows):
    if not SPEC_PATH.exists():
        return False, "规范文件不存在"
    text = SPEC_PATH.read_text(encoding="utf-8")
    if TABLE_START not in text or TABLE_END not in text:
        return False, "规范表缺少 ITEM_TABLE 标记"
    a = text.index(TABLE_START)
    b = text.index(TABLE_END, a)
    new = TABLE_START + "\n" + table_md(rows) + "\n" + TABLE_END
    text = text[:a] + new + text[b + len(TABLE_END):]
    atomic_text(SPEC_PATH, text)
    return True, "ok"


def make_c(rows):
    bullet_map = {"-": "-1", "": "-1", "A": "BULLET_A",
                  "CHILI": "BULLET_CHILI", "URANUS": "BULLET_URANUS"}
    enabled = sorted([r for r in rows if r.get("enabled") == "1"],
                     key=lambda r: int(r.get("id") or 0))
    lines = ["/* ---- generated from tools/item_designer/items.csv ---- */",
             "/* also update ITEM_* enum / ITEM_COUNT in game.h if the list changed */",
             "const item_def_t g_item_defs[ITEM_COUNT] = {"]
    for r in enabled:
        b = (r.get("bullet") or "-").strip()
        bt = bullet_map.get(b, b)
        vals = [r.get("hp") or "0", "0", r.get("lives") or "0",
                r.get("move_v8") or "0", r.get("fire_every") or "0",
                r.get("bullet_speed") or "0", r.get("range") or "0",
                r.get("damage") or "0", bt, r.get("triple") or "0"]
        lv = r.get("level") or "0"
        pool = r.get("pool") or "1"
        flags = "ITEM_FLAG_LIFE_PLUS" if str(r.get("life_plus", "0")) == "1" else "0"
        lines.append("    /* " + (r.get("key", "?") + "              ")[:14] + " */ { " +
                     json.dumps(r.get("desc", ""), ensure_ascii=False) + ", " +
                     r.get("icon_asset", "ASSET_?") + ", " + str(lv) + ", " + str(pool) + ", " + flags + ",")
        lines.append("        { " + ", ".join(str(v) for v in vals) + " } },")
    lines.append("};")
    lines.append("/* ---- end generated ---- */")
    return "\n".join(lines)


def diff_md(old_rows, new_rows):
    old = {r.get("key", ""): r for r in old_rows}
    new = {r.get("key", ""): r for r in new_rows}
    out = []
    for k in sorted(set(new) - set(old)):
        out.append("- 新增 " + k + "（" + new[k].get("desc", "") + "）")
    for k in sorted(set(old) - set(new)):
        out.append("- 删除 " + k)
    for k in sorted(set(old) & set(new)):
        changes = []
        for f in FIELDS:
            if (old[k].get(f, "") != new[k].get(f, "")):
                changes.append(f + ": " + old[k].get(f, "") + " -> " + new[k].get(f, ""))
        if changes:
            out.append("- " + k + "：" + "；".join(changes))
    return "\n".join(out) if out else "无变化"


def request_md(rows, brief, diff, stamp):
    c = make_c(rows)
    table = table_md(rows)
    note_lines = ["- " + r.get("key", "") + "：" + r.get("note", "")
                  for r in rows if (r.get("note", "") or "").strip()]
    notes = "\n".join(note_lines) if note_lines else "（无）"
    feat_lines = []
    for r in rows:
        fs = parse_features(r.get("features", ""))
        if fs:
            feat_lines.append("- " + r.get("key", "") + "：" + " / ".join(fs))
    feats = "\n".join(feat_lines) if feat_lines else "（无）"
    return "\n".join([
        "# 道具实现请求（本地设计器生成）",
        "",
        "- 生成时间：" + stamp,
        "- 数据表：tools/item_designer/items.csv",
        "- 设计说明：tools/item_designer/design_brief.md",
        "- 预览图：docs/spec/道具预览.png",
        "- 规范表：docs/spec/道具_规范.md（保存时已自动更新）",
        "",
        "## 设计说明",
        "",
        brief.strip() if brief.strip() else "（未填写，按 CSV 字段实现）",
        "",
        "## 本次变化",
        "",
        diff,
        "",
        "## 当前道具表",
        "",
        table,
        "",
        "## 每项 note（来自 CSV）",
        "",
        notes,
        "",
        "## 特殊功能 / 标签",
        "",
        feats,
        "",
        "## 品质 / 道具池",
        "",
        "level/pool 目前只在设计器/规范；C 的 g_item_defs 还没有这两个字段。",
        "",
        "## 可直接贴入 game_item.c 的表",
        "",
        "~~~c",
        c,
        "~~~",
        "",
        "## 请 AI 执行",
        "",
        "1. 读取 tools/item_designer/items.csv、本文件和 design_brief.md。",
        "2. 按启用顺序更新 game.h 的 ITEM_* / ITEM_COUNT，把上面的表贴进 game_item.c。",
        "3. 如果新增/替换了图标，先跑 uv run tools/img2hex/pic2pram.py，再重编固件/重综合 bit。",
        "4. 如果涉及新效果，检查 game_item_apply() 的字段限制（hp_max/增加弹幕等，见规范第 3.2 节）。",
        "5. 更新/检查 docs/spec/道具_规范.md 与预览图，回复改了哪些文件。",
        "",
        "> 对话里发送：请读取 tools/item_designer/pending_request.md 并实现道具。",
        "",
    ])


def save_all(body, submit=False):
    raw = body.get("items")
    if not isinstance(raw, list):
        raise ValueError("items 必须是列表")
    brief = str(body.get("brief", "") or "")
    rows = normalize(raw)
    errors, warnings = validate(rows)
    if errors:
        return 400, {"ok": False, "errors": errors, "warnings": warnings}
    old_rows = read_csv()
    old_stamp = now_text()
    try:
        write_csv(rows)
        atomic_text(BRIEF_PATH, brief)
        preview.render(rows, PREVIEW_PATH, emit_c=False)
        spec_ok, spec_msg = update_spec_table(rows)
        if not spec_ok:
            warnings.append("规范表未更新：" + spec_msg)
        diff = diff_md(old_rows, rows)
        state = {"saved_at": old_stamp, "brief": brief, "rows": rows}
        atomic_text(STATE_PATH, json.dumps(state, ensure_ascii=False, indent=2))
        request = ""
        if submit:
            request = request_md(rows, brief, diff, old_stamp)
            atomic_text(PENDING_PATH, request)
        return 200, {
            "ok": True,
            "warnings": warnings,
            "errors": [],
            "preview": "/preview.png?t=" + str(int(time.time())),
            "csv": str(CSV_PATH.relative_to(ROOT)),
            "spec_updated": spec_ok,
            "request": request,
            "diff": diff,
        }
    except Exception as e:
        return 500, {"ok": False, "errors": ["保存失败：" + str(e)], "warnings": warnings}


class Handler(BaseHTTPRequestHandler):
    server_version = "ItemDesigner/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("[WEB] " + (fmt % args) + "\n")

    def _json(self, obj, code=200):
        data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _bytes(self, data, ctype, code=200):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _read_json(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        if n <= 0 or n > 2 * 1024 * 1024:
            raise ValueError("请求体为空或过大")
        raw = self.rfile.read(n)
        return json.loads(raw.decode("utf-8"))

    def do_GET(self):
        try:
            path = urlparse(self.path).path
            if path in ("/", "/index.html"):
                if not HTML_PATH.exists():
                    self._json({"ok": False, "error": "页面文件 index.html 缺失"}, 500)
                    return
                self._bytes(HTML_PATH.read_bytes(), "text/html; charset=utf-8")
                return
            if path == "/api/items":
                rows = read_csv()
                rows_api = []
                for r in rows:
                    rr = dict(r)
                    rr["features"] = parse_features(r.get("features", ""))
                    rows_api.append(rr)
                icons = sorted(p.name for p in ICON_DIR.glob("*.png") if p.name != "accessory_base.png")
                assets = header_assets()
                self._json({
                    "ok": True,
                    "fields": FIELDS,
                    "int_fields": INT_FIELDS,
                    "rows": rows_api,
                    "brief": read_brief(),
                    "icons": icons,
                    "icon_assets": {n: safe_asset(n) for n in icons},
                    "asset_names": sorted(assets),
                    "base_icon": "accessory_base.png",
                    "heart_icon": "SpecialEffect_heart.png",
                    "preview": "/preview.png",
                    "csv": str(CSV_PATH.relative_to(ROOT)),
                    "pending": PENDING_PATH.exists(),
                    "root": str(ROOT),
                })
                return
            if path == "/api/request":
                if PENDING_PATH.exists():
                    self._bytes(PENDING_PATH.read_bytes(), "text/plain; charset=utf-8")
                else:
                    self._json({"ok": False, "error": "还没有提交请求"}, 404)
                return
            if path == "/api/brief":
                self._bytes(read_brief().encode("utf-8"), "text/plain; charset=utf-8")
                return
            if path == "/api/health":
                self._json({"ok": True, "root": str(ROOT), "csv": str(CSV_PATH)})
                return
            if path == "/preview.png":
                if PREVIEW_PATH.exists():
                    self._bytes(PREVIEW_PATH.read_bytes(), "image/png")
                else:
                    self.send_error(404)
                return
            if path.startswith("/icons/"):
                name = unquote(path[len("/icons/"):])
                if not name or "/" in name or "\\" in name or not name.endswith(".png"):
                    self.send_error(404)
                    return
                for p in (ICON_DIR / name, HEART_DIR / name):
                    if p.exists():
                        self._bytes(p.read_bytes(), "image/png")
                        return
                self.send_error(404)
                return
            if path == "/favicon.ico":
                self.send_response(204)
                self.end_headers()
                return
            self.send_error(404)
        except Exception as e:
            self._json({"ok": False, "error": str(e)}, 500)

    def do_POST(self):
        try:
            path = urlparse(self.path).path
            if path not in ("/api/save", "/api/submit"):
                self.send_error(404)
                return
            body = self._read_json()
            code, obj = save_all(body, submit=(path == "/api/submit"))
            self._json(obj, code)
        except Exception as e:
            self._json({"ok": False, "errors": [str(e)]}, 400)


def main():
    ap = argparse.ArgumentParser(description="Item designer local web app")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--no-open", action="store_true")
    args = ap.parse_args()
    if args.host not in ("127.0.0.1", "localhost"):
        print("[WARN] binding to non-loopback host; this tool is meant to be local", flush=True)
    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    httpd.daemon_threads = True
    url = "http://" + args.host + ":" + str(args.port) + "/"
    print("ITEM DESIGNER:", url, flush=True)
    print("ROOT:", ROOT, flush=True)
    print("CSV :", CSV_PATH, flush=True)
    print("PNG :", PREVIEW_PATH, flush=True)
    print("Press Ctrl+C to stop.", flush=True)
    if not args.no_open:
        try:
            webbrowser.open(url)
        except Exception:
            pass
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nbye", flush=True)


if __name__ == "__main__":
    main()
