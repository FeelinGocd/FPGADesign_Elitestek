#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""房间地图的格式、校验与 C 代码生成 —— 唯一实现。

数据格式（v2，现有文件就是这一版）::

    # DEBUG_ROOM_MAP v2
    <23 行 × 40 字符>
    # ROTATION
    <23 行 × 40 个 0..3 数字>

另外兼容：

* v1：只有 23 行地图，没有 # ROTATION（旋转全 0）；
* 稀疏旋转：# ROTATION 之后每行 x,y,r（只写非 0 的格子）；
* C 源码：room_map_shared / room_rot_shared 两张表（导入用）。

字符含义：. 空 / B 基岩地板 / W 木墙 / D 门（占 1×2）/
C X Y Z 角块（翻转）。

几何约定（和 game.h / game_world.c 对齐）：

* 屏幕 1280×720，40×23 格，32 px/格，最后一行只有 16 px；
* 竞技场 288,128 → 1024,544，外面是 2 格厚的墙带；
* 门格 D 在墙带里，旋转值就是方向：0=N 1=E 2=S 3=W；
* D 的“另一半”固定是 +x（横向门）或 +y（纵向门）那一格，必须是 . 。

这个模块不依赖第三方库，serve.py / 单元测试 / CI 共用。
"""

from __future__ import annotations

import hashlib
import re
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple


COLS = 40
ROWS = 23
TILE = 32
CANVAS_W = COLS * TILE          # 1280
CANVAS_H = 720                  # 第 23 行只有 16 px
ARENA_X0, ARENA_Y0 = 288, 128
ARENA_X1, ARENA_Y1 = 1024, 544
BAND = 2 * TILE                 # 墙带厚度 64 px

ALLOWED = frozenset(".BWDCXYZ")
CORNER = frozenset("CXYZ")
DIR_NAME = {0: "N", 1: "E", 2: "S", 3: "W"}

FORMAT_HEADER = "# DEBUG_ROOM_MAP v2"
DEFAULT_NAME = "room_map"
FENCE = chr(96) * 3


class MapError(ValueError):
    """地图文本 / C 源码格式错误。"""


@dataclass(frozen=True)
class Issue:
    level: str                  # "error" / "warning"
    message: str
    x: int = -1
    y: int = -1

    def as_dict(self) -> Dict[str, Any]:
        return {"level": self.level, "message": self.message, "x": self.x, "y": self.y}


@dataclass
class RoomMap:
    rows: List[List[str]]
    rot: List[List[int]]
    name: str = DEFAULT_NAME


# ---------------------------------------------------------------------------
#  解析 / 序列化
# ---------------------------------------------------------------------------

def _map_lines(lines: Sequence[str]) -> List[Tuple[int, str]]:
    out: List[Tuple[int, str]] = []
    for number, line in enumerate(lines, 1):
        text = line.rstrip()
        if not text:
            continue
        stripped = text.strip()
        if stripped.startswith("#") or stripped == FENCE:
            continue
        if re.match(r"^(MAP|ROOM)[_A-Z0-9]*\s*[:=]", stripped, re.I):
            continue
        out.append((number, text))
    return out


def parse_text(text: str, name: str = DEFAULT_NAME) -> RoomMap:
    """解析编辑器保存的 txt（v1 / v2 / 稀疏旋转都吃）。"""
    if not isinstance(text, str):
        raise MapError("地图内容必须是字符串")
    source = text.replace("\ufeff", "").replace("\r\n", "\n").replace("\r", "\n")
    lines = source.split("\n")

    marker = -1
    for index, line in enumerate(lines):
        if line.strip().upper().startswith("# ROTATION"):
            marker = index
            break

    head = lines[:marker] if marker >= 0 else lines
    rows_in = _map_lines(head)
    if len(rows_in) != ROWS:
        raise MapError(
            "地图需要 %d 行，实际 %d 行（空行和 # 注释行不算；行首也别缩进）"
            % (ROWS, len(rows_in))
        )

    rows: List[List[str]] = []
    for number, line in rows_in:
        if len(line) != COLS:
            raise MapError(
                "第 %d 行长度 %d，需要 %d（行首空格也会算进去）"
                % (number, len(line), COLS)
            )
        bad = sorted(set(line) - ALLOWED)
        if bad:
            raise MapError("第 %d 行有非法字符 %r" % (number, "".join(bad)))
        rows.append(list(line))

    rot = [[0] * COLS for _ in range(ROWS)]
    if marker >= 0:
        rot_lines = [
            line.strip()
            for line in lines[marker + 1:]
            if line.strip() and not line.strip().startswith("#")
        ]
        if not rot_lines:
            raise MapError("# ROTATION 后面没有内容")
        sparse = any(("," in line) or (" " in line) or ("\t" in line) for line in rot_lines)
        if sparse:
            for line in rot_lines:
                parts = re.split(r"[\s,]+", line)
                if len(parts) != 3:
                    raise MapError("稀疏旋转要写成 x,y,r：%r" % line)
                try:
                    x, y, r = (int(part) for part in parts)
                except ValueError:
                    raise MapError("稀疏旋转里有非整数：%r" % line) from None
                if not (0 <= x < COLS and 0 <= y < ROWS):
                    raise MapError("旋转坐标越界：(%d,%d)" % (x, y))
                if not 0 <= r <= 3:
                    raise MapError("旋转值必须是 0..3：%r" % line)
                rot[y][x] = r
        else:
            if len(rot_lines) < ROWS:
                raise MapError("旋转表需要 %d 行，实际 %d 行" % (ROWS, len(rot_lines)))
            for y in range(ROWS):
                line = rot_lines[y]
                if len(line) != COLS:
                    raise MapError(
                        "旋转表第 %d 行长度 %d，需要 %d" % (y + 1, len(line), COLS)
                    )
                for x, char in enumerate(line):
                    if char not in "0123":
                        raise MapError(
                            "旋转表第 %d 行第 %d 列不是 0..3：%r" % (y + 1, x + 1, char)
                        )
                    rot[y][x] = int(char)

    return RoomMap(rows, rot, name)


def serialize_text(room: RoomMap, sparse: bool = False) -> str:
    """写回 v2 文本；默认写满 23×40 的旋转表（和现有文件一致）。"""
    body = "\n".join("".join(row) for row in room.rows)
    if sparse:
        triples = [
            "%d,%d,%d" % (x, y, room.rot[y][x] & 3)
            for y in range(ROWS)
            for x in range(COLS)
            if room.rot[y][x]
        ]
        rotation = "# ROTATION sparse\n" + ("\n".join(triples) + "\n" if triples else "")
    else:
        rotation = "# ROTATION\n" + "\n".join(
            "".join(str(int(value) & 3) for value in row) for row in room.rot
        ) + "\n"
    return FORMAT_HEADER + "\n" + body + "\n" + rotation


def clean(room: RoomMap) -> RoomMap:
    """空白格的旋转没有意义，统一归零；同时把越界值夹回 0..3。"""
    rows = [list(row) for row in room.rows]
    rot = [[(int(value) & 3) for value in row] for row in room.rot]
    for y in range(ROWS):
        for x in range(COLS):
            if rows[y][x] == ".":
                rot[y][x] = 0
    return RoomMap(rows, rot, room.name)


def digest_of(room: RoomMap) -> str:
    text = serialize_text(clean(room)).encode("utf-8")
    return hashlib.sha256(text).hexdigest()


# ---------------------------------------------------------------------------
#  几何
# ---------------------------------------------------------------------------

def wall_sides(x: int, y: int) -> List[str]:
    """这一格属于墙带的哪一边（墙角会返回两个）。"""
    px, py = x * TILE, y * TILE
    sides: List[str] = []
    if ARENA_Y0 - BAND <= py < ARENA_Y0 and ARENA_X0 - BAND <= px < ARENA_X1 + BAND:
        sides.append("N")
    if ARENA_Y1 <= py < ARENA_Y1 + BAND and ARENA_X0 - BAND <= px < ARENA_X1 + BAND:
        sides.append("S")
    if ARENA_X0 - BAND <= px < ARENA_X0 and ARENA_Y0 - BAND <= py < ARENA_Y1 + BAND:
        sides.append("W")
    if ARENA_X1 <= px < ARENA_X1 + BAND and ARENA_Y0 - BAND <= py < ARENA_Y1 + BAND:
        sides.append("E")
    return sides


def in_arena(x: int, y: int) -> bool:
    px, py = x * TILE, y * TILE
    return ARENA_X0 <= px and px + TILE <= ARENA_X1 and ARENA_Y0 <= py and py + TILE <= ARENA_Y1


def in_wall_band(x: int, y: int) -> bool:
    return bool(wall_sides(x, y))


def door_pair(x: int, y: int, angle: int) -> Tuple[int, int]:
    """门格 D 的“另一半”坐标：横向门在右，纵向门在下。"""
    return (x + 1, y) if (angle & 1) else (x, y + 1)


# ---------------------------------------------------------------------------
#  校验
# ---------------------------------------------------------------------------

def validate(room: RoomMap) -> List[Issue]:
    issues: List[Issue] = []

    def add(level: str, message: str, x: int = -1, y: int = -1) -> None:
        issues.append(Issue(level, message, x, y))

    # ---- 尺寸 / 字符 ------------------------------------------------------
    if len(room.rows) != ROWS:
        add("error", "地图行数 %d，需要 %d" % (len(room.rows), ROWS))
        return issues
    for y, row in enumerate(room.rows):
        if len(row) != COLS:
            add("error", "第 %d 行长度 %d，需要 %d" % (y + 1, len(row), COLS), 0, y)
            return issues
        for x, char in enumerate(row):
            if char not in ALLOWED:
                add("error", "非法字符 %r" % char, x, y)
            if room.rot[y][x] not in (0, 1, 2, 3):
                add("error", "旋转值 %r 不在 0..3" % room.rot[y][x], x, y)

    # ---- 区域 ------------------------------------------------------------
    # 门洞的另一半是 "."，现有文件里会带着 1/2/3 的旋转（C 侧根本不读它），
    # 所以这里只提醒“真正的空白格”，门洞那格不报。
    door_halves = set()
    for y in range(ROWS):
        for x in range(COLS):
            if room.rows[y][x] == "D":
                door_halves.add(door_pair(x, y, room.rot[y][x]))

    for y in range(ROWS):
        for x in range(COLS):
            char = room.rows[y][x]
            if char == ".":
                if room.rot[y][x] and (x, y) not in door_halves:
                    add("warning", "空白格 (%d,%d) 的旋转没有意义（保存时会保留，C 侧会归零）" % (x, y), x, y)
                continue
            if in_arena(x, y):
                if char != "B":
                    add("warning", "竞技场内的 (%d,%d) 是 %r，会挡住玩家" % (x, y, char), x, y)
                continue
            if in_wall_band(x, y):
                if char == "B":
                    add("warning", "墙带里的 (%d,%d) 是地板" % (x, y), x, y)
                continue
            add("warning", "房间外的 (%d,%d) 画了 %r，上板看不到它" % (x, y, char), x, y)

    # ---- 门 --------------------------------------------------------------
    doors: List[Tuple[int, int, int]] = []
    pairs: Dict[Tuple[int, int], Tuple[int, int]] = {}
    for y in range(ROWS):
        for x in range(COLS):
            if room.rows[y][x] != "D":
                continue
            angle = room.rot[y][x] & 3
            sides = wall_sides(x, y)
            if not sides:
                add("error", "门 (%d,%d) 不在 2 格厚的墙带上" % (x, y), x, y)
                continue
            if len(sides) > 1:
                add("error", "门 (%d,%d) 在墙角，方向不唯一" % (x, y), x, y)
                continue

            side = sides[0]
            expect = {"N": 0, "E": 1, "S": 2, "W": 3}[side]
            if angle != expect:
                add(
                    "error",
                    "门 (%d,%d) 在 %s 墙，方向应为 %d（%s），现在是 %d"
                    % (x, y, side, expect, DIR_NAME[expect], angle),
                    x, y,
                )

            px, py = door_pair(x, y, angle)
            if not (0 <= px < COLS and 0 <= py < ROWS):
                add("error", "门 (%d,%d) 的另一格越界" % (x, y), x, y)
                continue
            if room.rows[py][px] != ".":
                add(
                    "error",
                    "门 (%d,%d) 的另一格 (%d,%d) 必须是空白格，现在是 %r"
                    % (x, y, px, py, room.rows[py][px]),
                    x, y,
                )
            if (px, py) in pairs:
                add("error", "两扇门共用了 (%d,%d)" % (px, py), px, py)
            pairs[(px, py)] = (x, y)
            doors.append((x, y, angle))

    # ---- 墙带上的空洞 ----------------------------------------------------
    for y in range(ROWS):
        for x in range(COLS):
            if room.rows[y][x] != "." or not in_wall_band(x, y):
                continue
            if (x, y) not in pairs:
                add("warning", "墙带里的 (%d,%d) 是空白，会漏出一个洞" % (x, y), x, y)

    # ---- 门数量 ----------------------------------------------------------
    if len(doors) != 4:
        add("warning", "当前有 %d 个门格，通常房间是 4 扇门（N/E/S/W 各一）" % len(doors))

    return issues


def errors_of(issues: Iterable[Issue]) -> List[Issue]:
    return [issue for issue in issues if issue.level == "error"]


def stats(room: RoomMap) -> Dict[str, Any]:
    counts = {char: 0 for char in ".BWDCXYZ"}
    doors: List[Dict[str, Any]] = []
    for y in range(ROWS):
        for x in range(COLS):
            char = room.rows[y][x]
            if char in counts:
                counts[char] += 1
            if char == "D":
                angle = room.rot[y][x] & 3
                doors.append({"x": x, "y": y, "dir": DIR_NAME[angle], "angle": angle})
    return {
        "counts": counts,
        "doors": doors,
        "size": [COLS, ROWS],
        "sha256": digest_of(room),
    }


# ---------------------------------------------------------------------------
#  C 代码生成 / 解析
# ---------------------------------------------------------------------------

def emit_c(room: RoomMap, source: str = "tools/sandbox/output/room_map.txt") -> str:
    """生成独立的数据文件（static 数组，可单独编译，也可被 include）。"""
    room = clean(room)
    info = stats(room)
    counts = info["counts"]
    doors = " ".join(
        "%s(%d,%d)" % (door["dir"], door["x"], door["y"]) for door in info["doors"]
    ) or "（无）"
    digest = info["sha256"]

    lines: List[str] = []
    lines.append("/* " + "=" * 74)
    lines.append(" *  game_world_map.c —— 房间地图数据（自动生成，别手改）")
    lines.append(" *  " + "-" * 73)
    lines.append(" *  生成器 : tools/sandbox/serve.py --emit-c")
    lines.append(" *  源文件 : %s" % source)
    lines.append(" *  指纹   : sha256:%s" % digest)
    lines.append(
        " *  统计   : 门 %d · 墙 %d · 地板 %d · 角块 %d"
        % (
            counts["D"],
            counts["W"],
            counts["B"],
            counts["C"] + counts["X"] + counts["Y"] + counts["Z"],
        )
    )
    lines.append(" *  门     : %s" % doors)
    lines.append(" *  " + "-" * 73)
    lines.append(" *  用法：把两张表整体替换进 game_world.c，或者删掉 game_world.c 里的")
    lines.append(' *        两段数组，改成 #include "game_world_map.c"（数组是 static，')
    lines.append(" *        不会和其他编译单元抢符号）。")
    lines.append(" * " + "=" * 74 + " */")
    lines.append("")
    lines.append("#ifndef GAME_WORLD_MAP_DATA_C")
    lines.append("#define GAME_WORLD_MAP_DATA_C")
    lines.append("")
    lines.append("#ifndef MAP_COLS")
    lines.append("#define MAP_COLS %d" % COLS)
    lines.append("#endif")
    lines.append("#ifndef MAP_ROWS")
    lines.append("#define MAP_ROWS %d" % ROWS)
    lines.append("#endif")
    lines.append("")
    lines.append('#define ROOM_MAP_SHA256 "%s"' % digest)
    lines.append("")
    lines.append("static const char room_map_shared[MAP_ROWS][MAP_COLS + 1] = {")
    for row in room.rows:
        lines.append('    "%s",' % "".join(row))
    lines.append("};")
    lines.append("")
    lines.append("static const unsigned char room_rot_shared[MAP_ROWS][MAP_COLS] = {")
    for row in room.rot:
        lines.append("    { %s }," % ", ".join(str(value) for value in row))
    lines.append("};")
    lines.append("")
    lines.append("#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L")
    lines.append('_Static_assert(sizeof(room_map_shared[0]) == MAP_COLS + 1, "room_map row width");')
    lines.append('_Static_assert(sizeof(room_rot_shared[0]) == MAP_COLS, "room_rot row width");')
    lines.append("#endif")
    lines.append("")
    lines.append("#endif /* GAME_WORLD_MAP_DATA_C */")
    lines.append("")
    return "\n".join(lines)


def _brace_block(text: str, name: str) -> str:
    match = re.search(r"\b%s\b[^;=]*=\s*\{" % re.escape(name), text)
    if not match:
        raise MapError("C 源码里找不到 %s 数组" % name)
    start = match.end() - 1
    depth = 0
    in_string = False
    escaped = False
    for index in range(start, len(text)):
        char = text[index]
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            continue
        if char == '"':
            in_string = True
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    raise MapError("%s 数组的括号没有闭合" % name)


def _find_block(text: str, names: Sequence[str]) -> str:
    last_error: Optional[MapError] = None
    for name in names:
        try:
            return _brace_block(text, name)
        except MapError as exc:
            last_error = exc
    raise last_error or MapError("找不到地图数组")


def parse_c(text: str, name: str = DEFAULT_NAME) -> RoomMap:
    """从 C 源码（生成文件或 game_world.c）里把两张表读回来。"""
    if not isinstance(text, str):
        raise MapError("C 内容必须是字符串")
    source = text.replace("\ufeff", "").replace("\r\n", "\n")
    map_block = _find_block(source, ("room_map_shared", "room_map", "room_map_data"))
    rot_block = _find_block(source, ("room_rot_shared", "room_rot", "room_rot_data"))

    rows: List[List[str]] = []
    for literal in re.findall(r'"([^"\\]*)"', map_block):
        if len(literal) == COLS and set(literal) <= ALLOWED:
            rows.append(list(literal))
    if len(rows) != ROWS:
        raise MapError("C 里解析到 %d 行地图，需要 %d 行" % (len(rows), ROWS))

    rot: List[List[int]] = []
    for match in re.finditer(r"\{([^{}]*)\}", rot_block):
        values = [int(number) for number in re.findall(r"\d+", match.group(1))]
        if len(values) == COLS:
            rot.append([value & 3 for value in values])
    if len(rot) != ROWS:
        flat = [int(number) for number in re.findall(r"\d+", rot_block)]
        if len(flat) == ROWS * COLS:
            rot = [[flat[y * COLS + x] & 3 for x in range(COLS)] for y in range(ROWS)]
        else:
            raise MapError("C 里解析到 %d 行旋转表，需要 %d 行" % (len(rot), ROWS))

    return clean(RoomMap(rows, rot, name))


def parse_any(text: str, name: str = DEFAULT_NAME) -> Tuple[RoomMap, str]:
    """自动判断是 txt 还是 C；返回 (地图, 格式名)。"""
    head = text[:4096]
    if "room_map_shared" in text or "room_rot_shared" in text or (
        "room_map" in head and "{" in head
    ):
        return parse_c(text, name), "c"
    return parse_text(text, name), "text"
