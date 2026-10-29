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

字符含义：. 空 / B 木地板 / K 基岩地板 / S 石头地板 / W 木墙 /
D 门（占 1×2）/ H 隐藏门 / C X Y Z 角块（翻转）。

几何约定（和 game.h / game_world.c 对齐）：

* 画布尺寸【自由】：cols×rows 格，32 px/格；默认 40×23，上限 45×35
  （= game.h 的 MAP_MAX_COLS / MAP_MAX_ROWS），下限 8×8；
* 尺寸从数据本身读 —— 多少行就是 rows，每行多少字符就是 cols，
  没有全局常量，同一份工具可以画 1×1（40×23）和 2×2 / L（45×35）；
* 竞技场 = 地板 B 的包围盒；墙带 = 包围盒外扩 2 格的那一圈；
* 1280×720 只是【参考框】（页面上画给你看），不参与校验、不落盘；
* 门格 D 在墙带里，旋转值就是方向：0=N 1=E 2=S 3=W；
* D 的“另一半”固定是 +x（横向门）或 +y（纵向门）那一格，必须是 . ；
* 对象坐标是【格坐标】，不是像素；C 侧乘 32 得到左上角。

这个模块不依赖第三方库，serve.py / 单元测试 / CI 共用。
"""

from __future__ import annotations

import hashlib
import json
import os
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

DEFAULT_COLS = 40               # 默认画布：40×23 格 = 1280×736
DEFAULT_ROWS = 23
MAX_COLS = 45                   # 上限 = game.h 的 MAP_MAX_COLS / MAP_MAX_ROWS
MAX_ROWS = 35
MIN_COLS = 8                    # 下限：再小就摆不下竞技场
MIN_ROWS = 8
COLS = DEFAULT_COLS             # 兼容旧引用；实际尺寸一律取 room.cols / room.height
ROWS = DEFAULT_ROWS
TILE = 32
CANVAS_W = 1280                 # 720p 参考框（只是参考，不参与校验）
CANVAS_H = 720
BAND = 2                        # 墙带厚度（格）

#  ★ 小写 = 隐藏房皮肤：w 隐藏房墙 / c,x,y,z 隐藏房角块（和 C/X/Y/Z 一一对应）
ALLOWED = frozenset(".BWDHKCXYZS" + "wcxyz")
#  JSON 存档的数字笔刷编号：0..7 = 面板顺序，8..10 = 角块翻转 X/Y/Z，11..15 = 隐藏房皮肤
TILE_INDEX = {
    ".": 0, "B": 1, "W": 2, "D": 3, "C": 4, "H": 5, "K": 6, "S": 7,
    "X": 8, "Y": 9, "Z": 10,
    "w": 11, "c": 12, "x": 13, "y": 14, "z": 15,
}
TILE_CHAR = {value: key for key, value in TILE_INDEX.items()}
DOOR_CHARS = frozenset("DH")      # D = 普通门，H = 隐藏门（炸弹炸开才通）
FLOOR_CHARS = frozenset("BKS")    # B = 木地板，K = 基岩地板，S = 石头地板（贴图不同，规则一样）
CORNER = frozenset("CXYZ")
DIR_NAME = {0: "N", 1: "E", 2: "S", 3: "W"}

FORMAT_HEADER = "# MAP_DESIGNER v3"
DEFAULT_NAME = "room_map"
#  旧文本里的 '# TEMPLATE start|empty|item|shop|boss'：认识，但只是历史遗留，不再有含义
LEGACY_TYPE_WORDS = frozenset(("start", "empty", "item", "shop", "boss"))
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

#  笔刷角色 → 候选文件名片段（按优先级）。改图只改 res/pic，这里不用动；
#  找不到的角色会在页面上显示成占位色块，并在 /api/assets 的 missing 里列出来。
#  ★ 放在 mapfmt 里是为了能热重载：改这张表不用重启 serve.py。
ASSET_ROLES = (
    ("wood", "木地板", ("background_floor_wood",)),
    ("hidden_floor", "隐藏房地板", ("background_floor_hidden",)),
    ("wall", "木墙", ("background_wall_wood",)),
    ("hidden_wall", "隐藏房墙", ("background_wall_hidden",)),
    ("corner", "角块", ("background_corner_wood",)),
    ("hidden_corner", "隐藏房角块", ("background_corner_hidden",)),
    ("door", "门", ("background_door_normal", "background_door")),
    ("slime", "史莱姆", ("mobs_slime",)),
    ("bug", "甲虫", ("mobs_bug",)),
    ("web", "蜘蛛网", ("background_interact_web",)),
    ("stone", "石头", ("background_interact_stone",)),
    ("crate", "板条箱", ("background_interact_crate",)),
    ("box", "木箱", ("background_interact_box_close", "background_interact_box")),
    ("exit", "出口", ("background_interact_eixt", "background_interact_exit")),
    ("hide", "隐藏入口", ("background_interact_hidden",)),
)

#  地图模板：只是"起手骨架"，按【形状】分 —— 设计器不认识 start / item / boss 这些
#  玩法概念，那些是固件 g_kinds[] 的事。所有房间共用同一个 map 池，一张图就是一张图。
#    · template_1x1：40×23 现场生成（地板默认 B 木地板，和固件 1×1 框架一致）
#    · template_L / template_2x2：整份地图存成 JSON（templates/ 下，和房间同一套格式），
#      想加一种形状 = 丢一个 JSON 进去、在这里登记一行。
TEMPLATES: Dict[str, Dict[str, Any]] = {
    "template_1x1": {"label": "1×1（40×23）", "file": "maps/1x1.json"},
    "template_L":   {"label": "L 形·左下空缺（45×35）", "file": "maps/L.json"},
    "template_2x2": {"label": "2×2（45×35）", "file": "maps/2x2.json"},
    "template_hidden": {"label": "隐藏房（40×23）", "file": "maps/hidden.json"},
}

#  老名字（脚本/旧习惯）→ 新模板名
TEMPLATE_ALIASES = {"small": "template_1x1", "big": "template_2x2"}

#  自定义模板：只写 templates/custom/，绝不覆盖上面的内置模板。
#  文件名统一 custom_<slug>.json；slug 只允许 ASCII 字母/数字/_/-，
#  显示名可以中文，写在 JSON 的 "label" 字段里（parse_json 会忽略它）。
TEMPLATE_ROOT = Path(__file__).resolve().parent
CUSTOM_TEMPLATE_DIR = TEMPLATE_ROOT / "templates" / "custom"
CUSTOM_TEMPLATE_PREFIX = "custom_"
CUSTOM_TEMPLATE_RE = re.compile(r"^custom_[A-Za-z0-9][A-Za-z0-9_-]{0,47}$")
CUSTOM_SLUG_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]{0,47}$")


class MapError(ValueError):
    """地图文本 / C 源码格式错误。"""


def check_size(cols: int, rows: int) -> None:
    """画布尺寸必须在 MIN..MAX 之间；越界一律当格式错误（不是警告）。"""
    if not (MIN_COLS <= cols <= MAX_COLS):
        raise MapError(
            "画布宽度 %d 格超出范围：%d ~ %d（32 px/格，%d 格 = %d px）"
            % (cols, MIN_COLS, MAX_COLS, MAX_COLS, MAX_COLS * TILE)
        )
    if not (MIN_ROWS <= rows <= MAX_ROWS):
        raise MapError(
            "画布高度 %d 格超出范围：%d ~ %d（32 px/格，%d 格 = %d px）"
            % (rows, MIN_ROWS, MAX_ROWS, MAX_ROWS, MAX_ROWS * TILE)
        )


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
    layout_id: Optional[int] = None      # 房型内部的稳定设计编号；不是 C 数组下标

    @property
    def cols(self) -> int:
        """画布宽度（格）—— 每行多少字符就是多少列。"""
        return len(self.rows[0]) if self.rows else 0

    @property
    def height(self) -> int:
        """画布高度（格）—— 多少行就是多少行。"""
        return len(self.rows)

    @property
    def size(self) -> Tuple[int, int]:
        return (self.cols, self.height)


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


def parse_object_line(line: str, cols: int = COLS, rows: int = ROWS) -> MapObject:
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
    if not (0 <= x < cols and 0 <= y < rows):
        raise MapError("对象坐标越界：(%d,%d)，画布是 %d×%d" % (x, y, cols, rows))
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

    for tag, args, body in _sections(lines):
        if tag == "rotation":
            saw_rotation = True
            rot_args = args.lower()
            rot_lines.extend(body)
            continue
        if tag in ("objects", "entities"):
            object_lines.extend(body)
            continue
        if tag in ("template", "room_type", "name"):
            # 旧文件会写 '# TEMPLATE empty'；现在一张图就是一张图，这个值忽略。
            # 兼容两种旧写法：值跟在段标记后面，或者单独占第一行。
            rest = body
            if rest and rest[0].strip().lower() in LEGACY_TYPE_WORDS:
                rest = rest[1:]
            map_lines.extend(rest)
            continue
        map_lines.extend(body)

    if not map_lines:
        raise MapError("地图是空的：至少要 %d 行" % MIN_ROWS)
    height = len(map_lines)
    cols = len(map_lines[0])
    check_size(cols, height)

    grid: List[List[str]] = []
    for number, line in enumerate(map_lines, 1):
        if len(line) != cols:
            raise MapError(
                "第 %d 行长度 %d，和第一行的 %d 不一致（行首空格也会算进去）"
                % (number, len(line), cols)
            )
        bad = sorted(set(line) - ALLOWED)
        if bad:
            raise MapError("第 %d 行有非法字符 %r" % (number, "".join(bad)))
        grid.append(list(line))

    rot = [[0] * cols for _ in range(height)]
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
                if not (0 <= x < cols and 0 <= y < height):
                    raise MapError("旋转坐标越界：(%d,%d)" % (x, y))
                if not 0 <= r <= 3:
                    raise MapError("旋转值必须是 0..3：%r" % line)
                rot[y][x] = r
        else:
            if len(rot_lines) < height:
                raise MapError("旋转表需要 %d 行，实际 %d 行" % (height, len(rot_lines)))
            for y in range(height):
                line = rot_lines[y]
                if len(line) != cols:
                    raise MapError(
                        "旋转表第 %d 行长度 %d，需要 %d" % (y + 1, len(line), cols)
                    )
                for x, char in enumerate(line):
                    if char not in "0123":
                        raise MapError(
                            "旋转表第 %d 行第 %d 列不是 0..3：%r" % (y + 1, x + 1, char)
                        )
                    rot[y][x] = int(char)

    objects = [parse_object_line(line, cols, height) for line in object_lines]
    return RoomMap(grid, rot, name, objects)


def _clean_objects(objects: Iterable[MapObject], cols: int, rows: int) -> List[MapObject]:
    out: List[MapObject] = []
    seen = set()
    for obj in objects:
        if obj.kind not in OBJECT_KINDS:
            continue
        if not (0 <= obj.x < cols and 0 <= obj.y < rows):
            continue
        key = (obj.kind, obj.x, obj.y)
        if key in seen:
            continue
        seen.add(key)
        out.append(MapObject(obj.kind, obj.x, obj.y, int(obj.arg) & 0xFF))
    out.sort(key=lambda item: (item.y, item.x, OBJECT_KINDS[item.kind]["id"]))
    return out


def serialize_text(room: RoomMap, sparse: bool = False) -> str:
    """写回 v3 文本；尺寸跟着地图走，默认写满整张画布的旋转表。"""
    lines: List[str] = [FORMAT_HEADER]
    lines.extend("".join(row) for row in room.rows)

    objects = _clean_objects(room.objects, room.cols, room.height)
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
            for y in range(room.height)
            for x in range(room.cols)
            if x < len(room.rot[y]) and room.rot[y][x]
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
    """空白格的旋转没有意义，统一归零；对象去重 / 排序 / 夹回参数范围。

    尺寸按 room 本身补齐成矩形（手工拼的 RoomMap 行长度不齐也不会炸）。
    """
    cols, height = room.cols, room.height
    rows = [
        list(row)[:cols] + ["."] * max(0, cols - len(row))
        for row in room.rows
    ]
    rot: List[List[int]] = []
    for y in range(height):
        line = room.rot[y] if y < len(room.rot) else []
        rot.append([
            (int(line[x]) & 3) if x < len(line) else 0
            for x in range(cols)
        ])
    for y in range(height):
        for x in range(cols):
            if rows[y][x] == ".":
                rot[y][x] = 0
    return RoomMap(rows, rot, room.name, _clean_objects(room.objects, cols, height))


def digest_of(room: RoomMap) -> str:
    text = serialize_text(clean(room)).encode("utf-8")
    return hashlib.sha256(text).hexdigest()


# ---------------------------------------------------------------------------
#  几何
# ---------------------------------------------------------------------------

def arena_rect(room: RoomMap) -> Tuple[int, int, int, int]:
    """竞技场 = 地板 B 的包围盒（含边界格）：(x0, y0, x1, y1)。

    尺寸自由之后不再有"288,128 → 1024,544"这种像素常数：你画在哪，
    竞技场就在哪。地图里一格 B 都没有时退回整张画布，validate 会单独报错。
    """
    xs: List[int] = []
    ys: List[int] = []
    for y, row in enumerate(room.rows):
        for x, char in enumerate(row):
            if char in FLOOR_CHARS:
                xs.append(x)
                ys.append(y)
    if not xs:
        return (0, 0, max(room.cols - 1, 0), max(room.height - 1, 0))
    return (min(xs), min(ys), max(xs), max(ys))


def in_arena(room: RoomMap, x: int, y: int) -> bool:
    x0, y0, x1, y1 = arena_rect(room)
    return x0 <= x <= x1 and y0 <= y <= y1


def wall_sides(room: RoomMap, x: int, y: int) -> List[str]:
    """这一格属于墙带的哪一边（墙角返回两条，竞技场内部返回空）。

    墙带 = 竞技场包围盒外扩 BAND 格的那一圈，不含竞技场本身。
    """
    x0, y0, x1, y1 = arena_rect(room)
    if x0 <= x <= x1 and y0 <= y <= y1:
        return []
    if not (x0 - BAND <= x <= x1 + BAND and y0 - BAND <= y <= y1 + BAND):
        return []
    sides: List[str] = []
    if y0 - BAND <= y <= y0 - 1:
        sides.append("N")
    if y1 + 1 <= y <= y1 + BAND:
        sides.append("S")
    if x0 - BAND <= x <= x0 - 1:
        sides.append("W")
    if x1 + 1 <= x <= x1 + BAND:
        sides.append("E")
    return sides


def in_wall_band(room: RoomMap, x: int, y: int) -> bool:
    return bool(wall_sides(room, x, y))


def door_pair(x: int, y: int, angle: int) -> Tuple[int, int]:
    """门格 D 的“另一半”坐标：横向门在右，纵向门在下。"""
    return (x + 1, y) if (angle & 1) else (x, y + 1)


def door_slots(room: RoomMap) -> List[Tuple[int, int, int]]:
    """四扇门的建议槽位 (x, y, 方向)：各边墙带的正中，门洞朝 +x / +y。"""
    x0, y0, x1, y1 = arena_rect(room)
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    return [
        (cx, y0 - BAND, 0),
        (x1 + 1, cy, 1),
        (cx, y1 + 1, 2),
        (x0 - BAND, cy, 3),
    ]


def make_room(
    cols: int = DEFAULT_COLS,
    rows: int = DEFAULT_ROWS,
    arena: Optional[Tuple[int, int, int, int]] = None,
    name: str = DEFAULT_NAME,
    corner: int = 0,
    band_rot: Sequence[int] = (0, 0, 0, 0),
    floor: str = "B",
) -> RoomMap:
    """生成一间"空房间"骨架：地板 + 2 格墙带 + 四扇门。

    arena 是 (x0, y0, x1, y1) 格坐标；不给就四周各留 4 格（2 格黑边 + 2 格墙带）。
    返回的图能直接过 validate：门方向对、门洞朝 +x / +y、墙带没有额外空洞。
    """
    check_size(cols, rows)
    if arena is None:
        m = 4
        arena = (m, m, cols - m - 1, rows - m - 1)
    x0, y0, x1, y1 = arena
    if not (BAND <= x0 <= x1 <= cols - BAND - 1 and BAND <= y0 <= y1 <= rows - BAND - 1):
        raise MapError(
            "竞技场 %r 放不进 %d×%d 的画布（四周要各留 2 格墙带）" % (arena, cols, rows)
        )

    # 骨架 = 一个标准框架参数，再用生成器铺满（和固件 game_frame.c 同一套规则）
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    frame = {
        "cols": cols,
        "rows": rows,
        "arena": [x0, y0, x1, y1],
        "doors": [cx, cy, cx, cy],
        "band_rot": [int(v) & 3 for v in band_rot],
        "corner": int(corner) & 1,
    }
    grid = [[frame_tile(frame, x, y) for x in range(cols)] for y in range(rows)]
    rot = [[frame_rot(frame, x, y) for x in range(cols)] for y in range(rows)]
    if floor not in FLOOR_CHARS:
        raise MapError("地板字符 %r 不认识（可用：%s）" % (floor, "".join(sorted(FLOOR_CHARS))))
    if floor != "B":
        for y in range(rows):
            for x in range(cols):
                if grid[y][x] == "B":
                    grid[y][x] = floor
    return RoomMap(grid, rot, name, [])


def custom_template_slug(raw: str) -> str:
    """把用户输入的模板名规范化成 custom_<slug>；只允许 ASCII 文件名。"""
    slug = (raw or "").strip()
    if slug.lower().endswith(".json"):
        slug = slug[:-5]
    if slug.lower().startswith(CUSTOM_TEMPLATE_PREFIX):
        slug = slug[len(CUSTOM_TEMPLATE_PREFIX):]
    if not CUSTOM_SLUG_RE.match(slug):
        raise MapError("模板文件名只能包含字母、数字、下划线、短横线，且不能以符号开头：%r" % raw)
    return CUSTOM_TEMPLATE_PREFIX + slug.lower()


def custom_template_path(name: str) -> Optional[Path]:
    """自定义模板 id → 磁盘路径；不是合法 id 就返回 None。"""
    if not CUSTOM_TEMPLATE_RE.match(name or ""):
        return None
    return CUSTOM_TEMPLATE_DIR / (name + ".json")


def _find_custom_template(name: str) -> Optional[Path]:
    """按 id 找自定义模板；Windows 文件名大小写不敏感，这里也做一次兼容。"""
    path = custom_template_path(name)
    if path and path.is_file():
        return path
    if not CUSTOM_TEMPLATE_DIR.is_dir():
        return None
    for item in CUSTOM_TEMPLATE_DIR.glob("*.json"):
        if item.stem.lower() == (name or "").lower() and CUSTOM_TEMPLATE_RE.match(item.stem):
            return item
    return None


def _template_label_from_file(path: Path, fallback: str) -> str:
    try:
        data = json.loads(path.read_text(encoding="utf-8").replace("\ufeff", ""))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        return fallback
    label = data.get("label")
    if isinstance(label, str) and label.strip():
        return label.strip()
    name = data.get("name")
    if (isinstance(name, str) and name.strip() and name.strip() != fallback
            and not name.strip().lower().startswith(CUSTOM_TEMPLATE_PREFIX)):
        return name.strip()
    return fallback


def _template_display_path(path: Path) -> str:
    try:
        return path.relative_to(TEMPLATE_ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def template_catalog() -> List[Dict[str, Any]]:
    """页面用的模板列表：内置模板 + templates/custom/*.json。"""
    items: List[Dict[str, Any]] = []
    for key, tpl in TEMPLATES.items():
        item: Dict[str, Any] = {
            "id": key,
            "label": tpl.get("label", key),
            "builtin": True,
            "loadable": True,
        }
        if tpl.get("file"):
            path = TEMPLATE_ROOT / tpl["file"]
            try:
                room = parse_json(path.read_text(encoding="utf-8"), key)
            except (OSError, MapError) as exc:
                item["loadable"] = False
                item["error"] = "模板文件读不到：%s" % exc
                item["size"] = None
            else:
                item["size"] = [room.cols, room.height]
        else:
            item["size"] = [tpl["cols"], tpl["rows"]]
        items.append(item)

    if CUSTOM_TEMPLATE_DIR.is_dir():
        for path in sorted(CUSTOM_TEMPLATE_DIR.glob("*.json"), key=lambda p: p.name.lower()):
            if not CUSTOM_TEMPLATE_RE.match(path.stem):
                continue
            item = {
                "id": path.stem,
                "label": path.stem[len(CUSTOM_TEMPLATE_PREFIX):],
                "builtin": False,
                "loadable": True,
            }
            try:
                room = parse_json(path.read_text(encoding="utf-8"), path.stem)
            except (OSError, MapError) as exc:
                item["loadable"] = False
                item["error"] = "自定义模板损坏：%s" % exc
                item["size"] = None
            else:
                item["size"] = [room.cols, room.height]
                item["label"] = _template_label_from_file(path, item["label"])
            items.append(item)
    return items


def template_label(name: str) -> str:
    """模板 id → 显示名；找不到返回空串。"""
    key = TEMPLATE_ALIASES.get(name, name)
    for builtin, tpl in TEMPLATES.items():
        if builtin.lower() == key.lower():
            return tpl.get("label", builtin)
    path = _find_custom_template(key)
    if path:
        return _template_label_from_file(path, path.stem[len(CUSTOM_TEMPLATE_PREFIX):])
    return ""


def save_custom_template(room: RoomMap, name: str, label: str = "") -> Dict[str, Any]:
    """把当前图存成自定义模板；同名覆盖，内置模板不受影响。"""
    template_id = custom_template_slug(name)
    label = (label or "").strip()
    if len(label) > 64:
        raise MapError("模板显示名最多 64 个字符")
    if not label:
        label = template_id[len(CUSTOM_TEMPLATE_PREFIX):]

    #  文件名用 id，显示名放 label；不要改调用方传进来的 room.name。
    saved = RoomMap(
        [row[:] for row in room.rows],
        [row[:] for row in room.rot],
        template_id,
        list(room.objects),
    )
    CUSTOM_TEMPLATE_DIR.mkdir(parents=True, exist_ok=True)
    path = CUSTOM_TEMPLATE_DIR / (template_id + ".json")
    existed = path.is_file()
    text = serialize_json(saved, label=label)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(text, encoding="utf-8", newline="\n")
    os.replace(tmp, path)
    return {
        "id": template_id,
        "label": label,
        "size": [saved.cols, saved.height],
        "file": _template_display_path(path),
        "existed": existed,
    }


def delete_custom_template(name: str) -> Dict[str, Any]:
    """删除自定义模板；内置模板明确拒绝。"""
    raw = (name or "").strip()
    for builtin in list(TEMPLATES) + list(TEMPLATE_ALIASES):
        if builtin.lower() == raw.lower():
            raise MapError("内置模板不能删除：%s" % builtin)
    if not raw.lower().startswith(CUSTOM_TEMPLATE_PREFIX):
        raise MapError("只能删除自定义模板（custom_ 开头）")
    template_id = custom_template_slug(raw)
    path = _find_custom_template(template_id)
    if not path:
        raise MapError("自定义模板不存在：%s" % template_id)
    path.unlink()
    return {"id": template_id, "file": _template_display_path(path)}


def make_template(key: str) -> RoomMap:
    """按【形状】取起手骨架：内置 1x1 / L / 2x2，或 templates/custom/ 下的自定义模板。"""
    key = TEMPLATE_ALIASES.get(key, key)
    if key not in TEMPLATES:                     # 名字大小写不敏感：template_l == template_L
        for name in TEMPLATES:
            if name.lower() == key.lower():
                key = name
                break
    tpl = TEMPLATES.get(key)
    if tpl:
        if tpl.get("file"):
            path = TEMPLATE_ROOT / tpl["file"]
            try:
                room = parse_json(path.read_text(encoding="utf-8"), key)
            except OSError as exc:
                raise MapError("模板文件读不到：%s" % exc) from None
            room.name = key
            return room
        return make_room(
            tpl["cols"], tpl["rows"], tpl["arena"],
            name=key,
            corner=tpl.get("corner", 0),
            band_rot=tpl.get("band_rot", (0, 0, 0, 0)),
            floor=tpl.get("floor", "B"),
        )

    path = _find_custom_template(key)
    if path:
        try:
            room = parse_json(path.read_text(encoding="utf-8"), path.stem)
        except OSError as exc:
            raise MapError("自定义模板读不到：%s" % exc) from None
        room.name = path.stem
        return room

    names = list(TEMPLATES)
    if CUSTOM_TEMPLATE_DIR.is_dir():
        names += [p.stem for p in sorted(CUSTOM_TEMPLATE_DIR.glob("*.json")) if CUSTOM_TEMPLATE_RE.match(p.stem)]
    raise MapError("未知模板 %r（可用：%s）" % (key, "、".join(names)))


# ---------------------------------------------------------------------------
#  标准矩形框架：参数 ⇄ 格子（和固件 game/game_frame.c 一一对应）
# ---------------------------------------------------------------------------

FRAME_DOOR_NONE = 0xFF
DOOR_ANGLE = (0, 1, 2, 3)          # N / E / S / W


def frame_tile(frame: Dict[str, Any], x: int, y: int) -> str:
    """框架参数 → 这一格的字符。'.' 黑边 / 'B' 地板 / 'W' 墙 / 'D' 门 / 'CXYZ' 角块。"""
    cols, rows = frame["cols"], frame["rows"]
    if not (0 <= x < cols and 0 <= y < rows):
        return "."
    ax0, ay0, ax1, ay1 = frame["arena"]
    doors = frame["doors"]

    for side, pos in enumerate(doors):
        if pos == FRAME_DOOR_NONE:
            continue
        dx, dy, px, py = _door_cells(frame, side)
        if (x, y) == (dx, dy):
            return frame.get("door_ch") or "D"
        if (x, y) == (px, py) and (frame.get("door_ch") or "D") != "H":
            return "."
    if ax0 <= x <= ax1 and ay0 <= y <= ay1:
        return frame.get("floor_ch") or "B"
    if not (ax0 - 2 <= x <= ax1 + 2 and ay0 - 2 <= y <= ay1 + 2):
        return "."
    if frame.get("corner"):
        ch = _corner_char(frame, x, y)
        if ch:
            return ch
    return frame.get("wall_ch") or "W"


def frame_rot(frame: Dict[str, Any], x: int, y: int) -> int:
    """框架参数 → 这一格的旋转（门的方向 / 墙木纹角度）。"""
    cols, rows = frame["cols"], frame["rows"]
    if not (0 <= x < cols and 0 <= y < rows):
        return 0
    ax0, ay0, ax1, ay1 = frame["arena"]

    for side, pos in enumerate(frame["doors"]):
        if pos == FRAME_DOOR_NONE:
            continue
        dx, dy, px, py = _door_cells(frame, side)
        if (x, y) == (dx, dy):
            return side
        if (x, y) == (px, py) and (frame.get("door_ch") or "D") != "H":
            return 0
    if ax0 <= x <= ax1 and ay0 <= y <= ay1:
        return 0
    if not (ax0 - 2 <= x <= ax1 + 2 and ay0 - 2 <= y <= ay1 + 2):
        return 0
    if frame.get("corner") and _corner_char(frame, x, y):
        return 0

    in_n = y <= ay0 - 1
    in_s = y >= ay1 + 1
    in_w = x <= ax0 - 1
    in_e = x >= ax1 + 1
    band = frame["band_rot"]
    if in_n and y == ay0 - 2:
        return band[0]
    if in_s and y == ay1 + 2:
        return band[2]
    if in_w:
        return band[3]
    if in_e:
        return band[1]
    if in_n:
        return band[0]
    return band[2]


def _door_cells(frame: Dict[str, Any], side: int) -> Tuple[int, int, int, int]:
    """门的 (门格, 第二格)。

    ★ 普通门 'D' 占两格；隐藏门 'H' 只占【靠竞技场那一格】（1 格），
      外侧那格是隐藏房墙 —— 所以 H 的门格总是在内圈，第二格是外圈（不画门洞）。
    """
    ax0, ay0, ax1, ay1 = frame["arena"]
    doors = frame["doors"]
    one = (frame.get("door_ch") == "H")
    if side == 0:
        dy = ay0 - 1 if one else ay0 - 2
        py = ay0 - 2 if one else ay0 - 1
        return (doors[0], dy, doors[0], py)
    if side == 1:
        return (ax1 + 1, doors[1], ax1 + 2, doors[1])
    if side == 2:
        return (doors[2], ay1 + 1, doors[2], ay1 + 2)
    dx = ax0 - 1 if one else ax0 - 2
    px = ax0 - 2 if one else ax0 - 1
    return (dx, doors[3], px, doors[3])


def _corner_char(frame: Dict[str, Any], x: int, y: int) -> Optional[str]:
    ax0, ay0, ax1, ay1 = frame["arena"]
    ox0, oy0, ox1, oy1 = ax0 - 2, ay0 - 2, ax1 + 2, ay1 + 2
    ix0, iy0, ix1, iy1 = ax0 - 1, ay0 - 1, ax1 + 1, ay1 + 1
    ch = None
    if y == oy0:
        ch = "X" if x == ox0 else ("C" if x == ox1 else None)
    elif y == oy1:
        ch = "Z" if x == ox0 else ("Y" if x == ox1 else None)
    elif y == iy0:
        ch = "X" if x == ix0 else ("C" if x == ix1 else None)
    elif y == iy1:
        ch = "Z" if x == ix0 else ("Y" if x == ix1 else None)
    return ch.lower() if ch and frame.get("corner_ch") == "c" else ch


#  地板/门的"等价字符"：几何一样，只是换了皮肤（见 game_frame.h 的 floor_ch/door_ch）。
#  ★ 判框架时先把它们折回默认字符，再逐格比；比完把实际字符记进 frame["floor_ch"]/["door_ch"]。
_FLOOR_EQUIV = {"K": "B", "S": "B"}
_WALL_EQUIV = {"w": "W"}
_CORNER_EQUIV = {"c": "C", "x": "X", "y": "Y", "z": "Z"}


def detect_frame(room: RoomMap) -> Optional[Dict[str, Any]]:
    """这张图是不是"标准矩形框架"？是就返回参数，否则返回 None（异形房，只能存表）。

    做法：从图里读出竞技场 / 门 / 角块 / 木纹角度，再按规则重建一遍，
    逐格和原图比对；只要有一格对不上就判为异形。

    ★ 地板/门允许"换皮肤"：K/S 当 B、H 当 D 参与比较，
      实际字符记在返回值的 floor_ch / door_ch 里（0 表示默认字符）。
      这样隐藏房这种"几何完全一样、只换贴图"的图就不用存 3 KB 地形表。
    """
    room = clean(room)
    ax0, ay0, ax1, ay1 = arena_rect(room)
    cols, height = room.cols, room.height

    floor_ch = ""
    wall_ch = ""
    door_ch = ""
    corner_ch = ""
    norm_rows: List[List[str]] = []
    for row in room.rows:
        nr: List[str] = []
        for ch in row:
            if ch in _FLOOR_EQUIV:
                floor_ch = ch
                nr.append(_FLOOR_EQUIV[ch])
            elif ch == "H":
                door_ch = ch          # 隐藏门只占 1 格，几何和普通门不同，不做字符归一
                nr.append(ch)
            elif ch in _WALL_EQUIV:
                wall_ch = ch
                nr.append(_WALL_EQUIV[ch])
            elif ch in _CORNER_EQUIV:
                corner_ch = "c"          # 只记家族；具体翻转由字符本身（c/x/y/z）决定
                nr.append(_CORNER_EQUIV[ch])
            else:
                nr.append(ch)
        norm_rows.append(nr)
    # Keep the original skin when comparing against frame_tile(). Normalized
    # characters are only used to infer the common geometry below.
    normalized = RoomMap(rows=norm_rows, rot=room.rot, name=room.name, objects=room.objects)

    doors = [FRAME_DOOR_NONE] * 4
    for y in range(height):
        for x in range(cols):
            if room.rows[y][x] not in DOOR_CHARS:
                continue
            sides = wall_sides(room, x, y)
            if len(sides) != 1:
                return None
            side = {"N": 0, "E": 1, "S": 2, "W": 3}[sides[0]]
            doors[side] = x if side in (0, 2) else y

    corner = 1 if any(ch in "CXYZ" for row in normalized.rows for ch in row) else 0
    band_rot = [0, 0, 0, 0]
    found = [False, False, False, False]
    for y in range(height):
        for x in range(cols):
            if normalized.rows[y][x] != "W":
                continue
            sides = wall_sides(room, x, y)
            if len(sides) != 1:
                continue
            side = {"N": 0, "E": 1, "S": 2, "W": 3}[sides[0]]
            if found[side]:
                continue
            band_rot[side] = int(room.rot[y][x]) & 3
            found[side] = True

    frame = {
        "cols": cols,
        "rows": height,
        "arena": [ax0, ay0, ax1, ay1],
        "doors": doors,
        "band_rot": band_rot,
        "corner": corner,
        "floor_ch": floor_ch,
        "wall_ch": wall_ch,
        "door_ch": door_ch,
        "corner_ch": corner_ch,
    }
    for y in range(height):
        for x in range(cols):
            if frame_tile(frame, x, y) != room.rows[y][x]:
                return None
            want_rot = int(room.rot[y][x]) & 3
            got_rot = frame_rot(frame, x, y)
            if got_rot != want_rot:
                #  ★ 地板的旋转没有意义（框架恒为 0），图里可能留着笔刷的角度；
                #    这里放过它，否则一张"只是地板换了皮肤"的图会被判成异形。
                if not (got_rot == 0 and room.rows[y][x] in ("B", "K", "S")):
                    return None
    return frame


def frame_world(frame: Dict[str, Any]) -> Tuple[int, int]:
    """框架对应的世界像素尺寸。

    ★ 40×23 格 = 1280×736，但最后一行只有 16 px（720p 窗口），所以高度按 720 算；
      其它尺寸（比如 45×35 = 1440×1120）原样使用。规则和固件 g_maps[] 一致。
    """
    width = frame["cols"] * TILE
    height = frame["rows"] * TILE
    if 0 < height - CANVAS_H < TILE:
        height = CANVAS_H
    return width, height


def frame_c(frame: Dict[str, Any], name: str = "") -> str:
    """框架参数 → 可以直接贴进 game_world.c 的 g_maps[] 的一行。"""
    doors = ", ".join(
        "FRAME_DOOR_NONE" if pos == FRAME_DOOR_NONE else str(pos) for pos in frame["doors"]
    )
    band = ", ".join(str(v) for v in frame["band_rot"])
    world_w, world_h = frame_world(frame)
    comment = "  /* %s */" % name if name else ""
    skins = []
    for key in ("floor_ch", "wall_ch", "door_ch", "corner_ch"):
        ch = frame.get(key) or ""
        skins.append("'%s'" % ch if ch else "0")
    skin = ""
    if any(frame.get(key) for key in ("floor_ch", "wall_ch", "door_ch", "corner_ch")):
        skin = ", " + ", ".join(skins)
    return (
        "    { 0, 0,\n"
        "      { %d, %d, %d, %d, %d, %d, { %s }, { %s }, %d%s },\n"
        "      %d, %d },%s"
        % (
            frame["cols"], frame["rows"],
            frame["arena"][0], frame["arena"][1], frame["arena"][2], frame["arena"][3],
            doors, band, frame["corner"], skin,
            frame.get("world_w", world_w),
            frame.get("world_h", world_h),
            comment,
        )
    )


