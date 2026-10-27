# -*- coding: utf-8 -*-
"""mapfmt 的单元测试：格式 / 对象层 / 校验 / C 生成与回读。"""

import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import mapfmt  # noqa: E402


MAP_PATH = HERE / "output" / "_archive" / "room_map.txt"
GAME_WORLD = HERE.parent.parent / "code" / "logic" / "game" / "game_world.c"


class MapFmtTest(unittest.TestCase):
    def setUp(self):
        self.text = MAP_PATH.read_text(encoding="utf-8")
        self.room = mapfmt.parse_text(self.text)
        self.clean_text = mapfmt.serialize_text(mapfmt.clean(self.room))

    def clone(self):
        return mapfmt.RoomMap(
            [row[:] for row in self.room.rows],
            [row[:] for row in self.room.rot],
            self.room.name,
            list(self.room.objects),
        )

    # ---- 基本 ----------------------------------------------------------

    def test_parse_dimensions(self):
        self.assertEqual(len(self.room.rows), mapfmt.ROWS)
        self.assertTrue(all(len(row) == mapfmt.COLS for row in self.room.rows))
        self.assertEqual(len(self.room.rot), mapfmt.ROWS)

    def test_valid_map_has_no_errors(self):
        issues = mapfmt.validate(self.room)
        self.assertEqual(mapfmt.errors_of(issues), [])
        self.assertEqual(issues, [])

    def test_stone_floor_is_separate_terrain(self):
        room = self.clone()
        room.rows[8][15] = "S"
        room.rot[8][15] = 0
        issues = mapfmt.validate(room)
        self.assertEqual(mapfmt.errors_of(issues), [])
        self.assertEqual(mapfmt.stats(room)["counts"]["S"], 1)
        back = mapfmt.parse_text(mapfmt.serialize_text(room))
        self.assertEqual(back.rows[8][15], "S")
        c_back = mapfmt.parse_c(mapfmt.emit_c(room))
        self.assertEqual(c_back.rows[8][15], "S")

    def test_text_roundtrip(self):
        out = mapfmt.serialize_text(self.room)
        self.assertEqual(out.rstrip("\n"), self.text.rstrip("\n"))

    def test_sparse_rotation_roundtrip(self):
        sparse = mapfmt.serialize_text(self.room, sparse=True)
        self.assertIn("# ROTATION sparse", sparse)
        back = mapfmt.parse_text(sparse)
        self.assertEqual(mapfmt.serialize_text(back), mapfmt.serialize_text(self.room))
        self.assertEqual(back.objects, self.room.objects)

    def test_stats_doors_and_objects(self):
        info = mapfmt.stats(self.room)
        self.assertEqual(info["counts"]["D"], 4)
        self.assertEqual(info["counts"]["B"], 299)
        self.assertEqual([door["dir"] for door in info["doors"]], ["N", "W", "E", "S"])
        self.assertEqual(len(info["sha256"]), 64)
        self.assertGreater(len(info["objects"]), 0)
        self.assertEqual(info["name"], self.room.name)

    def test_legacy_template_word_ignored(self):
        """旧文件里的 '# TEMPLATE empty' 只是历史遗留：不报错、不改变图。"""
        legacy = "# MAP_DESIGNER v3\n# TEMPLATE boss\n" + "\n".join(
            "".join(row) for row in self.room.rows
        ) + "\n"
        back = mapfmt.parse_text(legacy)
        self.assertEqual(back.size, self.room.size)
        self.assertEqual(
            ["".join(row) for row in back.rows],
            ["".join(row) for row in self.room.rows],
        )
        self.assertNotIn("# TEMPLATE", mapfmt.serialize_text(back))

    def test_emit_c_name_and_no_template(self):
        text = mapfmt.emit_c(self.clone())
        self.assertIn('#define MAP_NAME "', text)
        self.assertNotIn("MAP_TEMPLATE_ROOM", text)

    def test_v2_compatibility(self):
        body = "\n".join("".join(row) for row in self.room.rows)
        rot = "\n".join("".join(str(value) for value in row) for row in self.room.rot)
        v2 = "# DEBUG_ROOM_MAP v2\n" + body + "\n# ROTATION\n" + rot + "\n"
        back = mapfmt.parse_text(v2)
        self.assertEqual(back.objects, [])
        self.assertEqual(back.size, (mapfmt.COLS, mapfmt.ROWS))

    # ---- C ------------------------------------------------------------

    def test_c_roundtrip(self):
        text = mapfmt.emit_c(self.room)
        self.assertIn("room_map_shared", text)
        self.assertIn("room_rot_shared", text)
        self.assertIn("room_objects", text)
        self.assertIn("ROOM_OBJECT_COUNT", text)
        self.assertIn("MAP_NAME", text)
        back = mapfmt.parse_c(text)
        self.assertEqual(mapfmt.serialize_text(back), self.clean_text)
        self.assertEqual(back.objects, mapfmt.clean(self.room).objects)

    def test_c_import_from_emitted_artifact(self):
        """工程里的地形现在是生成的了；能导入的 C 只剩 output/ 里导出的这份。"""
        c_path = HERE / "output" / "game_world_map.c"
        if not c_path.is_file():
            self.skipTest("output/game_world_map.c 不存在")
        back = mapfmt.parse_c(c_path.read_text(encoding="utf-8"))
        json_path = HERE / "output" / "_archive" / "room_map.json"
        expected = (
            mapfmt.parse_json(json_path.read_text(encoding="utf-8"))
            if json_path.is_file()
            else self.room
        )
        self.assertEqual(
            ["".join(row) for row in back.rows],
            ["".join(row) for row in expected.rows],
        )
        self.assertEqual(back.rot, expected.rot)

    def test_json_roundtrip(self):
        text = mapfmt.serialize_json(self.room)
        self.assertIn('"format": "map_designer"', text)
        self.assertIn('"tile_encoding": "index"', text)
        self.assertNotIn('"BBBB', text)
        self.assertIn('"name"', text)
        self.assertIn('"objects"', text)
        back = mapfmt.parse_json(text)
        self.assertEqual(mapfmt.serialize_text(back), self.clean_text)
        self.assertEqual(back.objects, mapfmt.clean(self.room).objects)

    def test_parse_any(self):
        self.assertEqual(mapfmt.parse_any(self.text)[1], "text")
        self.assertEqual(mapfmt.parse_any(mapfmt.emit_c(self.room))[1], "c")
        self.assertEqual(mapfmt.parse_any(mapfmt.serialize_json(self.room))[1], "json")

    # ---- 对象层 --------------------------------------------------------

    def test_object_line_aliases(self):
        obj = mapfmt.parse_object_line("mob_slime 12,8")
        self.assertEqual((obj.kind, obj.x, obj.y, obj.arg), ("slime", 12, 8, 0))
        obj = mapfmt.parse_object_line("crate 3 4 7")
        self.assertEqual((obj.kind, obj.x, obj.y, obj.arg), ("crate", 3, 4, 7))

    def test_object_outside_arena(self):
        room = self.clone()
        room.objects.append(mapfmt.MapObject("slime", 1, 1))
        messages = [issue.message for issue in mapfmt.validate(room)]
        self.assertTrue(any("竞技场外" in message for message in messages), messages)

    def test_object_not_on_floor(self):
        room = self.clone()
        room.rows[5][20] = "W"                               # 竞技场内改成墙
        room.objects.append(mapfmt.MapObject("web", 20, 5))
        messages = [issue.message for issue in mapfmt.validate(room)]
        self.assertTrue(any("地板 B/K/S" in message for message in messages), messages)

    def test_duplicate_object_warning(self):
        room = self.clone()
        room.objects.append(mapfmt.MapObject("stone", 11, 10))
        messages = [issue.message for issue in mapfmt.validate(room)]
        self.assertTrue(any("重复" in message for message in messages), messages)

    def test_blocker_overlap_warning(self):
        room = self.clone()
        room.objects.append(mapfmt.MapObject("box", 11, 10))   # 和 stone 同格
        messages = [issue.message for issue in mapfmt.validate(room)]
        self.assertTrue(any("阻挡物会重叠" in message for message in messages), messages)

    def test_too_many_mobs_warning(self):
        room = self.clone()
        for _ in range(mapfmt.OBJECT_LIMITS["mob"] + 1):
            room.objects.append(mapfmt.MapObject("slime", 10, 10))
        messages = [issue.message for issue in mapfmt.validate(room)]
        self.assertTrue(any("N_MOBS" in message for message in messages), messages)

    # ---- 校验规则 ------------------------------------------------------

    def test_wrong_door_direction(self):
        room = self.clone()
        room.rot[2][20] = 1              # 顶门在 N 墙，必须是 0
        messages = [issue.message for issue in mapfmt.validate(room)]
        self.assertTrue(any("方向" in message for message in messages), messages)

    def test_door_half_not_empty(self):
        room = self.clone()
        room.rows[3][20] = "W"           # 顶门的另一半
        messages = [issue.message for issue in mapfmt.validate(room)]
        self.assertTrue(any("另一格" in message for message in messages), messages)

    def test_door_outside_band(self):
        room = self.clone()
        room.rows[10][10] = "D"          # 竞技场中间
        room.rot[10][10] = 0
        messages = [issue.message for issue in mapfmt.validate(room)]
        self.assertTrue(any("墙带" in message for message in messages), messages)

    def test_hole_in_wall_band(self):
        room = self.clone()
        room.rows[2][19] = "."
        room.rot[2][19] = 0
        messages = [issue.message for issue in mapfmt.validate(room)]
        self.assertTrue(any("漏出一个洞" in message for message in messages), messages)

    def test_l_shape_passes_validation(self):
        room = self.clone()
        # 把右上角挖成 L 形缺口（墙），再把落在缺口里的对象移掉
        for y in range(4, 10):
            for x in range(24, 32):
                room.rows[y][x] = "W"
                room.rot[y][x] = 0
        room.objects = [
            obj for obj in room.objects
            if not (24 <= obj.x < 32 and 4 <= obj.y < 10)
        ]
        issues = mapfmt.validate(room)
        self.assertEqual(mapfmt.errors_of(issues), [])
        self.assertFalse(any("会挡住玩家" in issue.message for issue in issues))
        self.assertTrue(any("异形房间" in issue.message for issue in issues))

    def test_outside_room_warning(self):
        room = self.clone()
        room.rows[0][0] = "W"
        messages = [issue.message for issue in mapfmt.validate(room)]
        self.assertTrue(any("房间外" in message for message in messages), messages)

    # ---- 格式错误 ------------------------------------------------------

    def test_short_row(self):
        lines = mapfmt.serialize_text(self.room).split("\n")
        for index, line in enumerate(lines):
            if len(line) == mapfmt.COLS:
                lines[index] = line[:-1]
                break
        with self.assertRaises(mapfmt.MapError):
            mapfmt.parse_text("\n".join(lines))

    def test_bad_char(self):
        lines = mapfmt.serialize_text(self.room).split("\n")
        for index, line in enumerate(lines):
            if len(line) == mapfmt.COLS and set(line) <= set(".BWDCXYZ"):
                lines[index] = "Q" + line[1:]
                break
        with self.assertRaises(mapfmt.MapError):
            mapfmt.parse_text("\n".join(lines))

    def test_missing_rotation_rows(self):
        lines = mapfmt.serialize_text(self.room).split("\n")
        marker = lines.index("# ROTATION")
        cut = lines[: marker + 1 + 5]
        with self.assertRaises(mapfmt.MapError):
            mapfmt.parse_text("\n".join(cut))

    def test_legacy_v1(self):
        body = "\n".join("".join(row) for row in self.room.rows)
        back = mapfmt.parse_text(body)
        self.assertTrue(all(value == 0 for row in back.rot for value in row))
        self.assertEqual(back.objects, [])

    def test_bad_object_kind(self):
        with self.assertRaises(mapfmt.MapError):
            mapfmt.parse_object_line("dragon 12,8")

    def test_bad_object_bounds(self):
        with self.assertRaises(mapfmt.MapError):
            mapfmt.parse_object_line("slime 99,8")


