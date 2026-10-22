#!/usr/bin/env python3
"""DEBUG 房间编辑器的本地静态服务器。

运行 ``uv run tools/sandbox/serve.py`` 后访问
http://127.0.0.1:8765/tools/sandbox/index.html。

除静态文件外，服务器还提供保存、读取、列出和删除 output/ 中地图文件的接口。
"""

import argparse
import json
import os
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.abspath(os.path.join(os.path.dirname(__file__), "output"))


def safe_name(name: str) -> str:
    """只允许 output 目录下的纯文件名，并统一使用 .txt 后缀。"""
    name = os.path.basename(name or "room_map.txt")
    if not name.endswith(".txt"):
        name += ".txt"
    return name


def json_response(handler, status: int, value: dict) -> None:
    body = json.dumps(value, ensure_ascii=False).encode("utf-8")
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(body)))
    handler.end_headers()
    handler.wfile.write(body)


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=ROOT, **kwargs)

    def log_message(self, fmt, *args):
        if "/api/" in (self.path or ""):
            super().log_message(fmt, *args)

    def do_GET(self):
        request = urlparse(self.path)
        query = parse_qs(request.query)

        if request.path == "/api/list":
            os.makedirs(OUT, exist_ok=True)
            files = sorted(name for name in os.listdir(OUT) if name.endswith(".txt"))
            json_response(self, 200, {"files": files})
            return

        if request.path == "/api/load":
            path = os.path.join(OUT, safe_name(query.get("name", [""])[0]))
            if not os.path.isfile(path):
                self.send_error(404, "地图文件不存在")
                return
            try:
                with open(path, "r", encoding="utf-8") as stream:
                    body = stream.read().encode("utf-8")
            except OSError as exc:
                self.send_error(500, str(exc))
                return
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        super().do_GET()

    def do_POST(self):
        request = urlparse(self.path)
        query = parse_qs(request.query)
        name = safe_name(query.get("name", [""])[0])
        path = os.path.join(OUT, name)

        if request.path == "/api/delete":
            try:
                existed = os.path.isfile(path)
                if existed:
                    os.remove(path)
            except OSError as exc:
                json_response(self, 500, {"ok": False, "error": str(exc)})
                return
            json_response(self, 200, {"ok": existed, "file": name})
            return

        if request.path != "/api/save":
            self.send_error(404)
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
            data = self.rfile.read(length).decode("utf-8")
            os.makedirs(OUT, exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="") as stream:
                stream.write(data)
        except (ValueError, UnicodeDecodeError, OSError) as exc:
            json_response(self, 400, {"ok": False, "error": str(exc)})
            return
        json_response(self, 200, {"ok": True, "file": name})


def main():
    parser = argparse.ArgumentParser(description="DEBUG 房间编辑器服务器")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    os.makedirs(OUT, exist_ok=True)
    print(f"sandbox: http://127.0.0.1:{args.port}/tools/sandbox/index.html")
    print(f"保存目录: {OUT}")
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