# ---------------------------------------------------------------------------
#  布局数据导出（给固件生成 game_layout_data.c/.h）
# ---------------------------------------------------------------------------

LAYOUT_OBJ_KINDS = {
    "slime": "OBJ_SLIME",
    "bug": "OBJ_BUG",
    "web": "OBJ_WEB",
    "stone": "OBJ_STONE",
    "box": "OBJ_BOX",
    "crate": "OBJ_CRATE",
}
LAYOUT_MAX_OBJS = 200          # room_layout_t.n_objs 是 unsigned char，留余量
POOL_MAP_IDS = ("1x1", "L", "2x2", "hidden")
NORMAL_MAP_IDS = ("1x1", "L", "2x2")
POOL_MAP_DIR = TEMPLATE_ROOT / "maps"


def pool_map(map_id: str) -> RoomMap:
    """固定空间由地图池持有；布局文件只引用 ID，L 永远是左下空缺。"""
    if map_id not in POOL_MAP_IDS:
        raise MapError("未知地图池房型 %r（可用：%s）" % (map_id, "、".join(POOL_MAP_IDS)))
    room = parse_json((POOL_MAP_DIR / (map_id + ".json")).read_text(encoding="utf-8"))
    room.objects = []
    return room


def geometry_key(room: RoomMap) -> Tuple:
    return (tuple("".join(row) for row in room.rows),
            tuple(tuple(row) for row in room.rot))