def build_room(cols, rows):
    """按给定尺寸画一张最小合规房间：地板 + 2 格墙带 + 四扇门。

    竞技场留 4 格边距（所以最小可用尺寸是 18×18）；门按 door_slots 摆。
    """
    grid = [["." for _ in range(cols)] for _ in range(rows)]
    rot = [[0 for _ in range(cols)] for _ in range(rows)]
    x0, x1 = 4, cols - 5
    y0, y1 = 4, rows - 5
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            grid[y][x] = "B"
    for y in range(y0 - 2, y1 + 3):
        for x in range(x0 - 2, x1 + 3):
            if x0 <= x <= x1 and y0 <= y <= y1:
                continue
            grid[y][x] = "W"
    room = mapfmt.RoomMap([list(r) for r in grid], [list(r) for r in rot], "built", [])
    for (dx, dy, angle) in mapfmt.door_slots(room):
        px, py = mapfmt.door_pair(dx, dy, angle)
        grid[dy][dx] = "D"
        rot[dy][dx] = angle
        grid[py][px] = "."
        rot[py][px] = 0
    return mapfmt.RoomMap([list(r) for r in grid], [list(r) for r in rot], "built", [])


class FreeCanvasTest(unittest.TestCase):
    """尺寸自由：40×23 / 45×35 都能画，越界和不对齐要报错。"""

    def test_default_canvas_geometry(self):
        room = mapfmt.parse_text(MAP_PATH.read_text(encoding="utf-8"))
        self.assertEqual(room.size, (40, 23))
        self.assertEqual(mapfmt.arena_rect(room), (9, 4, 31, 16))
        # 和改造前写死的 ARENA 像素矩形 288,128→1024,544 等价
        self.assertEqual(
            mapfmt.door_slots(room),
            [(20, 2, 0), (32, 10, 1), (20, 17, 2), (7, 10, 3)],
        )

    def test_large_canvas_valid(self):
        room = build_room(45, 35)
        self.assertEqual(room.size, (45, 35))
        self.assertEqual(mapfmt.errors_of(mapfmt.validate(room)), [])
        self.assertEqual(mapfmt.arena_rect(room), (4, 4, 40, 30))
        self.assertEqual(
            [door["dir"] for door in mapfmt.stats(room)["doors"]],
            ["N", "W", "E", "S"],
        )

    def test_smallest_canvas(self):
        room = build_room(18, 18)
        self.assertEqual(mapfmt.errors_of(mapfmt.validate(room)), [])

    def test_canvas_roundtrip_45x35(self):
        room = build_room(45, 35)
        text = mapfmt.serialize_text(room)
        self.assertEqual(mapfmt.parse_text(text).size, (45, 35))

        back_json = mapfmt.parse_json(mapfmt.serialize_json(room))
        self.assertEqual(back_json.size, (45, 35))

        back_c = mapfmt.parse_c(mapfmt.emit_c(room))
        self.assertEqual(back_c.size, (45, 35))
        self.assertEqual(mapfmt.serialize_text(back_c), mapfmt.serialize_text(mapfmt.clean(room)))

    def test_emit_c_has_size_and_arena(self):
        text = mapfmt.emit_c(build_room(45, 35))
        self.assertIn("#define MAP_COLS 45", text)
        self.assertIn("#define MAP_ROWS 35", text)
        self.assertIn("#define MAP_ARENA_X0 4", text)
        self.assertIn("#define MAP_ARENA_Y1 30", text)

    def test_too_wide(self):
        with self.assertRaises(mapfmt.MapError):
            mapfmt.parse_text("\n".join("." * 46 for _ in range(23)))

    def test_too_tall(self):
        with self.assertRaises(mapfmt.MapError):
            mapfmt.parse_text("\n".join("." * 40 for _ in range(36)))

    def test_too_small(self):
        with self.assertRaises(mapfmt.MapError):
            mapfmt.parse_text("\n".join("." * 7 for _ in range(9)))

    def test_ragged_rows(self):
        lines = ["." * 40 for _ in range(20)] + ["." * 39] + ["." * 40 for _ in range(2)]
        with self.assertRaises(mapfmt.MapError):
            mapfmt.parse_text("\n".join(lines))

    def test_json_size_mismatch(self):
        room = build_room(40, 23)
        payload = mapfmt.serialize_json(room).replace('"cols": 40', '"cols": 45')
        with self.assertRaises(mapfmt.MapError):
            mapfmt.parse_json(payload)

    def test_c_designated_rotation(self):
        cols, rows = 10, 8
        # 旋转只对非 '.' 的格子有意义（clean 会把空白格的旋转归零）
        body = [["." for _ in range(cols)] for _ in range(rows)]
        body[0][0] = "W"
        body[0][9] = "W"
        body[1][4] = "W"
        map_rows = "".join('    "%s",\n' % "".join(row) for row in body)
        rot_rows = "".join(
            '    [%d] = { %s },\n'
            % (
                y,
                "[0] = 3, [9] = 1" if y == 0 else "[4] = 2" if y == 1 else "[0] = 0",
            )
            for y in range(rows)
        )
        source = (
            '#define MAP_NAME "boss"\n'
            "static const char room_map_shared[%d][%d] = {\n%s};\n"
            "static const unsigned char room_rot_shared[%d][%d] = {\n%s};\n"
            % (rows, cols + 1, map_rows, rows, cols, rot_rows)
        )
        room = mapfmt.parse_c(source)
        self.assertEqual(room.size, (cols, rows))
        self.assertEqual(room.rot[0][0], 3)
        self.assertEqual(room.rot[0][9], 1)
        self.assertEqual(room.rot[1][4], 2)
        self.assertEqual(room.rot[1][0], 0)

    def test_object_bounds_follow_canvas(self):
        room = build_room(45, 35)
        room.objects.append(mapfmt.MapObject("slime", 44, 34))
        messages = [issue.message for issue in mapfmt.validate(room)]
        self.assertTrue(any("竞技场外" in message for message in messages), messages)


