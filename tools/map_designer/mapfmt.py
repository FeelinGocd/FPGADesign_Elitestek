#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""地图设计器的格式、校验与 C 代码生成 —— 唯一实现。

数据格式（v3，推荐）::

    # MAP_DESIGNER v3
    # TEMPLATE empty
    <23 行 × 40 字符>
    # OBJECTS
    slime 12,8
    bug 20,10
    web 15,12
    stone 10,6
    box 25,15
    crate 27,15
    exit 20,6
    # ROTATION
    <23 行 × 40 个 0..3 数字>

也兼容旧文件：

* v2：'# DEBUG_ROOM_MAP v2'，没有对象层；
* v1：只有 23 行地图，没有 '# ROTATION'（旋转全 0）；
* 稀疏旋转：'# ROTATION sparse' 之后每行 'x,y,r'；
* C 源码：'room_map_shared' / 'room_rot_shared' / 'room_objects'（导入用）；
* 约定 JSON：serialize_json / parse_json（存档 / 加载 / 给固件读）。

字符含义：. 空 / B 基岩地板 / W 木墙 / D 门（占 1×2）/
C X Y Z 角块（翻转）。

几何约定（和 game.h / game_world.c 对齐）：

* 屏幕 1280×720，40×23 格，32 px/格，最后一行只有 16 px；
* 竞技场 288,128 → 1024,544，外面是 2 格厚的墙带；
* 门格 D 在墙带里，旋转值就是方向：0=N 1=E 2=S 3=W；
* D 的“另一半”固定是 +x（横向门）或 +y（纵向门）那一格，必须是 . ；
* 对象坐标是【格坐标】，不是像素；C 侧乘 32 得到左上角。