def pool_map_id(room: RoomMap) -> str:
    key = geometry_key(room)
    for map_id in POOL_MAP_IDS:
        if key == geometry_key(pool_map(map_id)):
            return map_id
    raise MapError("房间空间与固定地图池不一致；请载入地图池模板后只编辑对象。L 型只接受左下空缺")


def pool_layout_name(map_id: str, layout_id: int) -> str:
    if map_id not in NORMAL_MAP_IDS:
        raise MapError("本轮只支持普通房型：1x1、L、2x2；特殊房暂不参与")
    if type(layout_id) is not int or not 0 <= layout_id <= 999:
        raise MapError("布局编号必须是 0..999 的整数")
    if layout_id == 0 and map_id != "1x1":
        raise MapError("只有 1x1 可以使用保留起点编号 000")
    return "%s_%03d.json" % (map_id, layout_id)


def serialize_pool_layout(room: RoomMap) -> str:
    """可被设计器回读的紧凑布局：不重复存墙、地板、门和旋转表。"""
    map_id = pool_map_id(room)
    issues = errors_of(validate(room))
    if issues:
        raise MapError("布局没通过校验：%s" % "；".join(i.message for i in issues[:3]))
    unsupported = sorted({o.kind for o in room.objects if o.kind not in LAYOUT_OBJ_KINDS})
    if unsupported:
        raise MapError("布局池尚不支持这些对象，不能静默丢弃：%s" % "、".join(unsupported))
    if len(room.objects) > LAYOUT_MAX_OBJS:
        raise MapError("布局对象超过上限 %d" % LAYOUT_MAX_OBJS)
    if room.layout_id == 0 and room.objects:
        raise MapError("1x1_000 是起点空布局，不能包含对象")
    data = {
        "format": "map_designer_layout", "version": 1, "name": room.name,
        "map": map_id,
        "objects": [{"kind": o.kind, "x": o.x, "y": o.y, "arg": o.arg}
                    for o in sorted(room.objects, key=lambda o: (o.y, o.x, o.kind))],
    }
    if room.layout_id is not None:
        canonical = pool_layout_name(map_id, room.layout_id)
        data["layout_id"] = room.layout_id
        data["name"] = Path(canonical).stem
    return json.dumps(data, ensure_ascii=False, indent=2) + "\n"