class FrameTest(unittest.TestCase):
    """标准矩形框架：参数 ⇄ 格子（和固件 game_frame.c 同一套规则）。"""

    def test_live_room_is_standard_frame(self):
        room = mapfmt.parse_text(MAP_PATH.read_text(encoding="utf-8"))
        frame = mapfmt.detect_frame(room)
        self.assertIsNotNone(frame)
        self.assertEqual(frame["arena"], [9, 4, 31, 16])
        self.assertEqual(frame["doors"], [20, 10, 20, 10])
        self.assertEqual(frame["band_rot"], [0, 1, 2, 3])
        self.assertEqual(frame["corner"], 1)
        for y in range(room.height):
            for x in range(room.cols):
                self.assertEqual(mapfmt.frame_tile(frame, x, y), room.rows[y][x])
                self.assertEqual(mapfmt.frame_rot(frame, x, y), room.rot[y][x] & 3)

    def test_45x35_frame_is_standard(self):
        room = mapfmt.make_room(45, 35, (8, 7, 36, 27))
        frame = mapfmt.detect_frame(room)
        self.assertIsNotNone(frame)
        self.assertEqual(frame["arena"], [8, 7, 36, 27])
        self.assertEqual(frame["corner"], 0)
        self.assertEqual(frame["band_rot"], [0, 0, 0, 0])
        self.assertEqual(mapfmt.frame_world(frame), (1440, 1120))

    def test_template_2x2_is_a_full_room(self):
        """2×2 模板 = 完整房间（角块 + 每边 2 扇门），不是框架参数。"""
        room = mapfmt.make_template("template_2x2")
        self.assertEqual((room.cols, room.height), (45, 35))
        doors = sum(1 for y in range(room.height) for x in range(room.cols)
                    if room.rows[y][x] == "D")
        self.assertEqual(doors, 8)
        self.assertIsNone(mapfmt.detect_frame(room))

    def test_template_L_has_notch(self):
        """L 模板左上/右上/右下有地板，左下是缺口（黑）。"""
        room = mapfmt.make_template("template_L")
        self.assertEqual((room.cols, room.height), (45, 35))
        self.assertTrue(all(room.rows[y][x] == "." for y in range(27, 35) for x in range(6, 14)))

    def test_small_template_matches_live_room(self):
        live = mapfmt.parse_text(MAP_PATH.read_text(encoding="utf-8"))
        tpl = mapfmt.make_template("small")

        def shape(rows):
            # 模板地板是 S，旧布局是 B；形状 / 墙 / 门 / 角块必须一致。
            return ["".join("F" if ch in "BKS" else ch for ch in row) for row in rows]

        self.assertEqual(shape(tpl.rows), shape(live.rows))
        self.assertEqual(tpl.rot, live.rot)

    def test_frame_c_line(self):
        frame = mapfmt.detect_frame(mapfmt.make_room(40, 23, (9, 4, 31, 16),
                                                     corner=1, band_rot=(0, 1, 2, 3)))
        line = mapfmt.frame_c(frame, "small")
        self.assertIn("{ 40, 23, 9, 4, 31, 16, { 20, 10, 20, 10 }", line)
        self.assertIn("1280, 720", line)

    def test_custom_terrain_is_not_a_frame(self):
        room = mapfmt.make_template("small")
        room.rows[10][10] = "W"          # 竞技场正中加一堵墙
        room.rot[10][10] = 0
        self.assertIsNone(mapfmt.detect_frame(room))

    def test_single_odd_wall_rotation_is_not_a_frame(self):
        room = mapfmt.make_template("small")
        room.rot[10][7] = (room.rot[10][7] + 1) & 3   # 左墙一格木纹转错
        self.assertIsNone(mapfmt.detect_frame(room))


