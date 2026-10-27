"""Formal skins and one-cell H entrances through JSON -> frame -> C export."""
import contextlib
import io
import re
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import mapfmt
import serve

HERE = Path(__file__).resolve().parent
WORLD = serve.GAME_DIRS[0] / "game_world.c"


class FormalTilesTest(unittest.TestCase):
    def setUp(self):
        self.frames = mapfmt.parse_world_frames(WORLD.read_text(encoding="utf-8"))
        # A user's current output map may contain deliberate terrain edits.
        # Exercise the canonical frame without coupling tests to that document.
        f = self.frames[2]
        self.hidden = mapfmt.RoomMap(
            rows=[[mapfmt.frame_tile(f, x, y) for x in range(f["cols"])]
                  for y in range(f["rows"])],
            rot=[[mapfmt.frame_rot(f, x, y) for x in range(f["cols"])]
                 for y in range(f["rows"])], name="hidden")

    def test_single_cell_hidden_entrances_and_backing_walls(self):
        f = self.frames[2]
        for side in range(4):
            x, y, bx, by = mapfmt._door_cells(f, side)
            self.assertEqual(self.hidden.rows[y][x], "H")
            self.assertEqual(self.hidden.rows[by][bx], "w")
        self.assertFalse(mapfmt.errors_of(mapfmt.validate(self.hidden)))

    def test_hidden_skin_frame_matches_every_cell_and_rotation(self):
        detected = mapfmt.detect_frame(self.hidden)
        self.assertIsNotNone(detected)
        self.assertEqual(detected["corner_ch"], "c")
        for y in range(self.hidden.height):
            for x in range(self.hidden.cols):
                self.assertEqual(mapfmt.frame_tile(detected, x, y), self.hidden.rows[y][x])
                self.assertEqual(mapfmt.frame_rot(detected, x, y), self.hidden.rot[y][x])

    def test_normal_doors_keep_their_second_cell(self):
        room = mapfmt.make_template("template_1x1")
        f = mapfmt.detect_frame(room)
        for side in range(4):
            x, y, px, py = mapfmt._door_cells(f, side)
            self.assertEqual(room.rows[y][x], "D")
            self.assertEqual(room.rows[py][px], ".")

    def test_old_two_cell_hidden_anchor_is_rejected(self):
        room = self.hidden
        room.rows[2][20], room.rows[3][20] = "H", "w"
        self.assertTrue(any("紧贴内侧地板" in i.message
                            for i in mapfmt.errors_of(mapfmt.validate(room))))

    def test_hidden_objects_stay_on_separate_layer(self):
        room = self.hidden
        room.objects = [mapfmt.MapObject("stone", 12, 8)]
        saved = mapfmt.serialize_json(room)
        back = mapfmt.parse_json(saved)
        self.assertEqual(back.rows, room.rows)
        self.assertEqual(back.objects, room.objects)
        self.assertEqual(mapfmt.layout_map_index(back, self.frames), 2)
        _, source, _ = mapfmt.emit_layout_data([("hidden", back)], self.frames)
        self.assertIn("O(12,8,OBJ_STONE)", source)
        self.assertIn('{ "hidden", L_hidden, 1, 2, 0 }', source)
        self.assertNotIn("LT_hidden", source)

    def test_hidden_corner_counts_and_numeric_roundtrip(self):
        counts = mapfmt.stats(self.hidden)["counts"]
        self.assertEqual(counts["H"], 4)
        self.assertGreater(sum(counts[c] for c in "cxyz"), 0)
        back = mapfmt.parse_json(mapfmt.serialize_json(self.hidden))
        self.assertEqual(back.rows, self.hidden.rows)
        self.assertEqual(back.rot, self.hidden.rot)

    def test_formal_asset_roles_have_no_missing_files(self):
        resolved = serve.resolve_assets()
        self.assertEqual(resolved["missing"], [])
        for role in ("wood", "wall", "corner", "hidden_floor", "hidden_wall",
                     "hidden_corner", "door", "hide"):
            self.assertNotIn("minecraft", resolved["roles"][role]["path"])

    def test_packed_rotations_roundtrip_and_use_only_actual_rows(self):
        for room in (self.hidden, mapfmt.make_template("template_L")):
            for y in range(room.height):
                for x in range(room.cols):
                    if room.rows[y][x] not in "DH":
                        room.rot[y][x] = (x + y) & 3
            _, source, _ = mapfmt.emit_layout_data([("packed", room)], self.frames)
            match = re.search(r"RT_packed\[(\d+)\]\[LAYOUT_ROT_COLS\] = \{(.*?)\n\};",
                              source, re.S)
            self.assertIsNotNone(match)
            self.assertEqual(int(match.group(1)), room.height)
            packed = [[int(v.strip()) for v in line.split(",") if v.strip()]
                      for line in re.findall(r"\{([^{}]+)\}", match.group(2))]
            self.assertEqual(len(packed), room.height)
            for y, row in enumerate(packed):
                self.assertEqual(len(row), (room.cols + 3) // 4)
                for x in range(room.cols):
                    self.assertEqual((row[x // 4] >> ((x & 3) * 2)) & 3, room.rot[y][x])
                if room.cols & 3:
                    self.assertEqual(row[-1] >> ((room.cols & 3) * 2), 0)

    def test_project_export_does_not_write_archive(self):
        with tempfile.TemporaryDirectory() as temp:
            project, archive = Path(temp)/"project", Path(temp)/"code"
            project.mkdir()
            archive.mkdir()
            (project/"game_world.c").write_text(WORLD.read_text(encoding="utf-8"), encoding="utf-8")
            sentinel = archive/"game_layout_data.c"
            sentinel.write_text("archive unchanged", encoding="utf-8")
            args = serve.build_parser().parse_args(["--emit-layouts", "--apply"])
            with patch.object(serve, "GAME_DIRS", (project, archive)), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(serve.cmd_emit_layouts(args), 0)
                args.check = True
                self.assertEqual(serve.cmd_emit_layouts(args), 0)
                (project/"game_layout_data.h").unlink()
                self.assertEqual(serve.cmd_emit_layouts(args), 1)
            self.assertEqual(sentinel.read_text(encoding="utf-8"), "archive unchanged")
            self.assertFalse((archive/"game_layout_data.h").exists())


if __name__ == "__main__":
    unittest.main()