def layout_symbol(name: str) -> str:
    """布局名 → C 标识符（字母数字下划线，不以数字开头）。"""
    sym = re.sub(r"[^0-9A-Za-z_]", "_", name.strip())
    if not sym:
        sym = "layout"
    if sym[0].isdigit():
        sym = "_" + sym
    return sym


_FRAME_MACROS = {
    "MAP_COLS": "40", "MAP_ROWS": "23",
    "BOSS_MAP_COLS": "45", "BOSS_MAP_ROWS": "35",
    "SCR_W": "1280", "SCR_H": "720", "TILE": "32",
}

_WORLD_FRAME_RE = re.compile(
    r"\{\s*0\s*,\s*0\s*,\s*\{\s*"
    r"(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,"
    r"\s*\{\s*([^}]*?)\s*\}\s*,\s*\{\s*([^}]*?)\s*\}\s*,\s*(\d+)\s*"
    r"(?:,\s*([^}]*?))?"            # 可选尾巴：floor/wall/door 皮肤字符（'K', 0, 'H'）
    r"\s*\}\s*,"
    r"\s*([^,}]+?)\s*,\s*([^,}]+?)\s*\}",
    re.S,
)


def _c_int(expr: str):
    value = 1
    for part in str(expr).split("*"):
        part = _FRAME_MACROS.get(part.strip(), part.strip())
        try:
            value *= int(part)
        except ValueError:
            return None
    return value


