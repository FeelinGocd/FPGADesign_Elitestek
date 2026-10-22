#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""DEBUG 房间编辑器的本地服务器 + 地图校验 / C 生成 CLI。

用法::

    uv run tools/sandbox/serve.py                 # 起服务，浏览器打开提示的地址
    uv run tools/sandbox/serve.py --open          # 起服务并自动开浏览器
    uv run tools/sandbox/serve.py --check         # 只校验：地图 + 生成的 C 是否最新
    uv run tools/sandbox/serve.py --emit-c        # 从 room_map.txt 重新生成 C

服务的目录：

* GET  /api/list                 output/ 里的 .txt / .c
* GET  /api/load?name=          读一个文件（文本）
* POST /api/save?name=          写地图 txt（先校验，原子写）
* POST /api/validate            校验一段地图文本或 C 源码
* POST /api/import              把 txt 或 C 解析成 map/rot JSON
* POST /api/emit_c?name=        生成 C 数据文件到 output/，并把文本返回
* POST /api/delete?name=        删除 output/ 里的文件

静态文件只放行 /tools/sandbox/ 和 /res/，不会把 .git、.venv、docs 暴露出去。
"""

from __future__ import annotations

import argparse
import difflib
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
MAX_BODY = 256 * 1024
NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
STATIC_PREFIXES = ("/tools/sandbox/", "/res/")
WRITE_LOCK = threading.Lock()


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


def parse_payload(text: str, name: str = "room_map") -> Tuple[mapfmt.RoomMap, str]:
    """自动识别 txt / C；格式错误转成 400。"""
    try:
        return mapfmt.parse_any(text, name)
    except mapfmt.MapError as exc:
        raise ApiError(400, str(exc)) from exc


# ---------------------------------------------------------------------------
#  HTTP
# ---------------------------------------------------------------------------

class Handler(SimpleHTTPRequestHandler):
    server_version = "SandboxRoomEditor/2.0"

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
        super().end_headers()

    def do_GET(self):
        request = urlparse(self.path)
        try:
            if request.path == "/api/list":
                self.api_list()
            elif request.path == "/api/load":
                self.api_load(parse_qs(request.query))
            elif request.path == "/api/health":
                self.send_json(200, {"ok": True, "files": str(OUT)})
            elif request.path == "/":
                self.send_response(302)
                self.send_header("Location", "/tools/sandbox/index.html")
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
        try:
            if request.path == "/api/save":
                self.api_save(query)
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
            if not entry.is_file() or entry.suffix.lower() not in (".txt", ".c"):
                continue
            stat = entry.stat()
            files.append({"name": entry.name, "size": stat.st_size, "mtime": int(stat.st_mtime)})
        self.send_json(200, {"ok": True, "files": files})

    def api_load(self, query: Dict[str, List[str]]) -> None:
        name = safe_name(query.get("name", [""])[0], "room_map.txt", (".txt", ".c"))
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
        name = safe_name(query.get("name", [""])[0], "room_map.txt", (".txt",))
        text = self.read_body()
        room, _kind = parse_payload(text, name)
        issues = mapfmt.validate(room)
        errors = mapfmt.errors_of(issues)
        if errors:
            raise ApiError(400, "校验没通过，没有写文件", issues)
        canonical = mapfmt.serialize_text(room)
        write_atomic(OUT / name, canonical)
        self.send_json(
            200,
            {
                "ok": True,
                "file": name,
                "text": canonical,
                "issues": issues_json(issues),
                "stats": mapfmt.stats(room),
            },
        )

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
        self.send_json(
            200,
            {
                "ok": True,
                "format": kind,
                "rows": ["".join(row) for row in room.rows],
                "rot": room.rot,
                "issues": issues_json(issues),
                "stats": mapfmt.stats(room),
            },
        )

    def api_emit_c(self, query: Dict[str, List[str]]) -> None:
        name = safe_name(query.get("name", [""])[0], "game_world_map.c", (".c",))
        map_name = safe_name(query.get("map", [""])[0], "room_map.txt", (".txt",))
        text = self.read_body()
        room, _kind = parse_payload(text, map_name)
        issues = mapfmt.validate(room)
        errors = mapfmt.errors_of(issues)
        if errors:
            raise ApiError(400, "校验没通过，没有生成 C", issues)
        c_text = mapfmt.emit_c(room, source="tools/sandbox/output/%s" % map_name)
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
        name = safe_name(query.get("name", [""])[0], "room_map.txt", (".txt", ".c"))
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
        return mapfmt.parse_text(read_text(path), path.stem)
    except mapfmt.MapError as exc:
        raise SystemExit("地图格式错误：%s" % exc) from None


def cmd_check(args: argparse.Namespace) -> int:
    map_path = Path(args.map).resolve() if args.map else OUT / "room_map.txt"
    c_path = Path(args.c).resolve() if args.c else OUT / "game_world_map.c"
    room = load_room(map_path)
    issues = mapfmt.validate(room)
    errors = mapfmt.errors_of(issues)
    print("地图 : %s" % rel(map_path))
    print("C    : %s" % rel(c_path))
    if issues:
        print_issues(issues)
    if errors:
        print("校验失败：%d 个错误" % len(errors))
        return 1
    expected = mapfmt.emit_c(room, source=rel(map_path))
    if not c_path.is_file():
        print("C 文件不存在，先跑 --emit-c")
        return 1
    actual = read_text(c_path)
    if actual == expected:
        print("OK：地图合法，C 已是最新（sha256:%s）" % mapfmt.digest_of(room)[:16])
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
    map_path = Path(args.map).resolve() if args.map else OUT / "room_map.txt"
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
    print("  %d 行 × %d 列，门 %d 个" % (mapfmt.ROWS, mapfmt.COLS, len(mapfmt.stats(room)["doors"])))
    return 0


def cmd_serve(args: argparse.Namespace) -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    try:
        server = ThreadingHTTPServer((args.host, args.port), Handler)
    except OSError as exc:
        print("端口 %d 起不来：%s（换一个 --port 试试）" % (args.port, exc))
        return 2
    url = "http://%s:%d/tools/sandbox/index.html" % (
        "127.0.0.1" if args.host in ("0.0.0.0", "") else args.host,
        args.port,
    )
    print("sandbox: %s" % url)
    print("输出目录: %s" % OUT)
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
        description="DEBUG 房间编辑器：本地服务 / 地图校验 / 生成 C",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--port", type=int, default=8765, help="监听端口（默认 8765）")
    parser.add_argument("--host", default="127.0.0.1", help="监听地址（默认只给本机）")
    parser.add_argument("--open", action="store_true", help="起服务后自动打开浏览器")
    parser.add_argument("--check", action="store_true", help="只校验地图 + C 是否最新")
    parser.add_argument("--emit-c", action="store_true", help="从地图重新生成 C 数据文件")
    parser.add_argument("--map", default="", help="地图 txt 路径（默认 output/room_map.txt）")
    parser.add_argument("--c", default="", help="C 输出路径（默认 output/game_world_map.c）")
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    if args.check:
        return cmd_check(args)
    if args.emit_c:
        return cmd_emit_c(args)
    return cmd_serve(args)


if __name__ == "__main__":
    sys.exit(main())