class CustomTemplateTest(unittest.TestCase):
    """自定义模板：保存到 templates/custom/，能列出、回读、删除，且不碰内置模板。"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.original_dir = mapfmt.CUSTOM_TEMPLATE_DIR
        mapfmt.CUSTOM_TEMPLATE_DIR = Path(self.tmp.name) / "custom"
        self.addCleanup(setattr, mapfmt, "CUSTOM_TEMPLATE_DIR", self.original_dir)

    def make_room(self):
        room = mapfmt.make_room(20, 15, (3, 3, 16, 11), name="mine")
        room.objects.append(mapfmt.MapObject("slime", 8, 8))
        return room

    def test_save_list_load_roundtrip(self):
        info = mapfmt.save_custom_template(self.make_room(), "my_l_room", "我的 L 房")
        self.assertEqual(info["id"], "custom_my_l_room")
        self.assertFalse(info["existed"])

        custom = [item for item in mapfmt.template_catalog() if not item["builtin"]]
        self.assertEqual(len(custom), 1)
        self.assertEqual(custom[0]["id"], "custom_my_l_room")
        self.assertEqual(custom[0]["label"], "我的 L 房")
        self.assertEqual(custom[0]["size"], [20, 15])

        back = mapfmt.make_template("custom_my_l_room")
        self.assertEqual(back.size, (20, 15))
        self.assertEqual(back.objects, [mapfmt.MapObject("slime", 8, 8)])
        self.assertEqual(mapfmt.template_label("custom_my_l_room"), "我的 L 房")

        path = mapfmt.CUSTOM_TEMPLATE_DIR / "custom_my_l_room.json"
        text = path.read_text(encoding="utf-8")
        self.assertIn('"label": "我的 L 房"', text)
        self.assertIn('"tiles": [', text)

    def test_save_overwrites_same_name(self):
        mapfmt.save_custom_template(self.make_room(), "same", "第一次")
        info = mapfmt.save_custom_template(self.make_room(), "same", "第二次")
        self.assertTrue(info["existed"])
        self.assertEqual(mapfmt.template_label("custom_same"), "第二次")

    def test_delete_custom_only(self):
        info = mapfmt.save_custom_template(self.make_room(), "gone", "临时")
        mapfmt.delete_custom_template(info["id"])
        self.assertIsNone(mapfmt._find_custom_template(info["id"]))
        with self.assertRaises(mapfmt.MapError):
            mapfmt.make_template(info["id"])
        with self.assertRaises(mapfmt.MapError):
            mapfmt.delete_custom_template("template_L")

    def test_invalid_slug_is_rejected(self):
        with self.assertRaises(mapfmt.MapError):
            mapfmt.save_custom_template(self.make_room(), "../escape", "bad")
        with self.assertRaises(mapfmt.MapError):
            mapfmt.save_custom_template(self.make_room(), "有中文", "bad")

    def test_optional_label_is_ignored_by_parser(self):
        room = self.make_room()
        text = mapfmt.serialize_json(room, label="显示名")
        self.assertIn('"label": "显示名"', text)
        back = mapfmt.parse_json(text)
        self.assertEqual(back.size, room.size)
        self.assertEqual(back.rows, room.rows)


class LayoutDataTest(unittest.TestCase):
    """房间数据导出：output/<房间名>.json → game_layout_data.c/.h。"""

    def items(self):
        return [
            ("boss", mapfmt.make_room(45, 35, (8, 7, 36, 27))),
            ("map1", mapfmt.parse_text(MAP_PATH.read_text(encoding="utf-8"))),
        ]

    def test_emit_contains_symbols(self):
        header, source, warnings = mapfmt.emit_layout_data(self.items())
        self.assertIn("#define LAY_boss", header)
        self.assertIn("#define LAY_map1", header)
        self.assertIn("#define LAYOUT_COUNT 2", header)
        self.assertIn("extern const room_layout_t g_layouts[LAYOUT_COUNT];", header)
        self.assertIn("const obj_place_t L_map1[]", source)
        self.assertIn('#define O(x, y, k)', source)
        # room_map.txt 里有一个 exit（固件没有这种对象）→ 跳过并提示
        self.assertTrue(any("exit" in message for message in warnings), warnings)

    def test_emit_sorted_table(self):
        _header, source, _warnings = mapfmt.emit_layout_data(self.items())
        boss = source.index('{ "boss"')
        map1 = source.index('{ "map1"')
        self.assertLess(boss, map1)
        self.assertIn('{ "boss", L_boss, 0, 1, 0 }', source)

    def test_emit_is_deterministic(self):
        first = mapfmt.emit_layout_data(self.items())
        second = mapfmt.emit_layout_data(list(reversed(self.items())))
        self.assertEqual(first, second)

    def test_emit_rejects_invalid_layout(self):
        room = mapfmt.make_template("small")
        room.objects.append(mapfmt.MapObject("slime", 1, 1))     # 竞技场外
        with self.assertRaises(mapfmt.MapError):
            mapfmt.emit_layout_data([("bad", room)])

    def test_emit_skips_unknown_object(self):
        room = mapfmt.make_room(40, 23, (9, 4, 31, 16), corner=1, band_rot=(0, 1, 2, 3))
        room.objects = [mapfmt.MapObject("exit", 20, 8)]
        _header, source, warnings = mapfmt.emit_layout_data([("only_exit", room)])
        self.assertTrue(any("exit" in w for w in warnings), warnings)
        self.assertIn('{ "only_exit", L_only_exit, 0, 0, 0 }', source)

    def test_emit_rejects_odd_canvas(self):
        room = mapfmt.make_room(20, 20, (4, 4, 15, 15))
        with self.assertRaises(mapfmt.MapError):
            mapfmt.emit_layout_data([("odd", room)])

    def test_layout_map_index(self):
        self.assertEqual(mapfmt.layout_map_index(mapfmt.make_room(40, 23, (9, 4, 31, 16))), 0)
        self.assertEqual(mapfmt.layout_map_index(mapfmt.make_room(45, 35, (8, 7, 36, 27))), 1)

    def test_layout_map_index_uses_firmware_frames(self):
        """框架表是固件说了算：多一种尺寸就多一个下标。"""
        frames = [
            {"cols": 40, "rows": 23},
            {"cols": 45, "rows": 35},
            {"cols": 40, "rows": 35},        # 以后加 map1x2 就是这一行
        ]
        tall = mapfmt.make_room(40, 35, (9, 4, 31, 28))
        self.assertEqual(mapfmt.layout_map_index(tall, frames), 2)
        odd = mapfmt.make_room(30, 30, (4, 4, 25, 25))
        with self.assertRaises(mapfmt.MapError) as ctx:
            mapfmt.layout_map_index(odd, frames)
        self.assertIn("40×35", str(ctx.exception))

    def test_parse_world_frames(self):
        world = (HERE.parent.parent
                 / "project" / "GamingPlayUnit" / "embedded_sw" / "SoC_RISCV32"
                 / "software" / "standalone" / "GamingPlayUnit" / "game" / "game_world.c")
        if not world.is_file():
            self.skipTest("game_world.c 不存在")
        frames = mapfmt.parse_world_frames(world.read_text(encoding="utf-8"))
        self.assertEqual([(f["cols"], f["rows"]) for f in frames],
                         [(40, 23), (45, 35), (40, 23)])
        self.assertEqual(frames[0]["arena"], [9, 4, 31, 16])
        self.assertEqual(frames[1]["doors"], [22, 17, 22, 17])
        #  第 3 行是隐藏房皮肤：同一套 1×1 几何，只换地板/门字符
        self.assertEqual((frames[2]["floor_ch"], frames[2]["wall_ch"],
                          frames[2]["door_ch"], frames[2]["corner_ch"]),
                         ("K", "w", "H", "c"))

    def test_layout_symbol(self):
        self.assertEqual(mapfmt.layout_symbol("boss"), "boss")
        self.assertEqual(mapfmt.layout_symbol("2x2-map"), "_2x2_map")
        self.assertEqual(mapfmt.layout_symbol(""), "layout")


if __name__ == "__main__":
    unittest.main(verbosity=2)
