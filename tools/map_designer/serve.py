#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MAP DESIGNER 房间设计器的本地服务器 + 地图校验 / 数据文件生成 CLI。

用法::

    uv run tools/map_designer/serve.py                 # 起服务，浏览器打开提示的地址
    uv run tools/map_designer/serve.py --open          # 起服务并自动开浏览器
    uv run tools/map_designer/serve.py --check         # 只校验：地图 + JSON / C 是否最新
    uv run tools/map_designer/serve.py --emit-json     # 从 room_map.txt 重新生成约定 JSON
    uv run tools/map_designer/serve.py --emit-c        # 从 room_map.txt 重新生成 C（可选）

服务的目录：

* GET  /api/list                 output/ 里的 .txt / .json / .c
* GET  /api/assets               res/pic 里当前有哪些图 + 各笔刷用哪张（不再写死文件名）
* GET  /api/templates          内置 + 自定义模板列表
* GET  /api/template?size=      生成起手骨架：template_1x1 / template_L / template_2x2 / custom_*（也可只给 cols/rows）
* POST /api/template_save?name=&label=  把当前图存成 templates/custom/custom_*.json
* POST /api/template_delete?name=       删除自定义模板（内置不可删）
* GET  /api/load?name=          读一个文件（文本）
* POST /api/save?name=          写 JSON 存档（允许半成品，原子写）
* POST /api/validate            校验一段地图文本 / JSON / C 源码
* POST /api/import              把 txt / JSON / C 解析成 map/rot/objects
* POST /api/emit_c?name=        生成 C 数据文件到 output/（可选导出，不改工程）
* CLI  --emit-frame             这张图如果正好是标准矩形框架，打印要贴进 g_maps[] 的那一行
* POST /api/delete?name=        删除 output/ 里的文件