def parse_world_frames(world_src: str) -> List[Dict[str, Any]]:
    """从 game_world.c 的 g_maps[] 读框架表。

    这是"地图尺寸 → 地图下标"的唯一真源：固件里加一行框架，
    设计器导出布局时就自动认得这个尺寸。
    """
    start = world_src.find("g_maps[]")
    if start < 0:
        raise MapError("game_world.c 里找不到 g_maps[]")
    block = world_src[start:world_src.find("};", start)]
    # Export against the release table, excluding the optional Boss test frame.
    block = re.sub(r"#if\s+GAME_BOSS_1X1_TEST\b.*?#endif", "", block, flags=re.S)
    block = block.replace("FRAME_DOOR_NONE", "255")
    for macro in sorted(_FRAME_MACROS, key=len, reverse=True):
        block = re.sub(r"\b%s\b" % macro, _FRAME_MACROS[macro], block)

    frames: List[Dict[str, Any]] = []
    for match in _WORLD_FRAME_RE.finditer(block):
        cols, rows, ax0, ay0, ax1, ay1 = (int(match.group(i)) for i in range(1, 7))
        doors = [int(v) for v in re.findall(r"\d+", match.group(7))]
        band = [int(v) for v in re.findall(r"\d+", match.group(8))]
        if len(doors) != 4 or len(band) != 4:
            continue
        tail = (match.group(10) or "").strip()
        fields = [f.strip() for f in tail.split(",")] if tail else []

        def _skin(i: int) -> str:
            """第 i 个皮肤字段：是带引号的字符就用它，否则空 = 用默认（'B'/'W'/'D'）。"""
            if i >= len(fields):
                return ""
            m = re.match(r"^'(.)'$", fields[i])
            return m.group(1) if m else ""

        frames.append({
            "cols": cols, "rows": rows,
            "arena": [ax0, ay0, ax1, ay1],
            "doors": doors, "band_rot": band,
            "corner": int(match.group(9)),
            "floor_ch": _skin(0),
            "wall_ch":  _skin(1),
            "door_ch":  _skin(2),
            "corner_ch": _skin(3),
            "world_w": _c_int(match.group(11)) or cols * TILE,
            "world_h": _c_int(match.group(12)) or rows * TILE,
        })
    if not frames:
        raise MapError("g_maps[] 里没解析到框架")
    return frames


def layout_map_index(room: RoomMap, frames: Optional[Sequence[Dict[str, Any]]] = None) -> int:
    """找到完整匹配的 g_maps[] 框架（frames 由 parse_world_frames 提供）。

    地形、门位和旋转必须一致；不匹配时由 emit_layout_data 保存逐格地形。
    """
    det = detect_frame(room)
    if det is None:
        raise MapError("不是标准矩形框架（墙/地板有额外改动），暂不支持自动导出")

    #  ★ 尺寸相同还不够：地板/门的"皮肤"也要一致（隐藏房和普通房都是 40×23，
    #    但一个用 K/H、一个用 B/D，必须落到 g_maps[] 里不同的两行）。
    want_floor = det.get("floor_ch") or "B"
    want_wall = det.get("wall_ch") or "W"
    want_door = det.get("door_ch") or "D"
    want_corner = det.get("corner_ch") or "C"

    table = list(frames) if frames else [
        {"cols": DEFAULT_COLS, "rows": DEFAULT_ROWS},
        {"cols": MAX_COLS, "rows": MAX_ROWS},
    ]
    for index, frame in enumerate(table):
        if (room.cols, room.height) != (int(frame["cols"]), int(frame["rows"])):
            continue
        if (frame.get("floor_ch") or "B") != want_floor:
            continue
        if (frame.get("wall_ch") or "W") != want_wall:
            continue
        if (frame.get("door_ch") or "D") != want_door:
            continue
        if (frame.get("corner_ch") or "C") != want_corner:
            continue
        # A canvas/skin match must never discard different doors, corners or
        # rotations. Compact frames are safe only when every source cell agrees.
        if "arena" in frame and any(
            room.rows[y][x] != frame_tile(frame, x, y) or
            (int(room.rot[y][x]) & 3) != frame_rot(frame, x, y)
            for y in range(room.height) for x in range(room.cols)
        ):
            continue
        return index
    sizes = "、".join("%d×%d%s%s%s" % (f["cols"], f["rows"],
                                       " 地板" + f["floor_ch"] if f.get("floor_ch") else "",
                                       " 墙" + f["wall_ch"] if f.get("wall_ch") else "",
                                       " 门" + f["door_ch"] if f.get("door_ch") else "")
                      for f in table)
    raise MapError(
        "画布 %d×%d（地板 %s / 墙 %s / 门 %s / 角块 %s）在固件框架表里没有对应项（现有：%s）。"
        "先在 game_world.c 的 g_maps[] 加一行这个尺寸+皮肤的框架"
        % (room.cols, room.height, want_floor, want_wall, want_door, want_corner, sizes)
    )