这个模块不依赖第三方库，serve.py / 单元测试 / CI 共用。
"""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass, field
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

FORMAT_HEADER = "# MAP_DESIGNER v3"
DEFAULT_NAME = "room_map"
DEFAULT_ROOM_TYPE = "empty"
FENCE = chr(96) * 3

#  对象类型。id 必须和 game.h 里的 MAP_OBJ_* 以及 C 侧枚举保持一致。
#  layer: scene = 画在地板之上、人物之下；actor = 和人物同一深度。
OBJECT_KINDS: Dict[str, Dict[str, Any]] = {
    "slime": {"id": 0, "label": "史莱姆", "layer": "actor"},
    "bug": {"id": 1, "label": "甲虫", "layer": "actor"},
    "web": {"id": 2, "label": "蜘蛛网", "layer": "scene"},
    "stone": {"id": 3, "label": "石头", "layer": "actor", "blocker": True},
    "box": {"id": 4, "label": "木箱", "layer": "actor", "blocker": True},
    "crate": {"id": 5, "label": "板条箱", "layer": "actor", "blocker": True},
    "exit": {"id": 6, "label": "出口", "layer": "scene"},
}
OBJECT_BY_ID = {info["id"]: name for name, info in OBJECT_KINDS.items()}
OBJECT_ALIASES = {
    "mob_slime": "slime",
    "mob.slime": "slime",
    "enemy_slime": "slime",
    "mob_bug": "bug",
    "mob.bug": "bug",
    "enemy_bug": "bug",
    "spider_web": "web",
    "rock": "stone",
    "crate_box": "box",
    "eixt": "exit",         # 素材名就是 eixt，顺手兼容这个拼写
}
OBJECT_LIMITS = {"mob": 16, "web": 40, "blocker": 112}

#  普通房类型。数字和 game.h 的 ROOM_START / ROOM_EMPTY / ROOM_ITEM / ROOM_SHOP / ROOM_BOSS 对齐。
ROOM_TYPE_IDS = {"start": 0, "empty": 1, "item": 2, "shop": 3, "boss": 4}
ROOM_TYPE_LABELS = {
    "start": "起点房",
    "empty": "普通房",
    "item": "道具房",
    "shop": "商店房",
    "boss": "Boss 大房间",
}


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


@dataclass(frozen=True)
class MapObject:
    kind: str
    x: int
    y: int
    arg: int = 0

    def as_dict(self) -> Dict[str, Any]:
        return {
            "kind": self.kind,
            "x": self.x,
            "y": self.y,
            "arg": self.arg,
            "label": OBJECT_KINDS[self.kind]["label"],
            "id": OBJECT_KINDS[self.kind]["id"],
        }


@dataclass
class RoomMap:
    rows: List[List[str]]
    rot: List[List[int]]
    name: str = DEFAULT_NAME
    objects: List[MapObject] = field(default_factory=list)
    room_type: str = DEFAULT_ROOM_TYPE


# ---------------------------------------------------------------------------
#  解析 / 序列化
# ---------------------------------------------------------------------------

_SECTION_RE = re.compile(r"^#\s*(ROTATION|OBJECTS|ENTITIES|TEMPLATE|ROOM_TYPE)\b(.*)$", re.I)


def _sections(lines: Sequence[str]) -> List[Tuple[str, str, List[str]]]:
    """把文本切成 (section, 行内参数, 内容行) 列表。

    段标记是 '# OBJECTS' / '# ROTATION' / '# TEMPLATE' / '# ROOM_TYPE'。
    标记之前的非注释行属于 'map' 段；'# TEMPLATE' 之后的地图行也留在
    TEMPLATE 段里，由 parse_text 识别出来。
    """
    sections: List[Tuple[str, str, List[str]]] = []
    tag = "map"
    args = ""
    body: List[str] = []

    for raw in lines:
        text = raw.rstrip()
        stripped = text.strip()
        if not stripped:
            continue
        if stripped.startswith("#"):
            match = _SECTION_RE.match(stripped)
            if match:
                sections.append((tag, args, body))
                tag = match.group(1).lower()
                args = match.group(2).strip()
                body = []
            continue
        body.append(text)
    sections.append((tag, args, body))
    return sections


def _normal_kind(raw: str) -> str:
    kind = raw.strip().lower().replace("-", "_")
    kind = OBJECT_ALIASES.get(kind, kind)
    if kind not in OBJECT_KINDS:
        raise MapError("未知对象类型 %r（可用：%s）" % (raw, "、".join(OBJECT_KINDS)))
    return kind


def parse_object_line(line: str) -> MapObject:
    parts = [part for part in re.split(r"[\s,]+", line.strip()) if part]
    if len(parts) < 3:
        raise MapError("对象要写成 kind x,y [arg]：%r" % line)
    if len(parts) > 4:
        raise MapError("对象只有 4 列（kind x y arg）：%r" % line)
    kind = _normal_kind(parts[0])
    try:
        x, y = int(parts[1]), int(parts[2])
        arg = int(parts[3]) if len(parts) > 3 else 0
    except ValueError:
        raise MapError("对象坐标 / 参数不是整数：%r" % line) from None
    if not (0 <= x < COLS and 0 <= y < ROWS):
        raise MapError("对象坐标越界：(%d,%d)" % (x, y))
    if not 0 <= arg <= 255:
        raise MapError("对象参数必须在 0..255：%r" % line)
    return MapObject(kind, x, y, arg)


def parse_text(text: str, name: str = DEFAULT_NAME) -> RoomMap:
    """解析编辑器保存的 txt（v1 / v2 / v3 / 稀疏旋转都吃）。"""
    if not isinstance(text, str):
        raise MapError("地图内容必须是字符串")
    source = text.replace("\ufeff", "").replace("\r\n", "\n").replace("\r", "\n")
    lines = source.split("\n")

    map_lines: List[str] = []
    object_lines: List[str] = []
    rot_lines: List[str] = []
    rot_args = ""
    saw_rotation = False
    room_type = DEFAULT_ROOM_TYPE

    for tag, args, body in _sections(lines):
        if tag == "rotation":
            saw_rotation = True
            rot_args = args.lower()
            rot_lines.extend(body)
            continue
        if tag in ("objects", "entities"):
            object_lines.extend(body)
            continue
        if tag in ("template", "room_type"):
            value = args.strip().lower()
            if not value and body:
                first = body[0].strip().lower()
                if first in ROOM_TYPE_IDS:
                    value = first
                    body = body[1:]
            if value:
                if value not in ROOM_TYPE_IDS:
                    raise MapError(
                        "房间类型 %r 不认识（可用：%s）"
                        % (value, "、".join(ROOM_TYPE_IDS))
                    )
                room_type = value
            map_lines.extend(body)
            continue
        map_lines.extend(body)

    if len(map_lines) != ROWS:
        raise MapError(
            "地图需要 %d 行，实际 %d 行（空行和 # 注释行不算；行首也别缩进）"
            % (ROWS, len(map_lines))
        )

    rows: List[List[str]] = []
    for number, line in enumerate(map_lines, 1):
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
    if saw_rotation:
        rot_lines = [
            line.strip()
            for line in rot_lines
            if line.strip() and not line.strip().startswith("#")
        ]
        sparse = rot_args == "sparse" or any(
            ("," in line) or (" " in line) or ("\t" in line) for line in rot_lines
        )
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

    objects = [parse_object_line(line) for line in object_lines]
    return RoomMap(rows, rot, name, objects, room_type)


def _clean_objects(objects: Iterable[MapObject]) -> List[MapObject]:
    out: List[MapObject] = []
    seen = set()
    for obj in objects:
        if obj.kind not in OBJECT_KINDS:
            continue
        if not (0 <= obj.x < COLS and 0 <= obj.y < ROWS):
            continue
        key = (obj.kind, obj.x, obj.y)
        if key in seen:
            continue
        seen.add(key)
        out.append(MapObject(obj.kind, obj.x, obj.y, int(obj.arg) & 0xFF))
    out.sort(key=lambda item: (item.y, item.x, OBJECT_KINDS[item.kind]["id"]))
    return out


def serialize_text(room: RoomMap, sparse: bool = False) -> str:
    """写回 v3 文本；默认写满 23×40 的旋转表。"""
    room_type = room.room_type if room.room_type in ROOM_TYPE_IDS else DEFAULT_ROOM_TYPE
    lines: List[str] = [FORMAT_HEADER, "# TEMPLATE %s" % room_type]
    lines.extend("".join(row) for row in room.rows)

    objects = _clean_objects(room.objects)
    if objects:
        lines.append("# OBJECTS")
        for obj in objects:
            if obj.arg:
                lines.append("%s %d,%d,%d" % (obj.kind, obj.x, obj.y, obj.arg))
            else:
                lines.append("%s %d,%d" % (obj.kind, obj.x, obj.y))

    if sparse:
        triples = [
            "%d,%d,%d" % (x, y, room.rot[y][x] & 3)
            for y in range(ROWS)
            for x in range(COLS)
            if room.rot[y][x]
        ]
        lines.append("# ROTATION sparse")
        if triples:
            lines.extend(triples)
    else:
        lines.append("# ROTATION")
        lines.extend(
            "".join(str(int(value) & 3) for value in row) for row in room.rot
        )
    return "\n".join(lines) + "\n"


def clean(room: RoomMap) -> RoomMap:
    """空白格的旋转没有意义，统一归零；对象去重 / 排序 / 夹回参数范围。"""
    rows = [list(row) for row in room.rows]
    rot = [[(int(value) & 3) for value in row] for row in room.rot]
    for y in range(ROWS):
        for x in range(COLS):
            if rows[y][x] == ".":
                rot[y][x] = 0
    room_type = room.room_type if room.room_type in ROOM_TYPE_IDS else DEFAULT_ROOM_TYPE
    return RoomMap(rows, rot, room.name, _clean_objects(room.objects), room_type)


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

    if room.room_type not in ROOM_TYPE_IDS:
        add("error", "房间类型 %r 不认识" % room.room_type)

    # ---- 区域 ------------------------------------------------------------
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

    # ---- 对象层 ----------------------------------------------------------
    if room.room_type == "boss":
        add("warning", "Boss 大房间是 45×35；当前设计器按普通房 40×23 输出，生成 C 前请确认尺寸")

    seen_objects = set()
    cell_blockers: Dict[Tuple[int, int], str] = {}
    counts = {"mob": 0, "web": 0, "blocker": 0, "exit": 0}
    for obj in room.objects:
        info = OBJECT_KINDS.get(obj.kind)
        if info is None:
            add("error", "未知对象类型 %r" % obj.kind, obj.x, obj.y)
            continue
        if not (0 <= obj.x < COLS and 0 <= obj.y < ROWS):
            add("error", "对象 %s 坐标越界 (%d,%d)" % (obj.kind, obj.x, obj.y), obj.x, obj.y)
            continue

        if obj.kind in ("slime", "bug"):
            counts["mob"] += 1
        elif obj.kind == "web":
            counts["web"] += 1
        elif obj.kind == "exit":
            counts["exit"] += 1
        if info.get("blocker"):
            counts["blocker"] += 1

        key = (obj.kind, obj.x, obj.y)
        if key in seen_objects:
            add("warning", "同一格重复放了 %s (%d,%d)" % (info["label"], obj.x, obj.y), obj.x, obj.y)
        seen_objects.add(key)

        if not in_arena(obj.x, obj.y):
            add("error", "%s (%d,%d) 在竞技场外，C 侧不会生成" % (info["label"], obj.x, obj.y), obj.x, obj.y)
            continue
        if room.rows[obj.y][obj.x] != "B":
            add(
                "error",
                "%s (%d,%d) 必须放在基岩地板 B 上，当前是 %r"
                % (info["label"], obj.x, obj.y, room.rows[obj.y][obj.x]),
                obj.x, obj.y,
            )

        if info.get("blocker"):
            other = cell_blockers.get((obj.x, obj.y))
            if other:
                add(
                    "warning",
                    "(%d,%d) 同时放了 %s 和 %s，两个阻挡物会重叠"
                    % (obj.x, obj.y, OBJECT_KINDS[other]["label"], info["label"]),
                    obj.x, obj.y,
                )
            else:
                cell_blockers[(obj.x, obj.y)] = obj.kind

    if counts["mob"] > OBJECT_LIMITS["mob"]:
        add("warning", "小怪 %d 只，超过 N_MOBS=%d，C 侧会截断" % (counts["mob"], OBJECT_LIMITS["mob"]))
    if counts["web"] > OBJECT_LIMITS["web"]:
        add("warning", "蜘蛛网 %d 张，超过 N_WEBS=%d" % (counts["web"], OBJECT_LIMITS["web"]))
    if counts["blocker"] > OBJECT_LIMITS["blocker"]:
        add("warning", "阻挡物 %d 个，超过 N_STONES=%d" % (counts["blocker"], OBJECT_LIMITS["blocker"]))
    if counts["exit"] > 1:
        add("warning", "放了 %d 个出口，通常一个房间只有 1 个" % counts["exit"])

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
    object_counts = {kind: 0 for kind in OBJECT_KINDS}
    for obj in room.objects:
        object_counts[obj.kind] += 1
    return {
        "counts": counts,
        "doors": doors,
        "objects": [obj.as_dict() for obj in room.objects],
        "object_counts": object_counts,
        "room_type": room.room_type,
        "template_id": ROOM_TYPE_IDS.get(room.room_type, -1),
        "size": [COLS, ROWS],
        "sha256": digest_of(room),
    }


# ---------------------------------------------------------------------------
#  C 代码生成 / 解析
# ---------------------------------------------------------------------------

def emit_c(room: RoomMap, source: str = "tools/map_designer/output/room_map.txt") -> str:
    """生成独立的数据文件（static 数组，可单独编译，也可被 include）。"""
    room = clean(room)
    info = stats(room)
    counts = info["counts"]
    object_counts = info["object_counts"]
    doors = " ".join(
        "%s(%d,%d)" % (door["dir"], door["x"], door["y"]) for door in info["doors"]
    ) or "（无）"
    object_summary = " ".join(
        "%s×%d" % (OBJECT_KINDS[kind]["label"], count)
        for kind, count in object_counts.items()
        if count
    ) or "（无）"
    digest = info["sha256"]
    room_type = room.room_type
    template_id = ROOM_TYPE_IDS[room_type]

    lines: List[str] = []
    lines.append("/* " + "=" * 74)
    lines.append(" *  game_world_map.c —— 房间地图数据（自动生成，别手改）")
    lines.append(" *  " + "-" * 73)
    lines.append(" *  生成器 : tools/map_designer/serve.py --emit-c")
    lines.append(" *  源文件 : %s" % source)
    lines.append(" *  指纹   : sha256:%s" % digest)
    lines.append(" *  模板   : %s（id %d）" % (room_type, template_id))
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
    lines.append(" *  对象   : %s" % object_summary)
    lines.append(" *  " + "-" * 73)
    lines.append(" *  用法：把两张地图表 + room_objects 整体替换进 game_world.c，或者")
    lines.append(' *        先把 MAP_STORAGE_ROWS/COLS 定义成 MAP_MAX_ROWS/COLS，再')
    lines.append(' *        #include "game_world_map.c"。后一种做法让普通房地图和对象层')
    lines.append(" *        始终来自同一个文件。")
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
    lines.append("#ifndef MAP_STORAGE_ROWS")
    lines.append("#define MAP_STORAGE_ROWS MAP_ROWS")
    lines.append("#endif")
    lines.append("#ifndef MAP_STORAGE_COLS")
    lines.append("#define MAP_STORAGE_COLS MAP_COLS")
    lines.append("#endif")
    lines.append("")
    lines.append('#define ROOM_MAP_SHA256 "%s"' % digest)
    lines.append("#define MAP_TEMPLATE_ROOM %d   /* %s */" % (template_id, room_type))
    lines.append('#define MAP_TEMPLATE_NAME "%s"' % room_type)
    lines.append("")
    for kind, kind_info in OBJECT_KINDS.items():
        macro = "MAP_OBJ_%s" % kind.upper()
        lines.append("#ifndef %s" % macro)
        lines.append("#define %s %d" % (macro, kind_info["id"]))
        lines.append("#endif")
    lines.append("")
    lines.append("static const char room_map_shared[MAP_STORAGE_ROWS][MAP_STORAGE_COLS + 1] = {")
    for row in room.rows:
        lines.append('    "%s",' % "".join(row))
    lines.append("};")
    lines.append("")
    lines.append("static const unsigned char room_rot_shared[MAP_STORAGE_ROWS][MAP_STORAGE_COLS] = {")
    for row in room.rot:
        lines.append("    { %s }," % ", ".join(str(value) for value in row))
    lines.append("};")
    lines.append("")
    lines.append("#define ROOM_OBJECT_COUNT %d" % len(room.objects))
    lines.append("/* kind, x, y, arg（坐标是 32 px 格，不是像素） */")
    lines.append(
        "static const unsigned char room_objects[ROOM_OBJECT_COUNT ? ROOM_OBJECT_COUNT : 1][4] = {"
    )
    for obj in room.objects:
        lines.append(
            "    { %d, %d, %d, %d },  /* %s */"
            % (
                OBJECT_KINDS[obj.kind]["id"],
                obj.x,
                obj.y,
                obj.arg,
                obj.kind,
            )
        )
    if not room.objects:
        lines.append("    { 0, 0, 0, 0 }   /* 占位：ROOM_OBJECT_COUNT == 0 */")
    lines.append("};")
    lines.append("")
    lines.append("#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L")
    lines.append('_Static_assert(sizeof(room_map_shared[0]) == MAP_STORAGE_COLS + 1, "room_map row width");')
    lines.append('_Static_assert(sizeof(room_rot_shared[0]) == MAP_STORAGE_COLS, "room_rot row width");')
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
    """从 C 源码（生成文件或 game_world.c）里把地图 / 对象读回来。"""
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

    objects: List[MapObject] = []
    count_match = re.search(r"#define\s+ROOM_OBJECT_COUNT\s+(\d+)", source)
    object_count = int(count_match.group(1)) if count_match else None
    try:
        object_block = _find_block(source, ("room_objects", "map_objects"))
    except MapError:
        object_block = ""
    if object_block:
        rows_found = re.findall(
            r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}", object_block
        )
        if object_count is not None:
            rows_found = rows_found[:object_count]
        for kind_id, x, y, arg in rows_found:
            kind = OBJECT_BY_ID.get(int(kind_id))
            if kind is None:
                continue
            objects.append(MapObject(kind, int(x), int(y), int(arg)))

    type_match = re.search(r'#define\s+MAP_TEMPLATE_NAME\s+"([^"]+)"', source)
    room_type = DEFAULT_ROOM_TYPE
    if type_match:
        value = type_match.group(1).strip().lower()
        if value in ROOM_TYPE_IDS:
            room_type = value

    return clean(RoomMap(rows, rot, name, objects, room_type))


def serialize_json(room: RoomMap) -> str:
    """输出约定 JSON 文件：可存档、可回读，不依赖 C 代码。"""
    room = clean(room)
    payload = {
        "format": "map_designer",
        "version": 3,
        "room_type": room.room_type,
        "size": {"cols": COLS, "rows": ROWS, "tile": TILE},
        "tiles": ["".join(row) for row in room.rows],
        "rotation": [list(row) for row in room.rot],
        "objects": [
            {"kind": obj.kind, "x": obj.x, "y": obj.y, "arg": obj.arg}
            for obj in room.objects
        ],
        "sha256": digest_of(room),
    }
    return json.dumps(payload, ensure_ascii=False, indent=2) + "\n"


def parse_json(text: str, name: str = DEFAULT_NAME) -> RoomMap:
    """解析约定 JSON 文件（serialize_json 的产物）。"""
    if not isinstance(text, str):
        raise MapError("JSON 内容必须是字符串")
    try:
        data = json.loads(text.replace("\ufeff", ""))
    except json.JSONDecodeError as exc:
        raise MapError("JSON 解析失败：%s" % exc) from None
    if not isinstance(data, dict):
        raise MapError("JSON 顶层必须是对象")

    size = data.get("size") or {}
    if isinstance(size, dict):
        if "cols" in size and int(size["cols"]) != COLS:
            raise MapError("JSON 尺寸 cols=%r，需要 %d" % (size["cols"], COLS))
        if "rows" in size and int(size["rows"]) != ROWS:
            raise MapError("JSON 尺寸 rows=%r，需要 %d" % (size["rows"], ROWS))

    tiles = data.get("tiles", data.get("map"))
    if not isinstance(tiles, list) or len(tiles) != ROWS:
        raise MapError("JSON 需要 %d 行 tiles，实际 %r" % (ROWS, len(tiles) if isinstance(tiles, list) else type(tiles).__name__))
    rows: List[List[str]] = []
    for index, line in enumerate(tiles, 1):
        if not isinstance(line, str) or len(line) != COLS:
            raise MapError("JSON 第 %d 行不是 %d 字符串：%r" % (index, COLS, line))
        bad = sorted(set(line) - ALLOWED)
        if bad:
            raise MapError("JSON 第 %d 行有非法字符 %r" % (index, "".join(bad)))
        rows.append(list(line))

    rot = [[0] * COLS for _ in range(ROWS)]
    rotation = data.get("rotation")
    if rotation is not None:
        if not isinstance(rotation, list) or len(rotation) != ROWS:
            raise MapError("JSON rotation 需要 %d 行" % ROWS)
        for y, line in enumerate(rotation):
            if not isinstance(line, list) or len(line) != COLS:
                raise MapError("JSON rotation 第 %d 行需要 %d 个数" % (y + 1, COLS))
            for x, value in enumerate(line):
                try:
                    value = int(value)
                except (TypeError, ValueError):
                    raise MapError("JSON rotation (%d,%d) 不是整数：%r" % (x, y, value)) from None
                if value not in (0, 1, 2, 3):
                    raise MapError("JSON rotation (%d,%d) 不在 0..3：%r" % (x, y, value))
                rot[y][x] = value

    objects: List[MapObject] = []
    raw_objects = data.get("objects", [])
    if raw_objects is None:
        raw_objects = []
    if not isinstance(raw_objects, list):
        raise MapError("JSON objects 必须是数组")
    for item in raw_objects:
        if not isinstance(item, dict):
            raise MapError("JSON objects 里的项必须是对象：%r" % (item,))
        kind = _normal_kind(str(item.get("kind", "")))
        try:
            x = int(item.get("x"))
            y = int(item.get("y"))
            arg = int(item.get("arg", 0))
        except (TypeError, ValueError):
            raise MapError("JSON 对象坐标 / 参数不是整数：%r" % (item,)) from None
        if not (0 <= x < COLS and 0 <= y < ROWS):
            raise MapError("JSON 对象坐标越界：(%d,%d)" % (x, y))
        if not 0 <= arg <= 255:
            raise MapError("JSON 对象参数必须在 0..255：%r" % (item,))
        objects.append(MapObject(kind, x, y, arg))

    room_type = str(data.get("room_type", DEFAULT_ROOM_TYPE)).strip().lower()
    if room_type not in ROOM_TYPE_IDS:
        raise MapError("JSON 房间类型不认识：%r" % room_type)
    return RoomMap(rows, rot, name, objects, room_type)


def parse_any(text: str, name: str = DEFAULT_NAME) -> Tuple[RoomMap, str]:
    """自动判断是 txt / JSON / C；返回 (地图, 格式名)。"""
    head = text[:4096]
    if head.lstrip().startswith("{") or ('"format"' in head and "map_designer" in head):
        return parse_json(text, name), "json"
    if (
        "room_map_shared" in text
        or "room_rot_shared" in text
        or "ROOM_OBJECT_COUNT" in text
        or ("room_map" in head and "{" in head)
    ):
        return parse_c(text, name), "c"
    return parse_text(text, name), "text"
