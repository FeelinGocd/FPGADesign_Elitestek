# -*- coding: utf-8 -*-
"""mapfmt 的单元测试：格式 / 校验 / C 生成与回读。"""

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

    def test_text_roundtrip(self):
        out = mapfmt.serialize_text(self.room)
        self.assertEqual(out.rstrip("\n"), self.text.rstrip("\n"))

    def test_sparse_rotation_roundtrip(self):
        sparse = mapfmt.serialize_text(self.room, sparse=True)
        self.assertIn("# ROTATION sparse", sparse)
        back = mapfmt.parse_text(sparse)
        self.assertEqual(mapfmt.serialize_text(back), mapfmt.serialize_text(self.room))

    def test_stats_doors(self):
        info = mapfmt.stats(self.room)
        self.assertEqual(info["counts"]["D"], 4)
        self.assertEqual(info["counts"]["B"], 299)
        self.assertEqual([door["dir"] for door in info["doors"]], ["N", "W", "E", "S"])
        self.assertEqual(len(info["sha256"]), 64)

    # ---- C ------------------------------------------------------------

    def test_c_roundtrip(self):
        text = mapfmt.emit_c(self.room)
        self.assertIn("room_map_shared", text)
        self.assertIn("room_rot_shared", text)
        self.assertIn("_Static_assert", text)
        back = mapfmt.parse_c(text)
        self.assertEqual(mapfmt.serialize_text(back), self.clean_text)

    def test_c_import_from_game_world(self):
        if not GAME_WORLD.is_file():
            self.skipTest("game_world.c 不存在")
        back = mapfmt.parse_c(GAME_WORLD.read_text(encoding="utf-8"))
        self.assertEqual(mapfmt.serialize_text(back), self.clean_text)

    def test_parse_any(self):
        self.assertEqual(mapfmt.parse_any(self.text)[1], "text")
        self.assertEqual(mapfmt.parse_any(mapfmt.emit_c(self.room))[1], "c")

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
        lines[1] = lines[1][:-1]
        with self.assertRaises(mapfmt.MapError):
            mapfmt.parse_text("\n".join(lines))

    def test_bad_char(self):
        lines = mapfmt.serialize_text(self.room).split("\n")
        lines[1] = "Q" + lines[1][1:]
        with self.assertRaises(mapfmt.MapError):
            mapfmt.parse_text("\n".join(lines))

    def test_missing_rotation_rows(self):
        lines = mapfmt.serialize_text(self.room).split("\n")
        cut = lines[: mapfmt.ROWS + 2]
        with self.assertRaises(mapfmt.MapError):
            mapfmt.parse_text("\n".join(cut))

    def test_legacy_v1(self):
        body = "\n".join("".join(row) for row in self.room.rows)
        back = mapfmt.parse_text(body)
        self.assertTrue(all(value == 0 for row in back.rot for value in row))


if __name__ == "__main__":
    unittest.main(verbosity=2)
