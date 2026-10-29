"""Fixed map -> compact layout -> generated shared geometry -> runtime pool."""
import contextlib
import io
import json
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen
from unittest.mock import patch

import mapfmt
import serve


class LayoutPoolTest(unittest.TestCase):
    def shared_terrain_rows(self, source, symbol):
        tiles_block = re.search(r"LT_rows\[\d+\]\[LAYOUT_MAX_COLS \+ 1\] = \{(.*?)\n\};", source, re.S)
        self.assertIsNotNone(tiles_block)
        tiles = re.findall(r'^    "([.BWDCHXYZKSwcxyz]+)",$', tiles_block.group(1), re.M)
        rotation_block = re.search(r"RT_rows\[\d+\]\[LAYOUT_ROT_COLS\] = \{(.*?)\n\};", source, re.S)
        self.assertIsNotNone(rotation_block)
        rotations = [[int(v.strip()) for v in line.split(",") if v.strip()]
                     for line in re.findall(r"\{([^{}]+)\}", rotation_block.group(1))]
        def indexes(prefix):
            block = re.search(prefix + re.escape(symbol) + r"\[\d+\] = \{([^{}]+)\};", source)
            self.assertIsNotNone(block)
            return [int(v.strip()) for v in block.group(1).split(",") if v.strip()]
        return [tiles[i] for i in indexes("LTI_")], [rotations[i] for i in indexes("RTI_")]

    def test_authored_2x2_keeps_eight_doors_and_corner_rotations(self):
        frames = mapfmt.parse_world_frames(
            (serve.GAME_DIRS[0] / "game_world.c").read_text(encoding="utf-8"))
        room = mapfmt.pool_map("2x2")
        with self.assertRaises(mapfmt.MapError):
            mapfmt.layout_map_index(room, frames)
        header, source, _ = mapfmt.emit_layout_data([("room_2x2", room)], frames)
        self.assertIn("extern const layout_terrain_t T_room_2x2;", header)
        self.assertIn('&T_room_2x2 }', source)
        rows, packed = self.shared_terrain_rows(source, "room_2x2")
        self.assertEqual(rows, ["".join(row) for row in room.rows])
        self.assertEqual(sum(row.count("D") for row in rows), 8)
        for y in range(room.height):
            for x in range(room.cols):
                self.assertEqual((packed[y][x//4] >> ((x%4)*2)) & 3, room.rot[y][x])

    def test_matching_frame_still_avoids_a_duplicate_terrain_table(self):
        frames = mapfmt.parse_world_frames(
            (serve.GAME_DIRS[0] / "game_world.c").read_text(encoding="utf-8"))
        room = mapfmt.pool_map("1x1")
        self.assertEqual(mapfmt.layout_map_index(room, frames), 0)
        _, source, _ = mapfmt.emit_layout_data([("small", room)], frames)
        self.assertNotIn("LT_small", source)

    def test_shared_rows_preserve_all_authored_maps(self):
        frames = mapfmt.parse_world_frames(
            (serve.GAME_DIRS[0] / "game_world.c").read_text(encoding="utf-8"))
        rooms = [(name, mapfmt.pool_map(name)) for name in ("L", "hidden", "2x2")]
        _, source, _ = mapfmt.emit_layout_data(rooms, frames)
        for name, room in rooms:
            rows, rotations = self.shared_terrain_rows(source, mapfmt.layout_symbol(name))
            self.assertEqual(rows, ["".join(row) for row in room.rows])
            for y in range(room.height):
                for x in range(room.cols):
                    self.assertEqual((rotations[y][x//4] >> ((x%4)*2)) & 3,
                                     room.rot[y][x], (name, x, y))
        unique_tiles = re.search(r"LT_rows\[(\d+)\]", source)
        unique_rotations = re.search(r"RT_rows\[(\d+)\]", source)
        full_rows = sum(room.height for _, room in rooms)
        self.assertLess(int(unique_tiles.group(1)), full_rows // 2)
        self.assertLess(int(unique_rotations.group(1)), full_rows // 2)

    def test_http_save_load_import_and_reject_geometry_change(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(serve, "OUT", Path(temp)):
            server = serve.ThreadingHTTPServer(("127.0.0.1", 0), serve.Handler)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            base = "http://127.0.0.1:%d" % server.server_port
            def post(route, payload):
                with urlopen(Request(base + route, data=payload.encode("utf-8"), method="POST"), timeout=5) as response:
                    return json.load(response)
            try:
                empty = mapfmt.pool_map("1x1")
                empty.layout_id = 0
                (Path(temp) / "layouts").mkdir(exist_ok=True)
                (Path(temp) / "layouts/1x1_000.json").write_text(
                    mapfmt.serialize_pool_layout(empty), encoding="utf-8")
                with urlopen(base + "/api/load_layout?map=1x1&id=0", timeout=5) as response:
                    loaded_start = json.load(response)["layout"]
                self.assertEqual(loaded_start["name"], "1x1_000")
                self.assertEqual(loaded_start["objects"], [])
                room = mapfmt.pool_map("L")
                room.objects = [mapfmt.MapObject("slime", 25, 9)]
                result = post("/api/save_layout?map=L", mapfmt.serialize_json(room))
                self.assertEqual(result["map"], "L")
                self.assertEqual(json.loads(result["json"])["name"], "L_001")
                self.assertEqual(result["layout_id"], 1)
                with urlopen(base + "/api/load_layout?map=L&id=1", timeout=5) as response:
                    saved = json.dumps(json.load(response)["layout"], ensure_ascii=False, indent=2) + "\n"
                imported = post("/api/import", saved)
                self.assertEqual(imported["rows"], ["".join(row) for row in room.rows])
                self.assertEqual(imported["objects"][0]["x"], 25)
                self.assertEqual(imported["layout_id"], 1)
                result = post("/api/save_layout?map=L", mapfmt.serialize_json(room))
                self.assertEqual(result["file"], "L_002.json")
                small = mapfmt.pool_map("1x1")
                result = post("/api/save_layout?map=1x1", mapfmt.serialize_json(small))
                self.assertEqual(result["file"], "1x1_001.json")
                with self.assertRaises(HTTPError) as collision:
                    post("/api/save_layout?map=L&id=1&create=1", mapfmt.serialize_json(room))
                self.assertEqual(collision.exception.code, 409)
                result = post("/api/save_layout?map=L&id=1&create=0", mapfmt.serialize_json(room))
                self.assertEqual(result["layout_id"], 1)
                saved = result["json"]
                with urlopen(base + "/api/layout_catalog?map=L", timeout=5) as response:
                    catalog = json.load(response)
                self.assertEqual(catalog["next_id"], 3)
                self.assertEqual([item["id"] for item in catalog["layouts"]], [1, 2])
                with self.assertRaises(HTTPError) as special:
                    post("/api/save_layout?map=hidden", mapfmt.serialize_json(mapfmt.pool_map("hidden")))
                self.assertEqual(special.exception.code, 400)
                result = post("/api/save_map?map=L", mapfmt.serialize_json(room))
                self.assertEqual(json.loads(result["json"])["objects"], [])
                self.assertTrue((Path(temp) / "maps/L.json").exists())
                room.rows[0][0] = "W"
                with self.assertRaises(HTTPError) as failure:
                    post("/api/save_layout?map=L&id=1&create=0", mapfmt.serialize_json(room))
                self.assertEqual(failure.exception.code, 400)
                self.assertEqual((Path(temp) / "layouts/L_001.json").read_text(encoding="utf-8"), saved)
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=5)

    def test_compact_roundtrip_all_fixed_spaces(self):
        for map_id in mapfmt.POOL_MAP_IDS:
            room = mapfmt.pool_map(map_id)
            floor = next((x, y) for y, row in enumerate(room.rows)
                         for x, ch in enumerate(row) if ch in "BK")
            room.objects = [mapfmt.MapObject("slime", *floor, 7)]
            payload = mapfmt.serialize_pool_layout(room)
            data = json.loads(payload)
            self.assertEqual(data["map"], map_id)
            self.assertNotIn("tiles", data)
            self.assertNotIn("rotation", data)
            back = mapfmt.parse_json(payload)
            self.assertEqual(mapfmt.geometry_key(back), mapfmt.geometry_key(room))
            self.assertEqual(back.objects, room.objects)

    def test_geometry_edits_and_l_variants_rejected(self):
        room = mapfmt.pool_map("L")
        # 左下没有地板，其他三个象限有地板；以内容外接框划分。
        cells = [(x, y) for y, row in enumerate(room.rows)
                 for x, ch in enumerate(row) if ch != "."]
        mx = (min(x for x, y in cells) + max(x for x, y in cells)) / 2
        my = (min(y for x, y in cells) + max(y for x, y in cells)) / 2
        self.assertFalse(any(ch == "B" and x < mx and y > my
                             for y, row in enumerate(room.rows) for x, ch in enumerate(row)))
        for flip in (1, 2, 3):
            changed = mapfmt.pool_map("L")
            if flip & 1:
                changed.rows = [list(reversed(row)) for row in changed.rows]
            if flip & 2:
                changed.rows.reverse()
            with self.assertRaises(mapfmt.MapError):
                mapfmt.serialize_pool_layout(changed)
        room.rows[0][0] = "W"
        with self.assertRaises(mapfmt.MapError):
            mapfmt.serialize_pool_layout(room)

    def test_invalid_reference_and_unsupported_objects_rejected(self):
        for data in ({"format": "map_designer_layout", "version": 2, "map": "L"},
                     {"format": "map_designer_layout", "version": 1, "map": "../L"}):
            with self.assertRaises(mapfmt.MapError):
                mapfmt.parse_json(json.dumps(data))
        room = mapfmt.pool_map("1x1")
        room.objects = [mapfmt.MapObject("exit", 20, 8)]
        with self.assertRaises(mapfmt.MapError):
            mapfmt.serialize_pool_layout(room)

    def test_stable_layout_number_validation(self):
        room = mapfmt.pool_map("L")
        room.layout_id = 7
        data = json.loads(mapfmt.serialize_pool_layout(room))
        self.assertEqual(data["name"], "L_007")
        self.assertEqual(mapfmt.parse_json(json.dumps(data)).layout_id, 7)
        for invalid in (-1, 1000, True, "7"):
            data["layout_id"] = invalid
            with self.assertRaises(mapfmt.MapError):
                mapfmt.parse_json(json.dumps(data))
        empty = mapfmt.pool_map("1x1")
        empty.layout_id = 0
        empty_payload = json.loads(mapfmt.serialize_pool_layout(empty))
        self.assertEqual(empty_payload["name"], "1x1_000")
        self.assertEqual(mapfmt.parse_json(json.dumps(empty_payload)).layout_id, 0)
        bad_zero = mapfmt.pool_map("L")
        bad_zero.layout_id = 0
        with self.assertRaises(mapfmt.MapError):
            mapfmt.serialize_pool_layout(bad_zero)
        data["layout_id"] = 7
        data["name"] = "L_008"
        with self.assertRaises(mapfmt.MapError):
            mapfmt.parse_json(json.dumps(data))

    def test_cli_reads_compact_layout_and_preserves_archive(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            out, project, archive = [root / n for n in ("output", "project", "code")]
            for folder in (out, project, archive):
                folder.mkdir()
            (out / "variant.json").write_text(
                mapfmt.serialize_pool_layout(mapfmt.pool_map("L")), encoding="utf-8")
            (out / "layouts").mkdir()
            numbered = mapfmt.pool_map("L")
            numbered.layout_id = 7
            (out / "layouts/L_007.json").write_text(mapfmt.serialize_pool_layout(numbered), encoding="utf-8")
            (project / "game_world.c").write_text(
                (serve.GAME_DIRS[0] / "game_world.c").read_text(encoding="utf-8"), encoding="utf-8")
            args = serve.build_parser().parse_args(["--emit-layouts", "--apply"])
            with patch.object(serve, "OUT", out), patch.object(serve, "GAME_DIRS", (project, archive)), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(serve.cmd_emit_layouts(args), 0)
                self.assertIn("LAY_L_007", (project/"game_layout_data.h").read_text(encoding="utf-8"))
                args.check = True
                self.assertEqual(serve.cmd_emit_layouts(args), 0)
                (out / "layouts/L_008.json").write_text(mapfmt.serialize_pool_layout(numbered), encoding="utf-8")
                self.assertEqual(serve.cmd_emit_layouts(args), 1)
            self.assertFalse(list(archive.iterdir()))

    def test_generated_variants_share_geometry_and_runtime_selection(self):
        compiler = shutil.which("gcc")
        if not compiler:
            self.skipTest("host GCC unavailable")
        sw = serve.GAME_DIRS[0].parent
        frames = mapfmt.parse_world_frames((sw / "game/game_world.c").read_text(encoding="utf-8"))
        items = [(name, mapfmt.pool_map(kind)) for name, kind in (
            ("map_1x1_type0", "1x1"), ("map_L", "L"),
            ("map_hidden_normal", "hidden"), ("room_2x2", "2x2"))]
        empty = mapfmt.pool_map("1x1")
        empty.layout_id = 0
        items.append(("1x1_000", mapfmt.parse_json(mapfmt.serialize_pool_layout(empty))))
        for index in range(10):
            room = mapfmt.pool_map("L")
            room.layout_id = index + 1
            room.objects = [mapfmt.MapObject("slime", 25, 9, index + 1)]
            # Real compact import path, not manually constructing generated C.
            items.append(("L_%03d" % (index + 1),
                          mapfmt.parse_json(mapfmt.serialize_pool_layout(room))))
        room = mapfmt.pool_map("1x1")
        room.objects = [mapfmt.MapObject("stone", 12, 8)]
        items.append(("small_variant", room))
        header, source, _ = mapfmt.emit_layout_data(items, frames)
        self.assertEqual(source.count("static const char LT_rows["), 1)
        self.assertEqual(source.count("static const unsigned short LTI_L_001["), 1)
        self.assertNotIn("LTI_variant", source)
        self.assertIn("OP(25,9,OBJ_SLIME,10)", source)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "game_layout_data.h").write_text(header, encoding="utf-8")
            (root / "game_layout_data.c").write_text(source, encoding="utf-8")
            shutil.copyfile(sw / "game/game_layout.c", root / "game_layout.c")
            (root / "test.c").write_text(r'''
#include <assert.h>
#include <stdio.h>
#include "game.h"
#include "game_layout_data.h"
int main(void) {
    int i, j, selected, seen[LAYOUT_COUNT] = {0};
    const room_layout_t *base = game_layout(LAY_map_L);
    for (i = 0; i < 1000; ++i) {
        selected = game_layout_pick_like(LAY_map_L, ROOM_EMPTY, i);
        assert(game_layout(selected)->terrain == base->terrain);
        assert(game_layout(selected)->map == base->map);
        assert(selected == game_layout_pick_like(LAY_map_L, ROOM_EMPTY, i));
        seen[selected] = 1;
        j = game_layout_pick(ROOM_EMPTY, i);
        assert(game_layout(j)->terrain == game_layout(LAY_map_1x1_type0)->terrain);
        assert(game_layout(j)->map == game_layout(LAY_map_1x1_type0)->map);
    assert(game_layout_pick(ROOM_START, i) == LAY__1x1_000);
    }
    j = 0;
    for (i = 0; i < LAYOUT_COUNT; ++i) j += seen[i];
    assert(j == 10); /* Numbered layouts replace the reference placeholder. */
    assert(!seen[LAY_map_L]);
    assert(g_layout_ids[LAY_L_010] == 10);
    assert(game_layout(LAY_L_010)->objs[0].param == 10);
    assert(game_layout_pick(ROOM_BOSS, 7) == LAY_room_2x2);
    assert(game_layout_pick(ROOM_HIDDEN, 7) == LAY_map_hidden_normal);
    for (i = 0; i < 4; ++i) {
        int x, y;
        const obj_place_t *obj = game_layout(LAY_L_010)->objs;
        game_layout_obj_cell(base, obj, i, &x, &y);
        assert(x == ((i & 1) ? base->terrain->mirror_x - 25 : 25));
        assert(y == ((i & 2) ? base->terrain->mirror_y - 9 : 9));
        assert(game_layout_tile_at(base->terrain, x, y) == 'B' || i != 0);
    }
    puts("PASS: 11 L layouts share fixed geometry; deterministic shape pools and object params");
    return 0;
}
''', encoding="utf-8")
            includes = [sw / p for p in ("verification/hud_camera", "game", "src",
                        "../driver", "peripheral/CInstruction", "peripheral/APB0/gpu2d",
                        "peripheral/APB0/lineBuffer")]
            exe = root / "pool_test.exe"
            command = [compiler, "-std=gnu11", "-O2", *["-I" + str(p) for p in includes],
                       str(root / "test.c"), str(root / "game_layout.c"),
                       str(root / "game_layout_data.c"), "-o", str(exe)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