静态文件只放行 /tools/map_designer/ 和 /res/，不会把 .git、.venv、docs 暴露出去。
"""

from __future__ import annotations

import argparse
import difflib
import importlib
import json
import os
import re
import sys
import threading
import webbrowser
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple
from urllib.parse import parse_qs, unquote, urlparse

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mapfmt  # noqa: E402


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
OUT = HERE / "output"
PIC = ROOT / "res" / "pic"
# 布局默认只生成到工程树；code/ 归档必须通过 --target archive/all 显式选择。
GAME_DIRS = (
    ROOT / "project/GamingPlayUnit/embedded_sw/SoC_RISCV32/software/standalone/GamingPlayUnit/game",
    ROOT / "code/logic/game",
)

#  笔刷角色表在 mapfmt.ASSET_ROLES（放那边才能热重载，见 resolve_assets）
PIC_EXTS = (".png", ".gif", ".jpg", ".jpeg")
MAX_BODY = 256 * 1024
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
STATIC_PREFIXES = ("/tools/map_designer/", "/res/")
WRITE_LOCK = threading.RLock()


_MAPFMT_MTIME = os.path.getmtime(mapfmt.__file__)


def reload_mapfmt_if_stale() -> None:
    """mapfmt.py 改了不用重启服务：只重载这一个模块（模板表/生成规则都在里面）。"""
    global _MAPFMT_MTIME
    try:
        mtime = os.path.getmtime(mapfmt.__file__)
    except OSError:
        return
    if mtime <= _MAPFMT_MTIME:
        return
    _MAPFMT_MTIME = mtime
    try:
        importlib.reload(mapfmt)
        print("[reload] mapfmt.py 已重新加载（模板：%s）" % "、".join(mapfmt.TEMPLATES), flush=True)
    except Exception as exc:                     # 改坏了就保留旧版本，别把服务弄挂
        print("[reload] mapfmt.py 重载失败，继续用旧的：%s" % exc, flush=True)


class DesignerServer(ThreadingHTTPServer):
    """Windows 上默认的 SO_REUSEADDR 会让两个进程同时绑同一端口，这里关掉。"""

    allow_reuse_address = False
    daemon_threads = True


class ApiError(Exception):
    def __init__(self, status: int, message: str, issues: Optional[List[mapfmt.Issue]] = None):
        super().__init__(message)
        self.status = status
        self.message = message
        self.issues = issues or []


# ---------------------------------------------------------------------------
#  小工具
# ---------------------------------------------------------------------------

def rel(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def read_text(path: Path) -> str:
    with open(path, "r", encoding="utf-8", newline="") as stream:
        return stream.read()


def write_atomic(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    with WRITE_LOCK:
        with open(tmp, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
        os.replace(tmp, path)


def safe_name(raw: str, default: str, exts: Tuple[str, ...]) -> str:
    name = os.path.basename((raw or "").replace("\\", "/")).strip() or default
    if not NAME_RE.match(name):
        raise ApiError(400, "文件名不合法（只允许字母、数字、点、下划线、短横线）")
    if not name.lower().endswith(exts):
        name += exts[0]
    return name


def issues_json(issues: List[mapfmt.Issue]) -> List[Dict[str, Any]]:
    return [issue.as_dict() for issue in issues]


def scan_pic() -> List[Dict[str, Any]]:
    """res/pic 下所有图片（跳过 _开头 / . 开头的目录和文件）。"""
    files: List[Dict[str, Any]] = []
    if not PIC.is_dir():
        return files
    for path in sorted(PIC.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in PIC_EXTS:
            continue
        parts = path.relative_to(PIC).parts
        if any(part.startswith(("_", ".")) for part in parts):
            continue
        stat = path.stat()
        files.append({
            "path": rel(path),
            "url": "/" + rel(path),
            "dir": "/".join(parts[:-1]),
            "name": path.name,
            "size": stat.st_size,
            "mtime_ns": stat.st_mtime_ns,
        })
    return files


def resolve_assets() -> Dict[str, Any]:
    """把 res/pic 的图对到笔刷角色上；多个候选时按 ASSET_ROLES 的顺序取第一个。"""
    files = scan_pic()
    roles: Dict[str, Any] = {}
    missing: List[str] = []
    for role, label, keys in mapfmt.ASSET_ROLES:
        hit: Optional[Dict[str, Any]] = None
        for key in keys:
            for item in files:
                if key in item["name"].lower():
                    hit = item
                    break
            if hit:
                break
        if hit:
            #  v= 文件修改时间：换图后浏览器不会继续用旧缓存。
            url = "%s?v=%d" % (hit["url"], hit["mtime_ns"])
            roles[role] = {"label": label, "path": hit["path"], "url": url, "name": hit["name"]}
        else:
            missing.append(role)
    return {"files": files, "roles": roles, "missing": missing, "root": rel(PIC)}


def parse_payload(text: str, name: str = "room_map") -> Tuple[mapfmt.RoomMap, str]:
    """自动识别 txt / C；格式错误转成 400。"""
    try:
        return mapfmt.parse_any(text, name)
    except mapfmt.MapError as exc:
        raise ApiError(400, str(exc)) from exc


# ---------------------------------------------------------------------------
#  HTTP
# ---------------------------------------------------------------------------

def numbered_layouts(map_id: str) -> List[Dict[str, Any]]:
    mapfmt.pool_layout_name(map_id, 1)
    entries = []
    for path in sorted((OUT / "layouts").glob(map_id + "_*.json")):
        room = mapfmt.parse_json(read_text(path), path.stem)
        if room.layout_id is None or mapfmt.pool_map_id(room) != map_id:
            raise mapfmt.MapError("编号布局缺少或混用了房型/编号：%s" % path.name)
        if path.name != mapfmt.pool_layout_name(map_id, room.layout_id):
            raise mapfmt.MapError("布局文件名与内部编号不一致：%s" % path.name)
        entries.append({"id": room.layout_id, "file": path.name})
    return sorted(entries, key=lambda e: e["id"])


def next_layout_id(entries: List[Dict[str, Any]]) -> Optional[int]:
    candidate = max((e["id"] for e in entries), default=0) + 1
    return candidate if candidate <= 999 else None


class Handler(SimpleHTTPRequestHandler):
    server_version = "MapDesigner/3.0"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT), **kwargs)

    # 别把静态请求刷满控制台，只记录 API
    def log_message(self, fmt, *args):
        if "/api/" in (self.path or ""):
            super().log_message(fmt, *args)

    def end_headers(self):
        path = self.path or ""
        if path.startswith("/api/") or path.endswith((".html", ".js", ".css")):
            self.send_header("Cache-Control", "no-store")
        elif path.startswith("/res/"):
            self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def do_GET(self):
        request = urlparse(self.path)
        reload_mapfmt_if_stale()
        try:
            if request.path == "/api/list":
                self.api_list()
            elif request.path == "/api/assets":
                self.api_assets()
            elif request.path == "/api/templates":
                self.api_templates()
            elif request.path == "/api/template":
                self.api_template(parse_qs(request.query))
            elif request.path == "/api/load":
                self.api_load(parse_qs(request.query))
            elif request.path == "/api/layout_catalog":
                self.api_layout_catalog(parse_qs(request.query))
            elif request.path == "/api/load_layout":
                self.api_load_layout(parse_qs(request.query))
            elif request.path == "/api/health":
                self.send_json(200, {"ok": True, "files": str(OUT)})
            elif request.path == "/":
                self.send_response(302)
                self.send_header("Location", "/tools/map_designer/index.html")
                self.end_headers()
            elif request.path in ("/favicon.ico",
                                   "/.well-known/appspecific/com.chrome.devtools.json"):
                self.send_response(204)
                self.end_headers()
            elif self.static_ok(request.path):
                super().do_GET()
            else:
                self.send_json(404, {"ok": False, "error": "not found"})
        except ApiError as exc:
            self.send_json(exc.status, {"ok": False, "error": exc.message, "issues": issues_json(exc.issues)})
        except OSError as exc:
            self.send_json(500, {"ok": False, "error": str(exc)})

    def do_POST(self):
        request = urlparse(self.path)
        query = parse_qs(request.query)
        reload_mapfmt_if_stale()
        try:
            if request.path == "/api/save":
                self.api_save(query)
            elif request.path == "/api/save_layout":
                self.api_save_layout(query)
            elif request.path == "/api/save_map":
                self.api_save_map(query)
            elif request.path == "/api/template_save":
                self.api_template_save(query)
            elif request.path == "/api/template_delete":
                self.api_template_delete(query)
            elif request.path == "/api/emit_c":
                self.api_emit_c(query)
            elif request.path == "/api/validate":
                self.api_validate()
            elif request.path == "/api/import":
                self.api_import()
            elif request.path == "/api/delete":
                self.api_delete(query)
            else:
                self.send_json(404, {"ok": False, "error": "not found"})
        except ApiError as exc:
            self.send_json(exc.status, {"ok": False, "error": exc.message, "issues": issues_json(exc.issues)})
        except OSError as exc:
            self.send_json(500, {"ok": False, "error": str(exc)})

    # ---- 路由对应的实现 -------------------------------------------------

    def api_list(self) -> None:
        OUT.mkdir(parents=True, exist_ok=True)
        files: List[Dict[str, Any]] = []
        for entry in sorted(OUT.iterdir(), key=lambda item: item.name.lower()):
            if not entry.is_file() or entry.suffix.lower() not in (".txt", ".json", ".c"):
                continue
            stat = entry.stat()
            files.append({"name": entry.name, "size": stat.st_size, "mtime": int(stat.st_mtime)})
        self.send_json(200, {"ok": True, "files": files})

    def api_assets(self) -> None:
        self.send_json(200, dict(ok=True, **resolve_assets()))

    def api_templates(self) -> None:
        self.send_json(200, {"ok": True, "templates": mapfmt.template_catalog()})

    def api_template(self, query: Dict[str, List[str]]) -> None:
        size = (query.get("size", [""])[0] or "").strip()
        try:
            if size:
                room = mapfmt.make_template(size)
            else:
                cols = int(query.get("cols", [str(mapfmt.DEFAULT_COLS)])[0])
                rows = int(query.get("rows", [str(mapfmt.DEFAULT_ROWS)])[0])
                room = mapfmt.make_room(cols, rows)
        except (mapfmt.MapError, ValueError) as exc:
            raise ApiError(400, str(exc)) from exc
        label = mapfmt.template_label(size) or "自定义 %d×%d" % (room.cols, room.height)
        issues = mapfmt.validate(room)
        self.send_json(
            200,
            {
                "ok": True,
                "label": label,
                "size": [room.cols, room.height],
                "text": mapfmt.serialize_text(room),
                "rows": ["".join(row) for row in room.rows],
                "rot": room.rot,
                "objects": [obj.as_dict() for obj in room.objects],
                "issues": issues_json(issues),
                "stats": mapfmt.stats(room),
            },
        )

    def api_template_save(self, query: Dict[str, List[str]]) -> None:
        name = (query.get("name", [""])[0] or "").strip()
        label = (query.get("label", [""])[0] or "").strip()
        text = self.read_body()
        room, _kind = parse_payload(text, name or "template")
        issues = mapfmt.validate(room)
        try:
            info = mapfmt.save_custom_template(room, name, label)
        except mapfmt.MapError as exc:
            raise ApiError(400, str(exc)) from exc
        self.send_json(
            200,
            {
                "ok": True,
                "template": info,
                "issues": issues_json(issues),
                "stats": mapfmt.stats(room),
            },
        )

    def api_template_delete(self, query: Dict[str, List[str]]) -> None:
        name = (query.get("name", [""])[0] or "").strip()
        try:
            info = mapfmt.delete_custom_template(name)
        except mapfmt.MapError as exc:
            raise ApiError(400, str(exc)) from exc
        self.send_json(200, {"ok": True, "template": info})

    def api_load(self, query: Dict[str, List[str]]) -> None:
        name = safe_name(query.get("name", [""])[0], "room_map.txt", (".txt", ".json", ".c"))
        path = OUT / name
        if not path.is_file():
            raise ApiError(404, "文件不存在：output/%s" % name)
        body = read_text(path).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def api_save(self, query: Dict[str, List[str]]) -> None:
        #  ★ 只存一份 JSON：它是唯一存档（文本那套只在导入老文件时认）。
        name = safe_name(query.get("name", [""])[0], "room_map.json", (".txt", ".json"))
        if name.lower().endswith(".txt"):
            name = name[:-4] + ".json"
        elif not name.lower().endswith(".json"):
            name += ".json"
        text = self.read_body()
        room, _kind = parse_payload(text, Path(name).stem)
        issues = mapfmt.validate(room)
        #  ★ 草稿可以带错误存下来（设计中途经常是半成品）；
        #    错误只在"进代码"（--emit-layouts / 生成 C）时才拦。
        canonical = mapfmt.serialize_text(room)
        payload = mapfmt.serialize_json(room)
        json_name = name
        write_atomic(OUT / json_name, payload)
        self.send_json(
            200,
            {
                "ok": True,
                "file": json_name,
                "saved": True,
                "error_count": len(mapfmt.errors_of(issues)),
                "text": canonical,
                "json": payload,
                "issues": issues_json(issues),
                "stats": mapfmt.stats(room),
            },
        )

    def api_save_layout(self, query: Dict[str, List[str]]) -> None:
        room, _kind = parse_payload(self.read_body())
        try:
            map_id = mapfmt.pool_map_id(room)
            if query.get("map", [map_id])[0] != map_id:
                raise mapfmt.MapError("当前地图空间与所选房型不一致")
            mapfmt.pool_layout_name(map_id, 1)  # 排除隐藏房等特殊房型
            with WRITE_LOCK:
                entries = numbered_layouts(map_id)
                raw_id = query.get("id", [""])[0]
                layout_id = int(raw_id) if raw_id else next_layout_id(entries)
                name = mapfmt.pool_layout_name(map_id, layout_id)
                target = OUT / "layouts" / name
                create = query.get("create", ["1"])[0] == "1"
                if create and target.exists():
                    raise ApiError(409, "布局编号已存在，请刷新列表或选择已有方案后保存")
                if not create and not target.exists():
                    raise ApiError(404, "待更新布局不存在，请使用新建布局")
                room.layout_id = layout_id
                room.name = Path(name).stem
                payload = mapfmt.serialize_pool_layout(room)
                write_atomic(target, payload)
        except (ValueError, mapfmt.MapError) as exc:
            raise ApiError(400, str(exc)) from exc
        self.send_json(200, {"ok": True, "file": name, "json": payload,
                             "text": mapfmt.serialize_text(room), "layout_id": layout_id,
                             "map": map_id, "issues": issues_json(mapfmt.validate(room))})

    def api_layout_catalog(self, query: Dict[str, List[str]]) -> None:
        map_id = query.get("map", ["1x1"])[0]
        try:
            entries = numbered_layouts(map_id)
            self.send_json(200, {"ok": True, "map": map_id, "layouts": entries,
                                 "next_id": next_layout_id(entries)})
        except mapfmt.MapError as exc:
            raise ApiError(400, str(exc)) from exc

    def api_load_layout(self, query: Dict[str, List[str]]) -> None:
        try:
            name = mapfmt.pool_layout_name(query.get("map", [""])[0], int(query.get("id", ["0"])[0]))
        except (ValueError, mapfmt.MapError) as exc:
            raise ApiError(400, str(exc)) from exc
        path = OUT / "layouts" / name
        if not path.is_file():
            raise ApiError(404, "布局不存在：%s" % name)
        self.send_json(200, {"ok": True, "layout": json.loads(read_text(path))})

    def api_save_map(self, query: Dict[str, List[str]]) -> None:
        map_id = query.get("map", [""])[0]
        if map_id not in mapfmt.NORMAL_MAP_IDS:
            raise ApiError(400, "本轮仅支持普通房型 1x1、L、2x2")
        room, _kind = parse_payload(self.read_body())
        room.name = map_id
        room.objects = []
        # 地图模式导出为草稿，不覆盖作为布局依据的正式固定空间。
        payload = mapfmt.serialize_json(room)
        write_atomic(OUT / "maps" / (map_id + ".json"), payload)
        self.send_json(200, {"ok": True, "file": map_id + ".json", "json": payload,
                             "text": mapfmt.serialize_text(room)})

    def api_validate(self) -> None:
        text = self.read_body()
        room, kind = parse_payload(text)
        issues = mapfmt.validate(room)
        self.send_json(
            200,
            {
                "ok": not mapfmt.errors_of(issues),
                "format": kind,
                "issues": issues_json(issues),
                "stats": mapfmt.stats(room),
            },
        )

    def api_import(self) -> None:
        text = self.read_body()
        room, kind = parse_payload(text)
        issues = mapfmt.validate(room)
        try:
            map_id = mapfmt.pool_map_id(room)
        except mapfmt.MapError:
            map_id = None
        self.send_json(
            200,
            {
                "ok": True,
                "format": kind,
                "rows": ["".join(row) for row in room.rows],
                "rot": room.rot,
                "objects": [obj.as_dict() for obj in room.objects],
                "name": room.name,
                "map": map_id,
                "layout_id": room.layout_id,
                "issues": issues_json(issues),
                "stats": mapfmt.stats(room),
            },
        )

    def api_emit_c(self, query: Dict[str, List[str]]) -> None:
        name = safe_name(query.get("name", [""])[0], "game_world_map.c", (".c",))
        map_name = safe_name(query.get("map", [""])[0], "room_map.txt", (".txt", ".json"))
        text = self.read_body()
        room, _kind = parse_payload(text, map_name)
        issues = mapfmt.validate(room)
        errors = mapfmt.errors_of(issues)
        if errors:
            raise ApiError(400, "校验没通过，没有生成 C", issues)
        c_text = mapfmt.emit_c(room, source="tools/map_designer/output/%s" % map_name)
        write_atomic(OUT / name, c_text)
        self.send_json(
            200,
            {
                "ok": True,
                "file": name,
                "c": c_text,
                "issues": issues_json(issues),
                "stats": mapfmt.stats(room),
            },
        )

    def api_delete(self, query: Dict[str, List[str]]) -> None:
        name = safe_name(query.get("name", [""])[0], "room_map.txt", (".txt", ".json", ".c"))
        path = OUT / name
        existed = path.is_file()
        if existed:
            path.unlink()
        self.send_json(200, {"ok": True, "file": name, "existed": existed})

    # ---- 基础设施 -------------------------------------------------------

    def read_body(self) -> str:
        raw_length = self.headers.get("Content-Length")
        try:
            length = int(raw_length or "0")
        except ValueError:
            raise ApiError(400, "Content-Length 不对") from None
        if length <= 0:
            raise ApiError(400, "请求体是空的")
        if length > MAX_BODY:
            raise ApiError(413, "请求体太大（上限 %d 字节）" % MAX_BODY)
        try:
            return self.rfile.read(length).decode("utf-8")
        except UnicodeDecodeError:
            raise ApiError(400, "请求体不是 UTF-8") from None

    def send_json(self, status: int, value: Dict[str, Any]) -> None:
        body = json.dumps(value, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    @staticmethod
    def static_ok(path: str) -> bool:
        decoded = unquote(path)
        if ".." in decoded.split("/"):
            return False
        parts = [part for part in decoded.split("/") if part]
        if any(part.startswith(".") for part in parts):
            return False
        return any(decoded.startswith(prefix) for prefix in STATIC_PREFIXES)


# ---------------------------------------------------------------------------
#  CLI
# ---------------------------------------------------------------------------

def print_issues(issues: List[mapfmt.Issue]) -> None:
    for issue in issues:
        where = "" if issue.x < 0 else " (%d,%d)" % (issue.x, issue.y)
        print("  [%s]%s %s" % (issue.level, where, issue.message))


def load_room(path: Path) -> mapfmt.RoomMap:
    if not path.is_file():
        raise SystemExit("地图文件不存在：%s" % path)
    try:
        return mapfmt.parse_any(read_text(path), path.stem)[0]
    except mapfmt.MapError as exc:
        raise SystemExit("地图格式错误：%s" % exc) from None


def cmd_check(args: argparse.Namespace) -> int:
    map_path = Path(args.map).resolve() if args.map else OUT / "room_map.json"
    c_path = Path(args.c).resolve() if args.c else OUT / "game_world_map.c"
    json_path = Path(args.json).resolve() if args.json else OUT / "room_map.json"
    room = load_room(map_path)
    issues = mapfmt.validate(room)
    errors = mapfmt.errors_of(issues)
    print("地图 : %s" % rel(map_path))
    print("JSON : %s" % rel(json_path))
    print("C    : %s" % rel(c_path))
    if issues:
        print_issues(issues)
    if errors:
        print("校验失败：%d 个错误" % len(errors))
        return 1

    expected_json = mapfmt.serialize_json(room)
    if json_path.is_file():
        if read_text(json_path) != expected_json:
            print("JSON 过期了，和地图对不上；跑一次 --emit-json 重新生成。")
            return 1
    else:
        print("提示：JSON 不存在，先跑 --emit-json 做存档")

    expected = mapfmt.emit_c(room, source=rel(map_path))
    if not c_path.is_file():
        print("OK：地图合法，JSON 是最新的（C 数据文件没生成 —— 现在是可选导出，手动 --emit-c）")
        return 0
    actual = read_text(c_path)
    if actual == expected:
        print("OK：地图合法，JSON / C 已是最新（sha256:%s）" % mapfmt.digest_of(room)[:16])
        return 0
    diff = list(
        difflib.unified_diff(
            actual.splitlines(),
            expected.splitlines(),
            fromfile=rel(c_path),
            tofile="generated",
            lineterm="",
            n=1,
        )
    )
    print("C 过期了，和地图对不上：")
    for line in diff[:40]:
        print("  " + line)
    print("跑一次 --emit-c 重新生成。")
    return 1


def cmd_emit_c(args: argparse.Namespace) -> int:
    map_path = Path(args.map).resolve() if args.map else OUT / "room_map.json"
    c_path = Path(args.c).resolve() if args.c else OUT / "game_world_map.c"
    room = load_room(map_path)
    issues = mapfmt.validate(room)
    if issues:
        print_issues(issues)
    errors = mapfmt.errors_of(issues)
    if errors:
        print("校验失败：%d 个错误，没有生成 C" % len(errors))
        return 1
    text = mapfmt.emit_c(room, source=rel(map_path))
    write_atomic(c_path, text)
    print("生成 %s" % rel(c_path))
    print("sha256:%s" % mapfmt.digest_of(room))
    arena = mapfmt.arena_rect(room)
    print(
        "  %d×%d 格（%d×%d px），竞技场 %d,%d → %d,%d，门 %d 个"
        % (
            room.cols, room.height, room.cols * mapfmt.TILE, room.height * mapfmt.TILE,
            arena[0], arena[1], arena[2], arena[3], len(mapfmt.stats(room)["doors"]),
        )
    )
    return 0


def cmd_emit_layouts(args: argparse.Namespace) -> int:
    """扫 output/*.json → 生成 game_layout_data.c/.h（零配置）。

    约定：**一份房间 = 一份文件**，文件名就是房间名（output/<房间名>.json）。
    布局引用 maps/ 的固定空间，只保存对象；旧完整 JSON 仍可读，但空间必须一致。
    · 默认只预览；加 --apply 才写文件，默认目标是 project 工程树；
    · --target archive/all 显式选择 code/ 归档或两份；
    · 加 --check 只比对"磁盘上的生成物是不是最新"，过期返回 1。
    """
    #  ★ 名字里带 `_tile_` 的是"房间部件"（L 房的分块 tile），不是独立房间：
    #    它们只是同一张画布的局部，导出成 room_layout_t 会把内存撑爆，直接跳过。
    all_files = sorted(OUT.glob("*.json"))
    for map_id in mapfmt.NORMAL_MAP_IDS:
        try:
            all_files.extend(OUT / "layouts" / e["file"] for e in numbered_layouts(map_id))
        except (OSError, mapfmt.MapError) as exc:
            print("布局池读取失败：%s" % exc)
            return 1
    files = [p for p in all_files if "_tile_" not in p.stem]
    skipped = [p.name for p in all_files if p not in files]
    if not files:
        print("output/ 里没有 .json")
        print("约定：output/<房间名>.json = 一份房间（地形 + 对象），文件名就是房间名")
        return 1

    items = []
    identities = set()
    for path in files:
        name = path.stem
        try:
            room = mapfmt.parse_json(read_text(path), name)
            # 旧完整 JSON 兼容读取，固定空间必须属于地图池。
            mapfmt.pool_map_id(room)
            if room.layout_id is not None:
                identity = (mapfmt.pool_map_id(room), room.layout_id)
                if identity in identities:
                    raise mapfmt.MapError("房型与布局编号重复：%s_%03d" % identity)
                identities.add(identity)
                if path.name != mapfmt.pool_layout_name(*identity):
                    raise mapfmt.MapError("文件名与布局编号不一致：%s" % path.name)
        except mapfmt.MapError as exc:
            print("  ! %s：%s" % (path.name, exc))
            return 1
        items.append((name, room))

    world_path = GAME_DIRS[0] / "game_world.c"
    try:
        frames = mapfmt.parse_world_frames(read_text(world_path))
        header, source, warnings = mapfmt.emit_layout_data(items, frames)
    except (OSError, mapfmt.MapError) as exc:
        print("生成失败：%s" % exc)
        return 1
    for item in warnings:
        print("  ~ %s" % item)

    targets = (("game_layout_data.h", header), ("game_layout_data.c", source))
    target = getattr(args, "target", "project")
    folders = (GAME_DIRS if target == "all" else
               GAME_DIRS[:1] if target == "project" else GAME_DIRS[1:])

    if args.check:
        stale = []
        for filename, text in targets:
            for folder in folders:
                path = folder / filename
                if not path.is_file() or read_text(path) != text:
                    stale.append(rel(path))
        if stale:
            print("生成物过期：%s" % "、".join(stale))
            print("跑一次 --emit-layouts --apply 重新生成")
            return 1
        print("OK：%d 张布局，game_layout_data.c/.h 是最新的" % len(items))
        return 0

    if skipped:
        print("跳过 %d 个部件文件（_tile_）：%s" % (len(skipped), "、".join(skipped)))
    print("扫到 %d 张布局：" % len(items))
    for name, room in sorted(items, key=lambda item: item[0]):
        print("  %-16s %d×%d  %d 个对象" % (name, room.cols, room.height, len(room.objects)))

    if not args.apply:
        print("\n（预览，没写文件）确认无误后加 --apply：")
        for folder in folders:
            print("  " + rel(folder))
        print("\n----- game_layout_data.h -----")
        print(header)
        print("----- game_layout_data.c -----")
        print(source)
        return 0

    written = []
    for folder in folders:
        if not folder.is_dir():
            continue
        for filename, text in targets:
            write_atomic(folder / filename, text)
        written.append(rel(folder))
    print("已写入：" + "、".join(written))
    return 0


def cmd_emit_frame(args: argparse.Namespace) -> int:
    """打印"标准矩形框架"的参数行；异形房会明确告诉你只能存表。"""
    map_path = Path(args.map).resolve() if args.map else OUT / "room_map.json"
    room = load_room(map_path)
    issues = mapfmt.validate(room)
    if issues:
        print_issues(issues)
    if mapfmt.errors_of(issues):
        print("校验没通过，先修上面的错误")
        return 1
    frame = mapfmt.detect_frame(room)
    if frame is None:
        print("这张图不是标准矩形框架（异形房）：墙/地板有额外改动，只能把地形存成表,")
        print("把 world_map_t.map/rot 指向生成的数组即可（生成器不生效）。")
        return 1
    world_w, world_h = mapfmt.frame_world(frame)
    print("标准框架：%d×%d 格，竞技场 %s，门 %s，世界 %d×%d"
          % (frame["cols"], frame["rows"], frame["arena"], frame["doors"], world_w, world_h))
    print("在 game_world.c 的 g_maps[] 里加一行：")
    print(mapfmt.frame_c(frame, map_path.stem))
    return 0


def cmd_emit_json(args: argparse.Namespace) -> int:
    map_path = Path(args.map).resolve() if args.map else OUT / "room_map.json"
    json_path = Path(args.json).resolve() if args.json else OUT / "room_map.json"
    room = load_room(map_path)
    issues = mapfmt.validate(room)
    if issues:
        print_issues(issues)
    errors = mapfmt.errors_of(issues)
    if errors:
        print("校验失败：%d 个错误，没有生成 JSON" % len(errors))
        return 1
    write_atomic(json_path, mapfmt.serialize_json(room))
    print("生成 %s" % rel(json_path))
    print("sha256:%s" % mapfmt.digest_of(room))
    return 0


def cmd_serve(args: argparse.Namespace) -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    port = args.port
    server = None
    last_error: Optional[OSError] = None
    attempts = 10 if args.port == 8766 else 1
    for offset in range(attempts):
        candidate = args.port + offset
        try:
            server = DesignerServer((args.host, candidate), Handler)
            port = candidate
            if candidate != args.port:
                print("端口 %d 被占用，改用 %d" % (args.port, candidate))
            break
        except OSError as exc:
            last_error = exc
    if server is None:
        print("端口 %d 起不来：%s（换一个 --port 试试）" % (args.port, last_error))
        return 2
    url = "http://%s:%d/tools/map_designer/index.html" % (
        "127.0.0.1" if args.host in ("0.0.0.0", "") else args.host,
        port,
    )
    print("map_designer: %s" % url)
    print("输出目录: %s" % OUT)
    print("内置模板: %s" % "、".join(mapfmt.TEMPLATES))
    print("自定义模板目录: %s" % rel(mapfmt.CUSTOM_TEMPLATE_DIR), flush=True)
    print("Ctrl+C 退出")
    if args.open:
        threading.Timer(0.4, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")
    finally:
        server.server_close()
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="地图设计器：本地服务 / 地图校验 / 生成 JSON 与可选 C",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--port", type=int, default=8766, help="监听端口（默认 8766；8765 常被 item_designer 占用）")
    parser.add_argument("--host", default="127.0.0.1", help="监听地址（默认只给本机）")
    parser.add_argument("--open", action="store_true", help="起服务后自动打开浏览器")
    parser.add_argument("--check", action="store_true", help="只校验地图 + JSON / C 是否最新")
    parser.add_argument("--emit-json", action="store_true", help="从地图重新生成约定 JSON")
    parser.add_argument("--emit-c", action="store_true", help="从地图重新生成 C 数据文件（可选导出）")
    parser.add_argument("--emit-frame", action="store_true", help="打印标准矩形框架的 g_maps[] 参数行")
    parser.add_argument("--emit-layouts", action="store_true", help="扫 output/*.json（一文件一房间）生成 game_layout_data.c/.h")
    parser.add_argument("--apply", action="store_true", help="配合 --emit-layouts：真正写文件（默认只预览）")
    parser.add_argument("--target", choices=("project", "archive", "all"), default="project",
                        help="布局生成/检查目标：默认仅 project，archive=code 归档，all=两份")
    parser.add_argument("--map", default="", help="地图 / JSON 路径（默认 output/room_map.json）")
    parser.add_argument("--json", default="", help="JSON 输出路径（默认 output/room_map.json）")
    parser.add_argument("--c", default="", help="C 输出路径（默认 output/game_world_map.c）")
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    if args.emit_layouts:                 # 放前面：--emit-layouts --check 是查生成物
        return cmd_emit_layouts(args)
    if args.check:
        return cmd_check(args)
    if args.emit_c:
        return cmd_emit_c(args)
    if args.emit_frame:
        return cmd_emit_frame(args)
    if args.emit_json:
        return cmd_emit_json(args)
    return cmd_serve(args)


if __name__ == "__main__":
    sys.exit(main())
