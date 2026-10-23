# -*- coding: utf-8 -*-
"""mapfmt 的单元测试：格式 / 对象层 / 校验 / C 生成与回读。"""

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import mapfmt  # noqa: E402


MAP_PATH = HERE / "output" / "room_map.txt"
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
            self.room.room_type,
        )

    # ---- 基本 ----------------------------------------------------------

    def test_parse_dimensions(self):
        self.assertEqual(len(self.room.rows), mapfmt.ROWS)
        self.assertTrue(all(len(row) == mapfmt.COLS for row in self.room.rows))
        self.assertEqual(len(self.room.rot), mapfmt.ROWS)
        self.assertIn(self.room.room_type, mapfmt.ROOM_TYPE_IDS)

    def test_valid_map_has_no_errors(self):
        issues = mapfmt.validate(self.room)
        self.assertEqual(mapfmt.errors_of(issues), [])
        self.assertEqual(issues, [])

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
        self.assertEqual(info["room_type"], self.room.room_type)

    def test_template_roundtrip(self):
        room = self.clone()
        room.room_type = "item"
        back = mapfmt.parse_text(mapfmt.serialize_text(room))
        self.assertEqual(back.room_type, "item")
        self.assertIn("MAP_TEMPLATE_ROOM 2", mapfmt.emit_c(room))

    def test_v2_compatibility(self):
        body = "\n".join("".join(row) for row in self.room.rows)
        rot = "\n".join("".join(str(value) for value in row) for row in self.room.rot)
        v2 = "# DEBUG_ROOM_MAP v2\n" + body + "\n# ROTATION\n" + rot + "\n"
        back = mapfmt.parse_text(v2)
        self.assertEqual(back.objects, [])
        self.assertEqual(back.room_type, mapfmt.DEFAULT_ROOM_TYPE)

    # ---- C ------------------------------------------------------------

    def test_c_roundtrip(self):
        text = mapfmt.emit_c(self.room)
        self.assertIn("room_map_shared", text)
        self.assertIn("room_rot_shared", text)
        self.assertIn("room_objects", text)
        self.assertIn("ROOM_OBJECT_COUNT", text)
        self.assertIn("MAP_TEMPLATE_ROOM", text)
        back = mapfmt.parse_c(text)
        self.assertEqual(mapfmt.serialize_text(back), self.clean_text)
        self.assertEqual(back.objects, mapfmt.clean(self.room).objects)

    def test_c_import_from_game_world(self):
        if not GAME_WORLD.is_file():
            self.skipTest("game_world.c 不存在")
        back = mapfmt.parse_c(GAME_WORLD.read_text(encoding="utf-8"))
        self.assertEqual(back.objects, [])
        self.assertEqual(
            ["".join(row) for row in back.rows],
            ["".join(row) for row in self.room.rows],
        )
        self.assertEqual(len(back.rot), mapfmt.ROWS)

    def test_json_roundtrip(self):
        text = mapfmt.serialize_json(self.room)
        self.assertIn('"format": "map_designer"', text)
        self.assertIn('"room_type"', text)
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
        self.assertTrue(any("基岩地板" in message for message in messages), messages)

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


if __name__ == "__main__":
    unittest.main(verbosity=2)