def emit_layout_data(
    items: Sequence[Tuple[str, RoomMap]],
    frames: Optional[Sequence[Dict[str, Any]]] = None,
) -> Tuple[str, str, List[str]]:
    """把 [(名字, 地图)] 生成 (header, source, warnings)。

    · 标准矩形房 -> 只出对象表，地图下标指向 g_maps[]；
    · 其余房间（含八门 2×2）-> 额外出逐格地形表（tiles + rot），布局自带地形；
    · 输出稳定：按名字排序、对象按 (y, x) 排序；
    · 有一张图不合法就抛 MapError（整批不生成）；
    · exit 这类固件不认的对象跳过并记 warning。
    """
    rows: List[Tuple[str, str, RoomMap, int, List[MapObject], Optional[Dict[str, Any]]]] = []
    warnings: List[str] = []
    used = set()

    for name, room in sorted(items, key=lambda item: item[0]):
        sym = layout_symbol(name)
        if sym in used:
            raise MapError("布局名 %r 重名了（生成出来的符号都是 %s）" % (name, sym))
        used.add(sym)

        issues = errors_of(validate(room))
        if issues:
            raise MapError(
                "布局 %r 没通过校验：%s" % (name, "；".join(i.message for i in issues[:3]))
            )

        objs: List[MapObject] = []
        for obj in room.objects:
            if obj.kind not in LAYOUT_OBJ_KINDS:
                warnings.append("布局 %r：跳过 %r（固件没有这种对象）" % (name, obj.kind))
                continue
            objs.append(obj)
        objs.sort(key=lambda o: (o.y, o.x, OBJECT_KINDS[o.kind]["id"]))
        if len(objs) > LAYOUT_MAX_OBJS:
            raise MapError(
                "布局 %r 有 %d 个对象，超过上限 %d" % (name, len(objs), LAYOUT_MAX_OBJS)
            )

        terrain: Optional[Dict[str, Any]] = None
        try:
            map_idx = layout_map_index(room, frames)
        except MapError:
            sizes = {(int(f["cols"]), int(f["rows"])) for f in frames} if frames else {
                (DEFAULT_COLS, DEFAULT_ROWS), (MAX_COLS, MAX_ROWS)}
            if detect_frame(room) is not None and (room.cols, room.height) not in sizes:
                raise
            # Fixed pool names identify the authored map, not an older frame
            # with the same dimensions. Preserve all cells and rotations.
            ax0, ay0, ax1, ay1 = arena_rect(room)
            world_w = room.cols * TILE
            world_h = room.height * TILE
            if 0 < world_h - CANVAS_H < TILE:
                world_h = CANVAS_H
            terrain = {
                "cols": room.cols,
                "rows": room.height,
                "arena": [ax0, ay0, ax1, ay1],
                "world": [world_w, world_h],
                "tiles": ["".join(row) for row in room.rows],
                "rot": [[int(v) & 3 for v in row] for row in room.rot],
            }
            map_idx = 0
            warnings.append("布局 %r 使用逐格地形表（未匹配完整固件框架）" % name)

        rows.append((name, sym, room, map_idx, objs, terrain))

    if not rows:
        raise MapError("一张布局都没有")
    if len(rows) > 127:
        raise MapError("布局超过 127 张（运行时布局编号使用 signed char）")

    # 同一固定空间只存一份逐格地形；对象方案共享地图身份。
    terrain_owners = {}
    terrain_refs = {}
    for index, (name, sym, room, map_idx, objs, terrain) in enumerate(rows):
        if terrain:
            owner, map_id = terrain_owners.setdefault(geometry_key(room), (sym, index))
            terrain_refs[sym] = owner
            rows[index] = (name, sym, room, map_id, objs, terrain)

    # Share identical rows across authored terrains, retaining O(1) cell lookup.
    # Rotation keys include zero padding to match the fixed C row stride.
    tile_rows: List[str] = []
    rotation_rows: List[Tuple[int, ...]] = []
    tile_ids = {}
    rotation_ids = {}
    terrain_row_ids = {}
    for name, sym, room, map_idx, objs, terrain in rows:
        if not terrain or terrain_refs[sym] != sym:
            continue
        ti, ri = [], []
        for line, rotations in zip(terrain["tiles"], terrain["rot"]):
            if line not in tile_ids:
                tile_ids[line] = len(tile_rows)
                tile_rows.append(line)
            ti.append(tile_ids[line])
            packed = tuple(sum(v << (2 * k) for k, v in enumerate(rotations[x:x+4]))
                           for x in range(0, room.cols, 4))
            packed += (0,) * (((MAX_COLS + 3) // 4) - len(packed))
            if packed not in rotation_ids:
                rotation_ids[packed] = len(rotation_rows)
                rotation_rows.append(packed)
            ri.append(rotation_ids[packed])
        terrain_row_ids[sym] = (ti, ri)

    digest = hashlib.sha256()
    for name, sym, room, map_idx, objs, terrain in rows:
        digest.update(("%s|%d|%d|%d|%d|" % (name, room.cols, room.height, 1 if terrain else 0, map_idx)).encode("utf-8"))
        digest.update(("id=%d|" % (room.layout_id or 0)).encode("utf-8"))
        for obj in objs:
            digest.update(("%s,%d,%d,%d;" % (obj.kind, obj.x, obj.y, obj.arg)).encode("utf-8"))
        if terrain:
            digest.update("".join(terrain["tiles"]).encode("utf-8"))
            digest.update(str(terrain["rot"]).encode("utf-8"))
    sha = digest.hexdigest()

    header: List[str] = []
    header.append("/* 自动生成，别手改 —— 源：tools/map_designer/output/<房间名>.json")
    header.append(" * 生成：python tools/map_designer/serve.py --emit-layouts --apply")
    header.append(" * sha256:%s" % sha)
    header.append(" *")
    for name, sym, room, map_idx, objs, terrain in rows:
        kind = ("共享地形=%d" % map_idx) if terrain else ("map=%d" % map_idx)
        header.append(" *  %-20s %d×%d  %-14s %d 个对象"
                      % (name, room.cols, room.height, kind, len(objs)))
    header.append(" */")
    header.append("#ifndef GAME_LAYOUT_DATA_H")
    header.append("#define GAME_LAYOUT_DATA_H")
    header.append("")
    header.append('#include "game_layout.h"')
    header.append("")
    for index, (name, sym, room, map_idx, objs, terrain) in enumerate(rows):
        header.append("#define LAY_%-24s %d" % (sym, index))
    header.append("")
    header.append("#define LAYOUT_COUNT %d" % len(rows))
    header.append("#define LAYOUT_SHARED_MAPS 1")
    header.append("")
    header.append("extern const room_layout_t g_layouts[LAYOUT_COUNT];")
    header.append("extern const unsigned short g_layout_ids[LAYOUT_COUNT]; /* 0 = 保留布局/旧参考布局 */")
    for name, sym, room, map_idx, objs, terrain in rows:
        header.append("extern const obj_place_t L_%s[];" % sym)
        if terrain:
            if terrain_refs[sym] == sym:
                header.append("extern const layout_terrain_t T_%s;" % sym)
            else:
                header.append("#define T_%s T_%s" % (sym, terrain_refs[sym]))
    header.append("")
    header.append("#endif /* GAME_LAYOUT_DATA_H */")

    source: List[str] = []
    source.append("/* 自动生成，别手改 —— 见 game_layout_data.h 的说明 */")
    source.append('#include "game.h"')
    source.append('#include "game_layout_data.h"')
    source.append("")
    source.append("#define O(x, y, k) { (unsigned char)(x), (unsigned char)(y), (unsigned char)(k), 0 }")
    source.append("#define OP(x, y, k, p) { (unsigned char)(x), (unsigned char)(y), (unsigned char)(k), (unsigned char)(p) }")
    source.append("")

    if tile_rows:
        source.append("/* Shared rows; per-terrain uint16 indexes preserve every cell/rotation. */")
        source.append("static const char LT_rows[%d][LAYOUT_MAX_COLS + 1] = {" % len(tile_rows))
        source.extend('    "%s",' % line for line in tile_rows)
        source.append("};")
        source.append("static const unsigned char RT_rows[%d][LAYOUT_ROT_COLS] = {" % len(rotation_rows))
        source.extend("    { %s }," % ", ".join(str(v) for v in line) for line in rotation_rows)
        source.append("};")
        source.append("")

    for name, sym, room, map_idx, objs, terrain in rows:
        if terrain and terrain_refs[sym] == sym:
            source.append("/* %s：%d×%d 异形地形 */" % (name, room.cols, room.height))
            ti, ri = terrain_row_ids[sym]
            source.append("static const unsigned short LTI_%s[%d] = { %s };" %
                          (sym, room.height, ", ".join(str(v) for v in ti)))
            source.append("static const unsigned short RTI_%s[%d] = { %s };" %
                          (sym, room.height, ", ".join(str(v) for v in ri)))
            source.append("const layout_terrain_t T_%s = {" % sym)
            occupied = [(x, y) for y, row in enumerate(room.rows)
                        for x, ch in enumerate(row) if ch != "."]
            mirror_x = min(x for x, y in occupied) + max(x for x, y in occupied)
            mirror_y = min(y for x, y in occupied) + max(y for x, y in occupied)
            source.append("    LT_rows, RT_rows, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, LTI_%s, RTI_%s" % (
                terrain["cols"], terrain["rows"],
                terrain["arena"][0], terrain["arena"][1], terrain["arena"][2], terrain["arena"][3],
                terrain["world"][0], terrain["world"][1], mirror_x, mirror_y, sym, sym))
            source.append("};")
            source.append("")

        source.append("/* %s：%d×%d，%d 个对象 */" % (name, room.cols, room.height, len(objs)))
        source.append("const obj_place_t L_%s[] = {" % sym)
        if not objs:
            source.append("    { 0, 0, 0, 0 },   /* 占位：空布局，n = 0 */")
        else:
            line = "   "
            for obj in objs:
                piece = " O(%d,%d,%s)," % (obj.x, obj.y, LAYOUT_OBJ_KINDS[obj.kind])
                if obj.arg:
                    piece = " OP(%d,%d,%s,%d)," % (obj.x, obj.y, LAYOUT_OBJ_KINDS[obj.kind], obj.arg)
                if len(line) + len(piece) > 96:
                    source.append(line)
                    line = "   "
                line += piece
            if line.strip():
                source.append(line)
        source.append("};")
        source.append("")

    source.append("const room_layout_t g_layouts[LAYOUT_COUNT] = {")
    for name, sym, room, map_idx, objs, terrain in rows:
        tail = "&T_%s" % terrain_refs[sym] if terrain else "0"
        source.append('    { "%s", L_%s, %d, %d, %s },' % (name, sym, len(objs), map_idx, tail))
    source.append("};")
    source.append("const unsigned short g_layout_ids[LAYOUT_COUNT] = { %s };" %
                  ", ".join(str(room.layout_id or 0) for name, sym, room, map_idx, objs, terrain in rows))
    source.append("")
    return "\n".join(header) + "\n", "\n".join(source) + "\n", warnings

# ---------------------------------------------------------------------------
#  校验
# ---------------------------------------------------------------------------

def validate(room: RoomMap) -> List[Issue]:
    issues: List[Issue] = []

    def add(level: str, message: str, x: int = -1, y: int = -1) -> None:
        issues.append(Issue(level, message, x, y))

    # ---- 尺寸 / 字符 ------------------------------------------------------
    cols, height = room.cols, room.height
    if not cols or not height:
        add("error", "地图是空的：至少要有 %d 行 %d 列" % (MIN_ROWS, MIN_COLS))
        return issues
    if not (MIN_COLS <= cols <= MAX_COLS and MIN_ROWS <= height <= MAX_ROWS):
        add(
            "error",
            "画布 %d×%d 超出范围 %d×%d ~ %d×%d"
            % (cols, height, MIN_COLS, MIN_ROWS, MAX_COLS, MAX_ROWS),
        )
        return issues

    for y, row in enumerate(room.rows):
        if len(row) != cols:
            add("error", "第 %d 行长度 %d，需要 %d" % (y + 1, len(row), cols), 0, y)
            return issues
        for x, char in enumerate(row):
            if char not in ALLOWED:
                add("error", "非法字符 %r" % char, x, y)
            spin = room.rot[y][x] if y < len(room.rot) and x < len(room.rot[y]) else 0
            if spin not in (0, 1, 2, 3):
                add("error", "旋转值 %r 不在 0..3" % spin, x, y)

    # ---- 竞技场 ----------------------------------------------------------
    if not any(("B" in row or "K" in row or "S" in row) for row in room.rows):
        add("error", "地图里没有地板 B / K / S —— 竞技场是从地板包围盒推出来的")
        return issues
    ax0, ay0, ax1, ay1 = arena_rect(room)
    aw, ah = ax1 - ax0 + 1, ay1 - ay0 + 1

    #  异形房判定：竞技场包围盒里出现非 B 的格子（L 形的缺口）。
    #  这种房间不套"矩形墙带"规则，门只做基本检查（见门的检查那一节）。
    #  门格 D 本身不是“异形缺口”，否则在矩形竞技场里误放一扇门就会被当成 L 形。
    irregular = any(room.rows[y][x] not in ("B", "K", "S", "D", "H")
                    for y in range(ay0, ay1 + 1) for x in range(ax0, ax1 + 1))
    if irregular:
        #  L 形 / 凹形房间：包围盒里本来就会有墙或空白，不能按"矩形竞技场"逐格报警。
        add("warning", "异形房间：竞技场不是矩形，按 B 的实际区域检查；L 形缺口按墙/空处理")
    if aw < 8 or ah < 8:
        add(
            "warning",
            "竞技场只有 %d×%d 格（普通房 23×13、Boss 房 29×21），确认一下 B 画全了" % (aw, ah),
        )

    # ---- 区域 ------------------------------------------------------------
    door_halves = set()
    for y in range(height):
        for x in range(cols):
            if room.rows[y][x] in DOOR_CHARS and room.rows[y][x] != "H":
                door_halves.add(door_pair(x, y, room.rot[y][x] & 3))

    for y in range(height):
        for x in range(cols):
            char = room.rows[y][x]
            if char == ".":
                spin = room.rot[y][x] if x < len(room.rot[y]) else 0
                if spin and (x, y) not in door_halves:
                    add("warning", "空白格 (%d,%d) 的旋转没有意义（保存时会保留，C 侧会归零）" % (x, y), x, y)
                continue
            if in_arena(room, x, y):
                if char not in FLOOR_CHARS and not irregular:
                    add("warning", "竞技场内的 (%d,%d) 是 %r，会挡住玩家" % (x, y, char), x, y)
                continue
            if in_wall_band(room, x, y):
                if char in FLOOR_CHARS and not irregular:
                    add("warning", "墙带里的 (%d,%d) 是地板" % (x, y), x, y)
                continue
            if not irregular:
                add("warning", "房间外的 (%d,%d) 画了 %r，上板看不到它" % (x, y, char), x, y)

    # ---- 门 --------------------------------------------------------------
    doors: List[Tuple[int, int, int]] = []
    pairs: Dict[Tuple[int, int], Tuple[int, int]] = {}
    for y in range(height):
        for x in range(cols):
            if room.rows[y][x] not in DOOR_CHARS:
                continue
            angle = room.rot[y][x] & 3
            sides = wall_sides(room, x, y)
            if not sides:
                if irregular:
                    doors.append((x, y, angle))
                    continue
                add("error", "门 (%d,%d) 不在 2 格厚的墙带上" % (x, y), x, y)
                continue
            if len(sides) > 1:
                if irregular:
                    doors.append((x, y, angle))
                    continue
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

            if room.rows[y][x] == "H":
                # A single-cell entrance must touch floor on its inward side.
                # Leaving H in the outer D anchor would hide it behind a wall.
                inward_x = x + (1 if angle == 3 else -1 if angle == 1 else 0)
                inward_y = y + (1 if angle == 0 else -1 if angle == 2 else 0)
                if (not (0 <= inward_x < cols and 0 <= inward_y < height)
                        or room.rows[inward_y][inward_x] not in FLOOR_CHARS):
                    add("error", "隐藏门 (%d,%d) 只占一格，必须紧贴内侧地板" % (x, y), x, y)
                doors.append((x, y, angle))
                continue

            px, py = door_pair(x, y, angle)
            if not (0 <= px < cols and 0 <= py < height):
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
    #  异形房的"墙带"不是矩形环，按包围盒算会大量误报；L 形 / 凹形直接跳过这一项。
    if not irregular:
        for y in range(height):
            for x in range(cols):
                if room.rows[y][x] != "." or not in_wall_band(room, x, y):
                    continue
                if (x, y) not in pairs:
                    add("warning", "墙带里的 (%d,%d) 是空白，会漏出一个洞" % (x, y), x, y)

    # ---- 门数量 ----------------------------------------------------------
    #  异形房的门数可以不是 4（L 形可能有多个出口），不做数量约束。
    if not irregular and len(doors) != 4:
        add("warning", "当前有 %d 个门格，通常房间是 4 扇门（N/E/S/W 各一）" % len(doors))

    # ---- 门洞内侧必须是地板 ----------------------------------------------
    #  ★ 门洞 = D 格 + 朝房间内那一格；再往里必须是 B/K。
    #    墙带是 2 格厚的，开门时只挖外层、内层忘了挖，人进门就被卡住，
    #    而且从房间里也走不回来（2026-10-24 在 L 房顶部抓到两处）。
    for (dx_, dy_, angle) in doors:
        ax = 0 if angle in (0, 2) else (1 if angle == 3 else -1)
        ay = 1 if angle == 0 else (-1 if angle == 2 else 0)
        blocked = None
        for k in range(1, 4):
            tx_, ty_ = dx_ + ax * k, dy_ + ay * k
            if not (0 <= tx_ < cols and 0 <= ty_ < height):
                break
            ch = room.rows[ty_][tx_]
            if ch == ".":
                continue
            if ch not in FLOOR_CHARS:
                blocked = (tx_, ty_)
            break
        if blocked is not None:
            bx, by = blocked
            add("error", "门 (%d,%d) 门洞内侧 (%d,%d) 是 %r，走不进去（墙没挖穿）"
                % (dx_, dy_, bx, by, room.rows[by][bx]), dx_, dy_)

    # ---- 对象层 ----------------------------------------------------------
    seen_objects = set()
    cell_blockers: Dict[Tuple[int, int], str] = {}
    counts = {"mob": 0, "web": 0, "blocker": 0, "exit": 0}
    for obj in room.objects:
        info = OBJECT_KINDS.get(obj.kind)
        if info is None:
            add("error", "未知对象类型 %r" % obj.kind, obj.x, obj.y)
            continue
        if not (0 <= obj.x < cols and 0 <= obj.y < height):
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

        if not in_arena(room, obj.x, obj.y):
            add("error", "%s (%d,%d) 在竞技场外，C 侧不会生成" % (info["label"], obj.x, obj.y), obj.x, obj.y)
            continue
        if room.rows[obj.y][obj.x] not in FLOOR_CHARS:
            add(
                "error",
                "%s (%d,%d) 必须放在地板 B/K/S 上，当前是 %r"
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
    #  网 / 石头在固件里已经没有实体池了（位置直接查布局），这里只做个"别太过分"的提示。
    if counts["web"] > OBJECT_LIMITS["web"]:
        add("warning", "蜘蛛网 %d 张，有点多（建议 ≤%d，叠太密会把可走区域堵死）" % (counts["web"], OBJECT_LIMITS["web"]))
    if counts["blocker"] > OBJECT_LIMITS["blocker"]:
        add("warning", "阻挡物 %d 个，有点多（建议 ≤%d，先确认门与门之间还走得通）" % (counts["blocker"], OBJECT_LIMITS["blocker"]))
    if counts["exit"] > 1:
        add("warning", "放了 %d 个出口，通常一个房间只有 1 个" % counts["exit"])

    return issues


def errors_of(issues: Iterable[Issue]) -> List[Issue]:
    return [issue for issue in issues if issue.level == "error"]


def stats(room: RoomMap) -> Dict[str, Any]:
    cols, height = room.cols, room.height
    counts = {char: 0 for char in TILE_INDEX}
    doors: List[Dict[str, Any]] = []
    for y in range(height):
        for x in range(cols):
            char = room.rows[y][x]
            if char in counts:
                counts[char] += 1
            if char in DOOR_CHARS:
                angle = room.rot[y][x] & 3
                doors.append({"x": x, "y": y, "dir": DIR_NAME[angle], "angle": angle, "hidden": char == "H"})
    object_counts = {kind: 0 for kind in OBJECT_KINDS}
    for obj in room.objects:
        object_counts[obj.kind] += 1
    return {
        "counts": counts,
        "doors": doors,
        "objects": [obj.as_dict() for obj in room.objects],
        "object_counts": object_counts,
        "name": room.name,
        "size": [cols, height],
        "arena": list(arena_rect(room)),
        "door_slots": [list(slot) for slot in door_slots(room)],
        "sha256": digest_of(room),
    }


# ---------------------------------------------------------------------------
#  C 代码生成 / 解析
# ---------------------------------------------------------------------------

def emit_c(room: RoomMap, source: str = "tools/map_designer/output/room_map.txt") -> str:
    """生成独立的数据文件（static 数组，可单独编译，也可被 include）。"""
    room = clean(room)
    cols, height = room.cols, room.height
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

    lines: List[str] = []
    lines.append("/* " + "=" * 74)
    lines.append(" *  game_world_map.c —— 房间地图数据（自动生成，别手改）")
    lines.append(" *  " + "-" * 73)
    lines.append(" *  生成器 : tools/map_designer/serve.py --emit-c")
    lines.append(" *  源文件 : %s" % source)
    lines.append(" *  指纹   : sha256:%s" % digest)
    lines.append(" *  名称   : %s（所有房间共用同一个 map 池，图不绑定房间类型）" % room.name)
    lines.append(
        " *  画布   : %d×%d 格 = %d×%d px；竞技场 %d,%d → %d,%d（格）"
        % (
            cols, height, cols * TILE, height * TILE,
            info["arena"][0], info["arena"][1], info["arena"][2], info["arena"][3],
        )
    )
    lines.append(
        " *  统计   : 门 %d · 墙 %d · 地板 %d · 角块 %d"
        % (
            counts["D"] + counts["H"],
            counts["W"] + counts["w"],
            counts["B"] + counts["K"] + counts["S"],
            sum(counts[ch] for ch in "CXYZcxyz"),
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
    lines.append("#define MAP_COLS %d" % cols)
    lines.append("#endif")
    lines.append("#ifndef MAP_ROWS")
    lines.append("#define MAP_ROWS %d" % height)
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
    lines.append('#define MAP_NAME "%s"' % room.name)
    ax0, ay0, ax1, ay1 = info["arena"]
    lines.append("/* 竞技场：地板 B 的包围盒（格坐标），相机和刷怪都按它算 */")
    lines.append("#define MAP_ARENA_X0 %d" % ax0)
    lines.append("#define MAP_ARENA_Y0 %d" % ay0)
    lines.append("#define MAP_ARENA_X1 %d" % ax1)
    lines.append("#define MAP_ARENA_Y1 %d" % ay1)
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

    literals = [
        literal
        for literal in re.findall(r'"([^"\\]*)"', map_block)
        if literal and set(literal) <= ALLOWED
    ]
    if not literals:
        raise MapError('C 里没找到地图行（room_map_shared 里没有 "...." 这样的字符串）')
    cols = len(literals[0])
    grid: List[List[str]] = [list(literal) for literal in literals if len(literal) == cols]
    if len(grid) != len(literals):
        raise MapError("C 里地图各行长度不一致（第一行 %d 列）" % cols)
    height = len(grid)
    check_size(cols, height)

    rot: List[List[int]] = []
    for match in re.finditer(r"\{([^{}]*)\}", rot_block):
        body = match.group(1)
        if "[" in body:
            # 指定初始化：[ 17] = { [0] = 3, [37] = 1 }（game_world.c 里的 Boss 地图）
            values = [0] * cols
            for index_s, value_s in re.findall(r"\[\s*(\d+)\s*\]\s*=\s*(\d+)", body):
                index = int(index_s)
                if 0 <= index < cols:
                    values[index] = int(value_s) & 3
            rot.append(values)
            continue
        values = [int(number) for number in re.findall(r"\d+", body)]
        if len(values) == cols:
            rot.append([value & 3 for value in values])
    if len(rot) != height:
        flat = [int(number) for number in re.findall(r"\d+", rot_block)]
        if len(flat) == height * cols:
            rot = [[flat[y * cols + x] & 3 for x in range(cols)] for y in range(height)]
        else:
            raise MapError("C 里解析到 %d 行旋转表，需要 %d 行" % (len(rot), height))

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

    name_match = re.search(r'#define\s+MAP_NAME\s+"([^"]+)"', source)
    if name_match:
        name = name_match.group(1).strip() or name

    return clean(RoomMap(grid, rot, name, objects))


def serialize_json(room: RoomMap, label: Optional[str] = None) -> str:
    """输出约定 JSON 文件：可存档、可回读，不依赖 C 代码。

    tiles / rotation 按【一行一格】排版，不把每个数字拆成单独一行。
    label 只给自定义模板用（显示名，可中文）；普通房间存档不写这个字段。
    """
    room = clean(room)
    lines: List[str] = ["{"]
    lines.append('  "format": "map_designer",')
    lines.append('  "version": 4,')
    lines.append('  "name": %s,' % json.dumps(room.name, ensure_ascii=False))
    if label:
        lines.append('  "label": %s,' % json.dumps(label, ensure_ascii=False))
    lines.append('  "size": {"cols": %d, "rows": %d, "tile": %d},'
                 % (room.cols, room.height, TILE))
    lines.append('  "tile_encoding": "index",')
    lines.append('  "tiles": [')
    for y, row in enumerate(room.rows):
        values = ", ".join(str(TILE_INDEX[ch]) for ch in row)
        lines.append("    [%s]%s" % (values, "," if y + 1 < len(room.rows) else ""))
    lines.append("  ],")
    lines.append('  "rotation": [')
    for y, row in enumerate(room.rot):
        values = ", ".join(str(int(value) & 3) for value in row)
        lines.append("    [%s]%s" % (values, "," if y + 1 < len(room.rows) else ""))
    lines.append("  ],")
    if room.objects:
        lines.append('  "objects": [')
        for index, obj in enumerate(room.objects):
            lines.append(
                '    {"kind": %s, "x": %d, "y": %d, "arg": %d}%s'
                % (
                    json.dumps(obj.kind, ensure_ascii=False),
                    obj.x, obj.y, obj.arg,
                    "," if index + 1 < len(room.objects) else "",
                )
            )
        lines.append("  ],")
    else:
        lines.append('  "objects": [],')
    lines.append('  "sha256": %s' % json.dumps(digest_of(room), ensure_ascii=False))
    lines.append("}")
    return "\n".join(lines) + "\n"


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

    if data.get("format") == "map_designer_layout":
        if data.get("version") != 1:
            raise MapError("不支持的布局池文件版本")
        base = pool_map(data.get("map"))
        expanded = json.loads(serialize_json(base))
        expanded["name"] = data.get("name", name)
        expanded["objects"] = data.get("objects", [])
        room = parse_json(json.dumps(expanded), name)
        if "layout_id" in data:
            canonical = pool_layout_name(data.get("map"), data["layout_id"])
            if data.get("name", Path(canonical).stem) != Path(canonical).stem:
                raise MapError("布局名称必须与房型和编号一致：%s" % canonical)
            room.layout_id = data["layout_id"]
            room.name = Path(canonical).stem
        return room

    tiles = data.get("tiles", data.get("map"))
    if not isinstance(tiles, list) or not tiles:
        raise MapError("JSON 缺少 tiles（或 map）数组")
    height = len(tiles)
    numeric_tiles = isinstance(tiles[0], list)
    if not numeric_tiles and not isinstance(tiles[0], str):
        raise MapError("JSON tiles 第 1 行既不是数字数组也不是字符串")
    cols = len(tiles[0])
    check_size(cols, height)

    size = data.get("size") or {}
    if isinstance(size, dict):
        if "cols" in size and int(size["cols"]) != cols:
            raise MapError("JSON 尺寸 cols=%r，实际 %d" % (size["cols"], cols))
        if "rows" in size and int(size["rows"]) != height:
            raise MapError("JSON 尺寸 rows=%r，实际 %d" % (size["rows"], height))

    grid: List[List[str]] = []
    for index, line in enumerate(tiles, 1):
        if len(line) != cols:
            raise MapError("JSON 第 %d 行长度 %d，需要 %d" % (index, len(line), cols))
        if numeric_tiles:
            row: List[str] = []
            for x, value in enumerate(line):
                try:
                    value = int(value)
                except (TypeError, ValueError):
                    raise MapError("JSON tiles (%d,%d) 不是数字：%r" % (x, index - 1, value)) from None
                ch = TILE_CHAR.get(value)
                if ch is None:
                    raise MapError("JSON tiles (%d,%d) 编号 %r 不在 0..10" % (x, index - 1, value))
                row.append(ch)
            grid.append(row)
        else:
            if not isinstance(line, str):
                raise MapError("JSON 第 %d 行不是字符串" % index)
            bad = sorted(set(line) - ALLOWED)
            if bad:
                raise MapError("JSON 第 %d 行有非法字符 %r" % (index, "".join(bad)))
            grid.append(list(line))

    rot = [[0] * cols for _ in range(height)]
    rotation = data.get("rotation")
    if rotation is not None:
        if not isinstance(rotation, list) or len(rotation) != height:
            raise MapError("JSON rotation 需要 %d 行" % height)
        for y, line in enumerate(rotation):
            if not isinstance(line, list) or len(line) != cols:
                raise MapError("JSON rotation 第 %d 行需要 %d 个数" % (y + 1, cols))
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
        if not (0 <= x < cols and 0 <= y < height):
            raise MapError("JSON 对象坐标越界：(%d,%d)，画布是 %d×%d" % (x, y, cols, height))
        if not 0 <= arg <= 255:
            raise MapError("JSON 对象参数必须在 0..255：%r" % (item,))
        objects.append(MapObject(kind, x, y, arg))

    if isinstance(data.get("name"), str) and data["name"].strip():
        name = data["name"].strip()
    return RoomMap(grid, rot, name, objects)


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
