#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_game_layout.py —— 校验房间布局能不能落地。

数据来源（都是机器产出的，不用手写）：
  · 框架参数：game_world.c 的 g_maps[]
  · 布局数据：game_layout_data.c（tools/map_designer 生成）
这里用 mapfmt 的同一套规则重建地形，所以不需要再解析整张地图表。

查什么：
  · 错误：对象落在竞技场地板（B）以外 / 不在 B 上
  · 错误：同布局内重复摆同一格
  · 错误：单布局对象数超过 N_MOBS / N_WEBS / N_STONES
  · 提示：压在门道线上、房间中心是障碍（运行时会绕开，但值得看一眼）

用法：
    python tools/map_designer/check_game_layout.py            # 查发布工程那份
    python tools/map_designer/check_game_layout.py --all      # 工程 + code/ 归档都查
退出码 0 = 全过，1 = 有问题。
"""
import io
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mapfmt  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))

TARGETS = [
    "project/GamingPlayUnit/embedded_sw/SoC_RISCV32/software/standalone/GamingPlayUnit/game",
    "code/logic/game",
]

LIMIT = {"OBJ_SLIME": 16, "OBJ_BUG": 16, "OBJ_WEB": 40, "OBJ_STONE": 128, "OBJ_CRATE": 128, "OBJ_BOX": 128}
BLOCK = ("OBJ_STONE", "OBJ_BOX", "OBJ_CRATE")

def parse_frames(world_src):
    """从 game_world.c 的 g_maps[] 读框架参数（实现在 mapfmt，导出工具共用）。"""
    try:
        return mapfmt.parse_world_frames(world_src)
    except mapfmt.MapError as exc:
        raise SystemExit(str(exc)) from None


def parse_layouts(data_src):
    """从 game_layout_data.c 读布局：[(名字, 数组名, [(x, y, kind)], 地图下标)]。

    ★ 布局数据现在是 tools/map_designer 生成的（--emit-layouts），
      手写的 game_layout.c 只剩"哪种房间用哪些布局"。
    """
    arrays = {}
    for match in re.finditer(r"const obj_place_t (L_\w+)\[\]\s*=\s*\{(.*?)\n\};", data_src, re.S):
        arrays[match.group(1)] = re.findall(r"O\((\d+)\s*,\s*(\d+)\s*,\s*(OBJ_\w+)\)", match.group(2))
    layouts = []
    for match in re.finditer(
        r'\{\s*"(\w+)"\s*,\s*(0|L_\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(0|&?T_\w+)\s*\}',
        data_src,
    ):
        name = match.group(1)
        arr = None if match.group(2) == "0" else match.group(2)
        map_idx = int(match.group(4))
        terrain = match.group(5) not in ("0", "")
        objs = arrays.get(arr, []) if arr else []
        layouts.append((name, arr, objs, -1 if terrain else map_idx))
    return layouts


def check(game_dir):
    data_path = os.path.join(game_dir, "game_layout_data.c")
    if not os.path.isfile(data_path):
        print("  ! 没有 game_layout_data.c（先跑 serve.py --emit-layouts --apply）")
        return 1, 0
    data = io.open(data_path, encoding="utf-8").read()
    world = io.open(os.path.join(game_dir, "game_world.c"), encoding="utf-8").read()

    frames = parse_frames(world)
    layouts = parse_layouts(data)
    if not layouts:
        print("  ! game_layout_data.c 里没解析到 g_layouts[]")
        return 1, 0

    bad = 0
    warn = 0
    for name, arr, objs, map_idx in layouts:
        if map_idx < 0:
            print("  %-12s objects=%2d  异形房（自带地形，不做逐格检查）" % (name, len(objs)))
            continue
        frame = frames[map_idx] if 0 <= map_idx < len(frames) else frames[0]
        ax0, ay0, ax1, ay1 = frame["arena"]
        seen = {}
        cnt = {}
        for xs, ys, kind in objs:
            x, y = int(xs), int(ys)
            cnt[kind] = cnt.get(kind, 0) + 1
            if (x, y) in seen:
                print("  ! %s 重复摆 (%d,%d)" % (name, x, y))
                bad += 1
            seen[(x, y)] = kind
            if not (ax0 <= x <= ax1 and ay0 <= y <= ay1):
                print("  ! %s 出竞技场 (%d,%d)（框架 %d×%d 竞技场 %d..%d / %d..%d）"
                      % (name, x, y, frame["cols"], frame["rows"], ax0, ax1, ay0, ay1))
                bad += 1
            tile = mapfmt.frame_tile(frame, x, y)
            if tile not in ("B", "K", "S"):
                print("  ! %s (%d,%d) 落在 '%s' 上，不是地板 B / K / S" % (name, x, y, tile))
                bad += 1
            doors = frame["doors"]
            lane_x = [d for d in (doors[0], doors[2]) if d != mapfmt.FRAME_DOOR_NONE]
            lane_y = [d for d in (doors[1], doors[3]) if d != mapfmt.FRAME_DOOR_NONE]
            if kind in BLOCK and (x in lane_x or y in lane_y):
                print("  ~ %s (%d,%d) 压在门道线上（运行时会绕开，不影响进房）" % (name, x, y))
                warn += 1
        center = ((ax0 + ax1) // 2, (ay0 + ay1) // 2)
        if center in seen and seen[center] in BLOCK:
            print("  ~ %s 房间中心 (%d,%d) 是障碍（运行时会把人挪到最近空格）"
                  % (name, center[0], center[1]))
            warn += 1
        for kind, n in cnt.items():
            if n > LIMIT.get(kind, 1 << 30):
                print("  ! %s %s=%d 超过池子上限 %d" % (name, kind, n, LIMIT[kind]))
                bad += 1
        desc = " ".join("%s=%d" % (k[4:], v) for k, v in sorted(cnt.items()))
        print("  %-12s objects=%2d  map=%d  %s" % (name, len(objs), map_idx, desc))
    return bad, warn


def main(argv):
    dirs = TARGETS if "--all" in argv else TARGETS[:1]
    total = 0
    warns = 0
    for rel in dirs:
        d = os.path.join(ROOT, rel)
        print("== %s" % rel)
        if not os.path.isdir(d):
            print("  (跳过：目录不存在)")
            continue
        b, w = check(d)
        total += b
        warns += w
    if total:
        print("\n结果：发现 %d 个问题（另有 %d 条提示）" % (total, warns))
    elif warns:
        print("\n结果：全部通过（%d 条提示，见上面的 ~ 行）" % warns)
    else:
        print("\n结果：全部通过")
    return 0 if total == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
