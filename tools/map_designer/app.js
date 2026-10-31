"use strict";

/* ==========================================================================
   MAP DESIGNER · 房间设计器
   --------------------------------------------------------------------------
   · 画布尺寸自由：默认 40×23，上限 45×35（= game.h 的 MAP_MAX_COLS/ROWS）
   · 32 px/格；尺寸从地图本身读，换尺寸 = 清空重画（不做内容迁移）
   · 地形层：. B W D C/X/Y/Z；对象层：slime/bug/aimmer/web/stone/box/crate/exit
   · 存档只有一份：output/<name>.json；生成 C 是手动步骤，写 output/game_world_map.c
   · 校验 / C 生成 / 解析都在 mapfmt.py（服务端），前端只负责画和发请求
   ========================================================================== */
(function () {
  var TILE = 32;
  var DEFAULT_COLS = 40;      /* 普通房：40×23 格 = 1280×736 px */
  var DEFAULT_ROWS = 23;
  var MAX_COLS = 45;          /* = game.h 的 MAP_MAX_COLS（Boss 房 45×35） */
  var MAX_ROWS = 35;
  var MIN_COLS = 8;
  var MIN_ROWS = 8;
  var REF_W = 1280;           /* 720p 参考框：只是参考，不参与校验 */
  var REF_H = 720;
  /* 当前画布像素尺寸，跟着 S.cols / S.rows 走（syncCanvas 里更新） */
  var W = DEFAULT_COLS * TILE;
  var H = DEFAULT_ROWS * TILE;
  /* 素材路径不写死：启动时拉 /api/assets，把 res/pic 里现有的图对到笔刷上。
     对不上的笔刷退回色块 + 名字，不会白屏。 */
  var LEGACY_TYPE_WORDS = ["start", "empty", "item", "shop", "boss"];
  var CORNER_CODES = ["C", "X", "Y", "Z"];
  var CORNER_NAMES = ["C · 原样", "X · 水平翻", "Y · 垂直翻", "Z · 双向翻"];
  var HIDDEN_CORNER_CODES = ["c", "x", "y", "z"];
  var HIDDEN_CORNER_NAMES = ["c · 原样", "x · 水平翻", "y · 垂直翻", "z · 双向翻"];
  function isCornerBrush(code) { return code === "C" || code === "c"; }
  function cornerCodesFor(code) { return code === "c" ? HIDDEN_CORNER_CODES : CORNER_CODES; }
  function cornerNamesFor(code) { return code === "c" ? HIDDEN_CORNER_NAMES : CORNER_NAMES; }
  var DIRS = ["N", "E", "S", "W"];
  var CODES = [".", "B", "W", "D", "C", "X", "Y", "Z", "H", "K", "S", "w", "c", "x", "y", "z"];
  var DOOR_CODES = { D: "door", H: "hide" };     /* 码 → 素材 id（H = 隐藏门） */
  function isDoorCode(code) { return Object.prototype.hasOwnProperty.call(DOOR_CODES, code); }
  var MAP_HEADER = "# MAP_DESIGNER v3";
  var PAGE_VERSION = "2026-10-29a";   /* 普通房型、稳定编号与地图/布局两种模式 */

  var TILES = [
    { id: "empty", code: ".", name: "空白", color: "#090e14" },
    { id: "wood", code: "B", name: "木地板", image: "" },
    { id: "wall", code: "W", name: "木墙", image: "" },
    { id: "door", code: "D", name: "门（2 格）", image: "" },
    { id: "corner", code: "C", name: "角块", image: "" },
    /*  ★ 新笔刷一律加在末尾：前面的顺序是页面/肌肉记忆的一部分，插队会让"门"变成别的 */
    { id: "hide", code: "H", name: "隐藏门（1 格）", image: "" },
    { id: "hidden_floor", code: "K", name: "隐藏房地板", image: "" },
    { id: "hidden_wall", code: "w", name: "隐藏房墙", image: "" },
    { id: "hidden_corner", code: "c", name: "隐藏房角块", image: "" }
  ];

  /* id 必须和 mapfmt.py / game.h 的 MAP_OBJ_* 一致。 */
  var OBJECT_DEFS = [
    { id: "slime", code: 0, name: "史莱姆", image: "", layer: "actor" },
    { id: "bug", code: 1, name: "甲虫", image: "", layer: "actor" },
    { id: "web", code: 2, name: "蜘蛛网", image: "", layer: "scene" },
    { id: "stone", code: 3, name: "石头", image: "", layer: "actor" },
    { id: "box", code: 4, name: "木箱", image: "", layer: "actor" },
    { id: "crate", code: 5, name: "板条箱", image: "", layer: "actor" },
    { id: "exit", code: 6, name: "出口", image: "", layer: "scene" },
    { id: "aimmer", code: 7, name: "瞄准怪", image: "", layer: "actor" }
  ];

  var OBJECT_BY_ID = {};
  var OBJECT_ORDER = {};
  OBJECT_DEFS.forEach(function (def, index) {
    OBJECT_BY_ID[def.id] = def;
    OBJECT_ORDER[def.id] = index;
  });

  function $(id) { return document.getElementById(id); }

  var canvas = $("map");
  var ctx = canvas.getContext("2d");
  var preview = $("preview");
  var pctx = preview.getContext("2d");
  var paletteEl = $("palette");
  var objectPaletteEl = $("objectPalette");
  var cornerRow = $("cornerRow");
  var angleRow = $("angleRow");
  var countsEl = $("counts");
  var issuesEl = $("issues");
  var issueBadge = $("issueBadge");
  var issueSummary = $("issueSummary");
  var statusEl = $("status");
  var cursorEl = $("cursor");
  var dirtyEl = $("dirty");
  var mapTextEl = $("mapText");
  var importTextEl = $("importText");
  var fileNameEl = $("fileName");
  var cFileNameEl = $("cFileName");
  var sizeColsEl = $("sizeCols");
  var sizeRowsEl = $("sizeRows");
  var templateListEl = $("templateList");
  var templateNameEl = $("templateName");
  var templateLabelEl = $("templateLabel");
  var fileListEl = $("fileList");
  var zoomSlider = $("zoomSlider");
  var zoomReadout = $("zoomReadout");
  var canvasWrap = $("canvasWrap");
  var mapStage = $("mapStage");
  var images = {};

  var S = {
    mode: "map",
    shape: "1x1",
    layoutId: null,
    candidateId: 1,
    cols: DEFAULT_COLS,
    rows: DEFAULT_ROWS,
    map: grid(".", DEFAULT_COLS, DEFAULT_ROWS),
    rot: grid(0, DEFAULT_COLS, DEFAULT_ROWS),
    objects: [],
    showRef: true,
    layer: "tile",
    brush: "W",
    corner: 0,
    angle: 0,
    objBrush: "slime",
    zoom: 1,
    fwView: false,
    history: [],
    future: [],
    pending: null,
    beforeText: "",
    savedText: "",
    dirty: false,
    file: "room_map.json",
    cfile: "game_world_map.c",
    templates: [],
    templateId: "",
    lastC: "",
    issueCells: {},
    issues: [],
    flash: null,
    hover: null,
    drawing: false,
    erase: false,
    loading: false,
    lastCell: null,
    served: /^https?:$/.test(location.protocol)
  };

  /* ---------------------------------------------------------------- 工具 */

  function grid(value, cols, rows) {
    cols = cols === undefined ? S.cols : cols;
    rows = rows === undefined ? S.rows : rows;
    var out = [];
    for (var y = 0; y < rows; y++) {
      var row = [];
      for (var x = 0; x < cols; x++) row.push(value);
      out.push(row);
    }
    return out;
  }

  /* 画布尺寸检查：越界直接抛给调用方（导入 / 换尺寸都走这里） */
  function checkCanvas(cols, rows) {
    if (!(cols >= MIN_COLS && cols <= MAX_COLS)) {
      throw new Error("画布宽度 " + cols + " 格超出范围：" + MIN_COLS + " ~ " + MAX_COLS);
    }
    if (!(rows >= MIN_ROWS && rows <= MAX_ROWS)) {
      throw new Error("画布高度 " + rows + " 格超出范围：" + MIN_ROWS + " ~ " + MAX_ROWS);
    }
  }

  /* 竞技场 = 地板 B 的包围盒；和 mapfmt.py 的 arena_rect 同一套规则 */
  function arenaRect() {
    var x0 = S.cols, y0 = S.rows, x1 = -1, y1 = -1;
    for (var y = 0; y < S.rows; y++) {
      for (var x = 0; x < S.cols; x++) {
        if ("BKS".indexOf(S.map[y][x]) < 0) continue;
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (x > x1) x1 = x;
        if (y > y1) y1 = y;
      }
    }
    if (x1 < 0) return { x0: 0, y0: 0, x1: S.cols - 1, y1: S.rows - 1, empty: true };
    return { x0: x0, y0: y0, x1: x1, y1: y1, empty: false };
  }

  /* 四扇门的建议槽位：各边墙带正中（和 mapfmt.door_slots 一致） */
  function doorSlots() {
    var a = arenaRect();
    var one = S.brush === "H";
    var cx = Math.floor((a.x0 + a.x1) / 2);
    var cy = Math.floor((a.y0 + a.y1) / 2);
    return [
      [cx, a.y0 - (one ? 1 : 2), 0],
      [a.x1 + 1, cy, 1],
      [cx, a.y1 + 1, 2],
      [a.x0 - (one ? 1 : 2), cy, 3]
    ];
  }

  function cloneGrid(g) {
    var out = [];
    for (var y = 0; y < g.length; y++) out.push(g[y].slice());
    return out;
  }

  function cloneObjects(list) {
    return (list || []).map(function (obj) {
      return { kind: obj.kind, x: obj.x | 0, y: obj.y | 0, arg: obj.arg | 0 };
    });
  }

  function inBounds(x, y, cols, rows) {
    cols = cols === undefined ? S.cols : cols;
    rows = rows === undefined ? S.rows : rows;
    return x >= 0 && x < cols && y >= 0 && y < rows;
  }

  function doorPair(x, y, angle) { return (angle & 1) ? [x + 1, y] : [x, y + 1]; }

  function tileOf(code) {
    if (code === "S") code = "B";   /* 石头地板 S 已废弃（旧图按木地板显示） */
    for (var i = 0; i < TILES.length; i++) if (TILES[i].code === code) return TILES[i];
    if (CORNER_CODES.indexOf(code) >= 0) {
      /*  X/Y/Z 共用角块那张图 —— 按 id 找，别按下标（表里插一个笔刷就会错位）  */
      for (var j = 0; j < TILES.length; j++) if (TILES[j].id === "corner") return TILES[j];
    }
    if (HIDDEN_CORNER_CODES.indexOf(code) >= 0) {
      for (var j2 = 0; j2 < TILES.length; j2++) if (TILES[j2].id === "hidden_corner") return TILES[j2];
    }
    return TILES[0];
  }

  function objectDef(kind) { return OBJECT_BY_ID[kind] || null; }

  function currentCode() { return isCornerBrush(S.brush) ? cornerCodesFor(S.brush)[S.corner] : S.brush; }

  function cloneState() {
    return {
      mode: S.mode, shape: S.shape, layoutId: S.layoutId, candidateId: S.candidateId,
      cols: S.cols,
      rows: S.rows,
      map: cloneGrid(S.map),
      rot: cloneGrid(S.rot),
      objects: cloneObjects(S.objects)
    };
  }

  function applyState(rows, rot, objects) {
    S.map = rows.map(function (row) { return row.split(""); });
    S.cols = S.map.length ? S.map[0].length : DEFAULT_COLS;
    S.rows = S.map.length || DEFAULT_ROWS;
    S.rot = [];
    for (var y = 0; y < S.rows; y++) {
      var src = rot && rot[y] ? rot[y] : [];
      var line = [];
      for (var x = 0; x < S.cols; x++) line.push(src[x] === undefined ? 0 : (src[x] & 3));
      S.rot.push(line);
    }
    S.objects = cloneObjects(objects);
    syncCanvas();
    updateSizeUi();
    S.hover = null;
    S.flash = null;
  }

  /* 尺寸变了：画布像素、CSS 尺寸、控件读数一起跟上 */
  function syncCanvas() {
    W = S.cols * TILE;
    H = S.rows * TILE;
    canvas.width = W;
    canvas.height = H;
    setZoom(S.zoom);
  }

  function updateSizeUi() {
    if (sizeColsEl) sizeColsEl.value = String(S.cols);
    if (sizeRowsEl) sizeRowsEl.value = String(S.rows);
    var a = arenaRect();
    var hint = $("sizeHint");
    if (hint) {
      hint.textContent = S.cols + " × " + S.rows + " 格 = " + (S.cols * TILE) + " × " + (S.rows * TILE) +
        " px（" + MIN_COLS + "×" + MIN_ROWS + " ~ " + MAX_COLS + "×" + MAX_ROWS + "；换尺寸保留重叠部分）";
    }
    var sub = $("subTitle");
    if (sub) {
      sub.textContent = S.cols + " × " + S.rows + " · " + (S.cols * TILE) + " × " + (S.rows * TILE) +
        " px · 32 px/格 · 地形 + 对象";
    }
    var info = $("constraintInfo");
    if (info) {
      info.innerHTML =
        "<strong>画布</strong> " + S.cols + " × " + S.rows + " 格 = " + (S.cols * TILE) + " × " + (S.rows * TILE) + " px<br>" +
        "<strong>竞技场</strong> " + a.x0 + "," + a.y0 + " → " + a.x1 + "," + a.y1 +
        (a.empty ? "（还没有地板 B，竞技场是从 B 的包围盒推的）" : "") + "<br>" +
        "<strong>墙带</strong> 2 格厚；普通门 2 格，隐藏入口 1 格<br>" +
        "<strong>门槽位</strong> " + doorSlots().map(function (slot) {
          return DIRS[slot[2]] + "(" + slot[0] + "," + slot[1] + ")";
        }).join(" ") + "<br>" +
        "<strong>字符</strong> <code>.</code> 空 <code>B</code> 地板 <code>W</code> 墙 <code>D</code> 门 <code>C/X/Y/Z</code> 角块";
    }
  }

  /* 换尺寸：保留左上角重叠的部分（变小时裁掉，变大时补空白），整个操作一步撤销。
     想从干净骨架开始就用「小房间 / 大房间」模板。 */
  function resizeCanvas(cols, rows) {
    if (S.mode === "layout") { setStatus("布局模式锁定房间空间，请切换地图模式", false); return; }
    cols = Math.round(Number(cols));
    rows = Math.round(Number(rows));
    if (!isFinite(cols) || !isFinite(rows)) { setStatus("尺寸要是整数", false); return; }
    try {
      checkCanvas(cols, rows);
    } catch (e) {
      setStatus(e.message, false);
      return;
    }
    if (cols === S.cols && rows === S.rows) { setStatus("尺寸没变：" + cols + " × " + rows); return; }
    pushHistory();
    var oldMap = S.map, oldRot = S.rot, oldObjects = S.objects;
    var keepW = Math.min(S.cols, cols), keepH = Math.min(S.rows, rows);
    S.cols = cols;
    S.rows = rows;
    S.map = grid(".", cols, rows);
    S.rot = grid(0, cols, rows);
    for (var y = 0; y < keepH; y++) {
      for (var x = 0; x < keepW; x++) {
        S.map[y][x] = oldMap[y][x];
        S.rot[y][x] = oldRot[y][x];
      }
    }
    S.objects = oldObjects.filter(function (obj) { return obj.x < cols && obj.y < rows; });
    syncCanvas();
    updateSizeUi();
    changed();
    var dropped = oldObjects.length - S.objects.length;
    setStatus("画布改成 " + cols + " × " + rows + "（保留重叠部分" +
      (dropped > 0 ? "，裁掉 " + dropped + " 个越界对象" : "") + "；Ctrl+Z 可撤销）");
  }

  function sortObjects(list) {
    return (list || []).slice().sort(function (a, b) {
      return (a.y - b.y) || (a.x - b.x) || (OBJECT_ORDER[a.kind] - OBJECT_ORDER[b.kind]);
    });
  }

  function serialize() {
    var lines = [MAP_HEADER];
    for (var y = 0; y < S.rows; y++) {
      lines.push(S.map[y].join(""));
    }

    var objects = sortObjects(S.objects);
    if (objects.length) {
      lines.push("# OBJECTS");
      for (var i = 0; i < objects.length; i++) {
        var obj = objects[i];
        if (obj.arg) lines.push(obj.kind + " " + obj.x + "," + obj.y + "," + obj.arg);
        else lines.push(obj.kind + " " + obj.x + "," + obj.y);
      }
    }

    lines.push("# ROTATION");
    for (var ry = 0; ry < S.rows; ry++) {
      var row = [];
      for (var rx = 0; rx < S.cols; rx++) row.push(String(S.rot[ry][rx] & 3));
      lines.push(row.join(""));
    }
    return lines.join("\n") + "\n";
  }

  var TILE_INDEX = { ".": 0, "B": 1, "W": 2, "D": 3, "C": 4, "H": 5, "K": 6, "S": 7,
    "X": 8, "Y": 9, "Z": 10, "w": 11, "c": 12, "x": 13, "y": 14, "z": 15 };
  var TILE_CHAR = {};
  Object.keys(TILE_INDEX).forEach(function (ch) { TILE_CHAR[TILE_INDEX[ch]] = ch; });

  function serializeJson() {
    var objects = sortObjects(S.objects);
    var lines = ["{"];
    lines.push('  "format": "map_designer",');
    lines.push('  "version": 4,');
    lines.push('  "name": ' + JSON.stringify(baseName()) + ',');
    lines.push('  "size": {"cols": ' + S.cols + ', "rows": ' + S.rows + ', "tile": ' + TILE + '},');
    lines.push('  "tile_encoding": "index",');
    lines.push('  "tiles": [');
    for (var y = 0; y < S.rows; y++) {
      var values = S.map[y].map(function (ch) { return TILE_INDEX[ch]; }).join(", ");
      lines.push("    [" + values + "]" + (y + 1 < S.rows ? "," : ""));
    }
    lines.push("  ],");
    lines.push('  "rotation": [');
    for (var ry = 0; ry < S.rows; ry++) {
      var rv = S.rot[ry].slice(0, S.cols).join(", ");
      lines.push("    [" + rv + "]" + (ry + 1 < S.rows ? "," : ""));
    }
    lines.push("  ],");
    if (objects.length) {
      lines.push('  "objects": [');
      for (var oi = 0; oi < objects.length; oi++) {
        var obj = objects[oi];
        lines.push('    {"kind": ' + JSON.stringify(obj.kind) +
          ', "x": ' + obj.x + ', "y": ' + obj.y + ', "arg": ' + obj.arg + '}' +
          (oi + 1 < objects.length ? "," : ""));
      }
      lines.push("  ]");
    } else {
      lines.push('  "objects": []');
    }
    lines.push("}");
    return lines.join("\n") + "\n";
  }

  function normalizeKind(raw) {
    var kind = String(raw).trim().toLowerCase().replace(/-/g, "_");
    var aliases = {
      mob_slime: "slime", "mob.slime": "slime", enemy_slime: "slime",
      mob_bug: "bug", "mob.bug": "bug", enemy_bug: "bug",
      mob_aimmer: "aimmer", "mob.aimmer": "aimmer", enemy_aimmer: "aimmer", aimer: "aimmer",
      spider_web: "web", rock: "stone", crate_box: "box", eixt: "exit"
    };
    return aliases[kind] || kind;
  }

  function parseObjectLine(line, cols, rows) {
    var parts = line.trim().split(/[\s,]+/).filter(function (part) { return !!part; });
    if (parts.length < 3) throw new Error("对象要写成 kind x,y [arg]：" + line);
    if (parts.length > 4) throw new Error("对象只有 4 列（kind x y arg）：" + line);
    var kind = normalizeKind(parts[0]);
    if (!OBJECT_BY_ID[kind]) throw new Error("未知对象类型：" + parts[0]);
    var x = Number(parts[1]);
    var y = Number(parts[2]);
    var arg = parts.length > 3 ? Number(parts[3]) : 0;
    if (!isFinite(x) || !isFinite(y) || !isFinite(arg)) throw new Error("对象坐标 / 参数不是整数：" + line);
    if (!inBounds(x, y, cols, rows)) throw new Error("对象坐标越界：" + line);
    if (arg < 0 || arg > 255) throw new Error("对象参数必须在 0..255：" + line);
    return { kind: kind, x: x, y: y, arg: arg };
  }

  function parseJsonLocal(raw) {
    var text = String(raw).replace(/^\uFEFF/, "");
    var data;
    try {
      data = JSON.parse(text);
    } catch (e) {
      throw new Error("JSON 解析失败：" + e.message);
    }
    if (data && data.format === "map_designer_layout") {
      throw new Error("布局池文件需要通过 serve.py 打开，以读取对应的固定房型");
    }
    if (!data || !Array.isArray(data.tiles) || !data.tiles.length) throw new Error("JSON 缺少 tiles 数组");
    var height = data.tiles.length;
    var numericTiles = Array.isArray(data.tiles[0]);
    var cols = numericTiles ? data.tiles[0].length : (typeof data.tiles[0] === "string" ? data.tiles[0].length : 0);
    if (!cols) throw new Error("JSON tiles 第 1 行既不是数字数组也不是字符串");
    checkCanvas(cols, height);
    if (data.size) {
      if (data.size.cols && Number(data.size.cols) !== cols) throw new Error("JSON cols " + data.size.cols + " 和实际 " + cols + " 不一致");
      if (data.size.rows && Number(data.size.rows) !== height) throw new Error("JSON rows " + data.size.rows + " 和实际 " + height + " 不一致");
    }
    var rows = [];
    for (var y = 0; y < height; y++) {
      var line = data.tiles[y];
      if (Array.isArray(line)) {
        if (line.length !== cols) throw new Error("JSON 第 " + (y + 1) + " 行不是 " + cols + " 个数字");
        var textRow = "";
        for (var nx = 0; nx < cols; nx++) {
          var ch = TILE_CHAR[Number(line[nx])];
          if (!ch) throw new Error("JSON 第 " + (y + 1) + " 行第 " + (nx + 1) + " 列编号越界：" + line[nx]);
          textRow += ch;
        }
        rows.push(textRow);
      } else {
        if (typeof line !== "string" || line.length !== cols) throw new Error("JSON 第 " + (y + 1) + " 行不是 " + cols + " 字符串");
        for (var x = 0; x < cols; x++) {
          if (".BWDHKCXYZS".indexOf(line.charAt(x)) < 0) throw new Error("JSON 第 " + (y + 1) + " 行第 " + (x + 1) + " 列有非法字符");
        }
        rows.push(line);
      }
    }
    var rot = grid(0, cols, height);
    if (data.rotation) {
      if (!Array.isArray(data.rotation) || data.rotation.length !== height) throw new Error("JSON rotation 需要 " + height + " 行");
      for (var ry = 0; ry < height; ry++) {
        if (!Array.isArray(data.rotation[ry]) || data.rotation[ry].length !== cols) throw new Error("JSON rotation 第 " + (ry + 1) + " 行长度不对");
        for (var rx = 0; rx < cols; rx++) {
          var value = Number(data.rotation[ry][rx]);
          if (!(value >= 0 && value <= 3)) throw new Error("JSON rotation 越界");
          rot[ry][rx] = value;
        }
      }
    }
    var objects = [];
    (data.objects || []).forEach(function (item) {
      if (!item || typeof item !== "object") throw new Error("JSON objects 里的项不是对象");
      var kind = normalizeKind(item.kind);
      if (!OBJECT_BY_ID[kind]) throw new Error("未知对象类型：" + item.kind);
      var x = Number(item.x), y = Number(item.y), arg = Number(item.arg || 0);
      if (!inBounds(x, y, cols, height)) throw new Error("JSON 对象坐标越界：" + item.kind + " " + item.x + "," + item.y);
      if (!(arg >= 0 && arg <= 255)) throw new Error("JSON 对象参数越界");
      objects.push({ kind: kind, x: x, y: y, arg: arg });
    });
    return { rows: rows, rot: rot, objects: objects };
  }

  function parseLocal(raw) {
    var text = String(raw).replace(/^\uFEFF/, "").replace(/\r\n/g, "\n").replace(/\r/g, "\n");
    if (text.trim().charAt(0) === "{") return parseJsonLocal(text);
    var lines = text.split("\n");
    var sections = [];
    var tag = "map";
    var args = "";
    var body = [];

    for (var i = 0; i < lines.length; i++) {
      var line = lines[i].replace(/\s+$/, "");
      var stripped = line.trim();
      if (!stripped) continue;
      if (stripped.charAt(0) === "#") {
        var match = stripped.match(/^#\s*(ROTATION|OBJECTS|ENTITIES|TEMPLATE|ROOM_TYPE)\b(.*)$/i);
        if (match) {
          sections.push({ tag: tag, args: args, body: body });
          tag = match[1].toLowerCase();
          args = match[2].trim();
          body = [];
        }
        continue;
      }
      body.push(line);
    }
    sections.push({ tag: tag, args: args, body: body });

    var mapLines = [];
    var objectLines = [];
    var rotLines = [];
    var rotArgs = "";
    var sawRotation = false;

    sections.forEach(function (section) {
      if (section.tag === "rotation") {
        sawRotation = true;
        rotArgs = section.args.toLowerCase();
        rotLines = rotLines.concat(section.body);
        return;
      }
      if (section.tag === "objects" || section.tag === "entities") {
        objectLines = objectLines.concat(section.body);
        return;
      }
      if (section.tag === "template" || section.tag === "room_type" || section.tag === "name") {
        /* 旧文件的 '# TEMPLATE empty' 只是历史遗留：忽略，不改变图 */
        var rest = section.body.slice();
        if (rest.length && LEGACY_TYPE_WORDS.indexOf(rest[0].trim().toLowerCase()) >= 0) rest.shift();
        mapLines = mapLines.concat(rest);
        return;
      }
      mapLines = mapLines.concat(section.body);
    });

    if (!mapLines.length) throw new Error("地图是空的：至少要 " + MIN_ROWS + " 行");
    var height = mapLines.length;
    var cols = mapLines[0].length;
    checkCanvas(cols, height);
    var rows = [];
    for (var y = 0; y < height; y++) {
      if (mapLines[y].length !== cols) throw new Error("第 " + (y + 1) + " 行长度 " + mapLines[y].length + "，和第一行的 " + cols + " 不一致");
      for (var x = 0; x < cols; x++) {
        if (".BWDHKCXYZS".indexOf(mapLines[y].charAt(x)) < 0) throw new Error("第 " + (y + 1) + " 行第 " + (x + 1) + " 列有非法字符");
      }
      rows.push(mapLines[y]);
    }

    var rot = grid(0, cols, height);
    if (sawRotation) {
      var cleanRot = rotLines.map(function (line) { return line.trim(); }).filter(function (line) { return !!line && line.charAt(0) !== "#"; });
      if (!cleanRot.length && rotArgs !== "sparse") throw new Error("# ROTATION 后面没有内容");
      var sparse = rotArgs === "sparse" || /[\s,]/.test(cleanRot[0] || "");
      if (sparse) {
        for (var s = 0; s < cleanRot.length; s++) {
          var parts = cleanRot[s].split(/[\s,]+/);
          if (parts.length !== 3) throw new Error("稀疏旋转要写成 x,y,r");
          var sx = Number(parts[0]), sy = Number(parts[1]), sr = Number(parts[2]);
          if (!(sx >= 0 && sx < cols && sy >= 0 && sy < height && sr >= 0 && sr <= 3)) throw new Error("稀疏旋转越界");
          rot[sy][sx] = sr;
        }
      } else {
        if (cleanRot.length < height) throw new Error("旋转表需要 " + height + " 行");
        for (var ry = 0; ry < height; ry++) {
          if (cleanRot[ry].length !== cols) throw new Error("旋转表第 " + (ry + 1) + " 行长度不对");
          for (var rx = 0; rx < cols; rx++) {
            var ch = cleanRot[ry].charAt(rx);
            if ("0123".indexOf(ch) < 0) throw new Error("旋转表第 " + (ry + 1) + " 行有非 0..3 字符");
            rot[ry][rx] = Number(ch);
          }
        }
      }
    }

    var objects = objectLines.map(function (line) { return parseObjectLine(line, cols, height); });
    return { rows: rows, rot: rot, objects: objects };
  }

  /* ---------------------------------------------------------------- 状态 */

  function markDirty() {
    S.dirty = serialize() !== S.savedText;
    dirtyEl.classList.toggle("on", S.dirty);
  }

  function pushHistory() {
    S.history.push(cloneState());
    if (S.history.length > 100) S.history.shift();
    S.future.length = 0;
  }

  function restoreState(snap) {
    S.mode = snap.mode || "map";
    S.shape = snap.shape || "1x1";
    S.layoutId = hasLayoutId(snap.layoutId) ? snap.layoutId : null;
    S.candidateId = hasLayoutId(snap.candidateId) ? snap.candidateId : 1;
    S.cols = snap.cols || (snap.map.length ? snap.map[0].length : DEFAULT_COLS);
    S.rows = snap.rows || snap.map.length || DEFAULT_ROWS;
    S.map = cloneGrid(snap.map);
    S.rot = cloneGrid(snap.rot);
    S.objects = cloneObjects(snap.objects);
    updateWorkspaceControls();
    syncCanvas();
    updateSizeUi();
    changed();
  }

  function undo() {
    if (!S.history.length) return;
    S.future.push(cloneState());
    restoreState(S.history.pop());
  }

  function redo() {
    if (!S.future.length) return;
    S.history.push(cloneState());
    restoreState(S.future.pop());
  }

  function changed() {
    refreshInfo();
    scheduleDraw();
    markDirty();
    saveDraftSoon();
    scheduleValidate();
  }

  /* ---------------------------------------------------------------- 编辑 */

  function putCell(x, y, code) {
    if (S.mode === "layout") return;
    if (!inBounds(x, y)) return;
    if (isDoorCode(code)) { placeDoor(x, y, code); return; }
    removeDoorAt(x, y);
    S.map[y][x] = code;
    S.rot[y][x] = code === "." ? 0 : (S.angle & 3);
  }

  function placeDoor(x, y, code) {
    if (S.mode === "layout") return;
    var angle = S.angle & 3;
    code = code || "D";
    var p = doorPair(x, y, angle);
    if (code === "D" && !inBounds(p[0], p[1])) {
      setStatus("普通门需要两格，这个位置放不下（" + DIRS[angle] + " 方向）", false);
      return;
    }
    removeDoorAt(x, y);
    if (code === "H") {          /* 隐藏门只占 1 格，背后是隐藏房墙 */
      S.map[y][x] = code;
      S.rot[y][x] = angle;
      return;
    }
    removeDoorAt(p[0], p[1]);
    S.map[y][x] = code;
    S.rot[y][x] = angle;
    S.map[p[1]][p[0]] = ".";
    S.rot[p[1]][p[0]] = 0;
  }

  function removeDoorAt(x, y) {
    if (S.mode === "layout") return;
    if (!inBounds(x, y)) return;
    if (isDoorCode(S.map[y][x])) {
      var code = S.map[y][x];
      var p = doorPair(x, y, S.rot[y][x]);
      S.map[y][x] = ".";
      S.rot[y][x] = 0;
      /*  隐藏门只占 1 格：不要顺手把旁边一格也擦了  */
      if (code === "D" && inBounds(p[0], p[1])) { S.map[p[1]][p[0]] = "."; S.rot[p[1]][p[0]] = 0; }
      return;
    }
    var dirs = [[1, 0], [-1, 0], [0, 1], [0, -1]];
    for (var i = 0; i < dirs.length; i++) {
      var nx = x + dirs[i][0], ny = y + dirs[i][1];
      if (!inBounds(nx, ny) || S.map[ny][nx] !== "D") continue;   /* H 没有第二格 */
      var q = doorPair(nx, ny, S.rot[ny][nx]);
      if (q[0] === x && q[1] === y) { S.map[ny][nx] = "."; S.rot[ny][nx] = 0; }
    }
  }

  function objectsAt(x, y) {
    return S.objects.filter(function (obj) { return obj.x === x && obj.y === y; });
  }

  function removeObjectsAt(x, y) {
    S.objects = S.objects.filter(function (obj) { return !(obj.x === x && obj.y === y); });
  }

  function removeObjectKindAt(x, y, kind) {
    S.objects = S.objects.filter(function (obj) {
      return !(obj.x === x && obj.y === y && obj.kind === kind);
    });
  }

  function addObject(kind, x, y) {
    if (S.mode === "layout" && (kind === "exit" || "B".indexOf(S.map[y] && S.map[y][x]) < 0)) return;
    if (!inBounds(x, y) || !OBJECT_BY_ID[kind]) return;
    removeObjectKindAt(x, y, kind);
    S.objects.push({ kind: kind, x: x, y: y, arg: 0 });
    S.objects = sortObjects(S.objects);
  }

  function paintAt(p) {
    if (S.loading) return;
    if (S.mode === "layout" && S.layer !== "object") return;
    if (S.mode === "map" && S.layer === "object") return;
    if (S.layer === "object") {
      if (S.erase) removeObjectsAt(p.x, p.y);
      else addObject(S.objBrush, p.x, p.y);
    } else if (S.erase) {
      removeDoorAt(p.x, p.y);
      S.map[p.y][p.x] = ".";
      S.rot[p.y][p.x] = 0;
    } else {
      putCell(p.x, p.y, currentCode());
    }
    changed();
  }

  /* ---------------------------------------------------------------- 渲染 */

  function drawAngle(angle) {
    if (!S.fwView) return angle & 3;
    var a = angle & 3;
    return a === 1 ? 3 : (a === 3 ? 1 : a);
  }

  function drawTileImage(target, img, px, py, size, angle, fx, fy) {
    target.save();
    target.translate(px + size / 2, py + size / 2);
    target.rotate(angle * Math.PI / 2);
    if (fx) target.scale(-1, 1);
    if (fy) target.scale(1, -1);
    target.imageSmoothingEnabled = false;
    target.drawImage(img, -size / 2, -size / 2, size, size);
    target.restore();
  }

  function paintCell(target, code, x, y) {
    var t = tileOf(code);
    var px = x * TILE, py = y * TILE;
    target.fillStyle = t.color || "#172535";
    target.fillRect(px, py, TILE, TILE);
    if (!t.image || !images[t.id]) return;
    var fx = code === "X" || code === "Z" || code === "x" || code === "z";
    var fy = code === "Y" || code === "Z" || code === "y" || code === "z";
    drawTileImage(target, images[t.id], px, py, TILE, drawAngle(S.rot[y][x]), fx, fy);
  }

  function paintDoor(target, x, y) {
    var code = S.map[y][x];
    var one = code === "H";
    var angle = S.rot[y][x] & 3;
    var horiz = (angle & 1) !== 0;
    var px = x * TILE, py = y * TILE;
    var fw = one ? TILE : (horiz ? 2 * TILE : TILE);
    var fh = one ? TILE : (horiz ? TILE : 2 * TILE);
    var spriteAngle = drawAngle(angle);

    var wall = one ? images.hidden_wall : images.wall;
    if (wall) {
      for (var i = 0; i < (one ? 1 : 2); i++) {
        var wx = px + (horiz ? i * TILE : 0);
        var wy = py + (horiz ? 0 : i * TILE);
        drawTileImage(target, wall, wx, wy, TILE, spriteAngle, false, false);
      }
    }
    var img = images[DOOR_CODES[code] || "door"];
    if (!img) return;

    var dw = img.naturalWidth || (one ? TILE : 40);
    var dh = img.naturalHeight || (one ? TILE : 64);
    var ow = horiz ? dh : dw;
    var oh = horiz ? dw : dh;
    var ox = px + (fw - ow) / 2;
    var oy = py + (fh - oh) / 2;

    target.save();
    target.translate(ox + ow / 2, oy + oh / 2);
    target.rotate(spriteAngle * Math.PI / 2);
    target.imageSmoothingEnabled = false;
    target.drawImage(img, -dw / 2, -dh / 2, dw, dh);
    target.restore();
  }

  function paintObject(target, obj) {
    var def = OBJECT_BY_ID[obj.kind];
    var px = obj.x * TILE, py = obj.y * TILE;
    var image = images[obj.kind];
    if (image) {
      target.imageSmoothingEnabled = false;
      target.drawImage(image, px, py, TILE, TILE);
    } else {
      var color = obj.kind === "slime" ? "#65c46f" : obj.kind === "bug" ? "#c58a4a" :
        obj.kind === "aimmer" ? "#d84a4a" : obj.kind === "web" ? "#c8d0dc" : "#a07850";
      target.fillStyle = color;
      target.fillRect(px + 3, py + 3, TILE - 6, TILE - 6);
      target.fillStyle = "#0b1118";
      target.font = "9px Consolas, monospace";
      target.textAlign = "center";
      target.textBaseline = "middle";
      target.fillText(obj.kind.slice(0, 4), px + TILE / 2, py + TILE / 2);
    }
  }

  function drawGrid(target) {
    var x, y, lx, ly;
    target.save();
    target.lineWidth = 1;
    target.strokeStyle = "rgba(210,230,250,.10)";
    target.beginPath();
    for (x = 0; x <= S.cols; x++) {
      lx = x * TILE + 0.5;
      target.moveTo(lx, 0);
      target.lineTo(lx, H);
    }
    for (y = 0; y <= S.rows; y++) {
      ly = y * TILE + 0.5;
      target.moveTo(0, ly);
      target.lineTo(W, ly);
    }
    target.stroke();

    target.strokeStyle = "rgba(90,170,255,.20)";
    target.beginPath();
    for (x = 0; x <= S.cols; x += 4) {
      lx = x * TILE + 0.5;
      target.moveTo(lx, 0);
      target.lineTo(lx, H);
    }
    for (y = 0; y <= S.rows; y += 4) {
      ly = y * TILE + 0.5;
      target.moveTo(0, ly);
      target.lineTo(W, ly);
    }
    target.stroke();
    target.restore();
  }

  function drawOverlay(target) {
    target.save();

    var a = arenaRect();
    var ax0 = a.x0 * TILE, ay0 = a.y0 * TILE;
    var ax1 = (a.x1 + 1) * TILE, ay1 = (a.y1 + 1) * TILE;
    target.strokeStyle = a.empty ? "rgba(231,200,106,.45)" : "rgba(89,169,255,.55)";
    target.lineWidth = 2;
    target.strokeRect(ax0 + 1, ay0 + 1, Math.max(ax1 - ax0 - 2, 1), Math.max(ay1 - ay0 - 2, 1));

    target.setLineDash([5, 5]);
    target.strokeStyle = "rgba(89,169,255,.30)";
    target.lineWidth = 1;
    var slots = doorSlots();
    for (var i = 0; i < slots.length; i++) {
      var slot = slots[i];
      var horiz = (slot[2] & 1) !== 0;
      var one = S.brush === "H";
      target.strokeRect(slot[0] * TILE + 0.5, slot[1] * TILE + 0.5,
                       one ? TILE - 1 : (horiz ? 2 * TILE - 1 : TILE - 1),
                       one ? TILE - 1 : (horiz ? TILE - 1 : 2 * TILE - 1));
    }
    target.setLineDash([]);

    /* 720p 参考框：画布比 720p 大时才有意义（只是参考，不落盘） */
    if (S.showRef && W >= REF_W && H >= REF_H) {
      target.setLineDash([6, 6]);
      target.strokeStyle = "rgba(120,200,160,.45)";
      target.lineWidth = 2;
      target.strokeRect(1, 1, REF_W - 2, REF_H - 2);
      target.setLineDash([]);
      target.fillStyle = "rgba(120,200,160,.80)";
      target.font = "12px Consolas, monospace";
      target.textAlign = "left";
      target.textBaseline = "top";
      target.fillText("720p 参考框 " + REF_W + " × " + REF_H + "（不参与校验）", 8, 8);
    }

    for (var key in S.issueCells) {
      if (!Object.prototype.hasOwnProperty.call(S.issueCells, key)) continue;
      var p = key.split(",");
      target.strokeStyle = S.issueCells[key] === "error" ? "rgba(255,110,110,.95)" : "rgba(231,200,106,.9)";
      target.lineWidth = 2;
      target.strokeRect(Number(p[0]) * TILE + 1, Number(p[1]) * TILE + 1, TILE - 2, TILE - 2);
    }

    if (S.hover) {
      target.strokeStyle = S.layer === "object" ? "rgba(244,170,98,.9)" : "rgba(255,255,255,.55)";
      target.lineWidth = S.layer === "object" ? 2 : 1;
      target.strokeRect(S.hover.x * TILE + 0.5, S.hover.y * TILE + 0.5, TILE - 1, TILE - 1);
    }

    if (S.flash && Date.now() < S.flash.until) {
      target.strokeStyle = "#f4aa62";
      target.lineWidth = 3;
      target.strokeRect(S.flash.x * TILE + 1.5, S.flash.y * TILE + 1.5, TILE - 3, TILE - 3);
    } else if (S.flash) {
      S.flash = null;
    }

    target.restore();
  }

  function draw() {
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.fillStyle = "#090e14";
    ctx.fillRect(0, 0, W, H);

    for (var y = 0; y < S.rows; y++) {
      for (var x = 0; x < S.cols; x++) {
        var code = S.map[y][x];
        if (code !== "." && !isDoorCode(code)) paintCell(ctx, code, x, y);
      }
    }
    for (var dy = 0; dy < S.rows; dy++) {
      for (var dx = 0; dx < S.cols; dx++) {
        if (isDoorCode(S.map[dy][dx])) paintDoor(ctx, dx, dy);
      }
    }

    var objects = S.objects.slice().sort(function (a, b) {
      var la = OBJECT_BY_ID[a.kind] && OBJECT_BY_ID[a.kind].layer === "scene" ? 0 : 1;
      var lb = OBJECT_BY_ID[b.kind] && OBJECT_BY_ID[b.kind].layer === "scene" ? 0 : 1;
      return (la - lb) || (a.y - b.y) || (a.x - b.x);
    });
    for (var oi = 0; oi < objects.length; oi++) paintObject(ctx, objects[oi]);

    drawGrid(ctx);
    drawOverlay(ctx);
  }

  var pendingDraw = false;
  function scheduleDraw() {
    if (pendingDraw) return;
    pendingDraw = true;
    window.requestAnimationFrame(function () {
      pendingDraw = false;
      draw();
    });
  }

  function drawPreview() {
    pctx.setTransform(1, 0, 0, 1, 0, 0);
    pctx.clearRect(0, 0, 96, 96);
    pctx.fillStyle = "#0b121b";
    pctx.fillRect(0, 0, 96, 96);

    if (S.layer === "object") {
      var def = OBJECT_BY_ID[S.objBrush];
      var objImage = images[S.objBrush];
      if (objImage) {
        var ow = objImage.naturalWidth || 32;
        var oh = objImage.naturalHeight || 32;
        var scale = 80 / Math.max(ow, oh);
        pctx.imageSmoothingEnabled = false;
        pctx.drawImage(objImage, 48 - ow * scale / 2, 48 - oh * scale / 2, ow * scale, oh * scale);
      }
      $("previewText").textContent = (def ? def.name : S.objBrush) + "\n对象 id：" + (def ? def.code : "?") +
        "\n坐标单位：32 px 格\n左键放，右键擦";
      return;
    }

    var code = currentCode();
    var t = tileOf(code);
    var angle = drawAngle(S.angle);
    var fx = code === "X" || code === "Z" || code === "x" || code === "z";
    var fy = code === "Y" || code === "Z" || code === "y" || code === "z";

    if (code === ".") {
      pctx.fillStyle = t.color;
      pctx.fillRect(16, 16, 64, 64);
    } else if (!images[t.id]) {
      pctx.fillStyle = t.color || "#172535";
      pctx.fillRect(16, 16, 64, 64);
    } else if (images[t.id]) {
      var img = images[t.id];
      var dw = img.naturalWidth || 32;
      var dh = img.naturalHeight || 32;
      var s = 80 / Math.max(dw, dh);
      var w = dw * s;
      var h = dh * s;
      pctx.save();
      pctx.translate(48, 48);
      pctx.rotate(angle * Math.PI / 2);
      if (fx) pctx.scale(-1, 1);
      if (fy) pctx.scale(1, -1);
      pctx.imageSmoothingEnabled = false;
      pctx.drawImage(img, -w / 2, -h / 2, w, h);
      pctx.restore();
    }

    var text = t.name + "\n字符：" + code;
    if (isDoorCode(code)) text += "\n方向：" + DIRS[S.angle & 3] + "（" + (S.angle & 3) * 90 + "°）" + (code === "H" ? "\n隐藏门：炸弹炸开才通" : "");
    else if (isCornerBrush(S.brush)) text += "\n翻转：" + cornerNamesFor(S.brush)[S.corner] + "\n旋转：" + (S.angle & 3) * 90 + "°";
    else if (code !== ".") text += "\n旋转：" + (S.angle & 3) * 90 + "°";
    $("previewText").textContent = text;
  }

  function refreshInfo() {
    var counts = {};
    for (var i = 0; i < CODES.length; i++) counts[CODES[i]] = 0;
    for (var y = 0; y < S.rows; y++) {
      for (var x = 0; x < S.cols; x++) counts[S.map[y][x]] += 1;
    }
    var html = "";
    for (var k = 0; k < CODES.length; k++) {
      var code = CODES[k];
      html += '<div class="count"><span>' + code + " · " + tileOf(code).name + "</span><b>" + counts[code] + "</b></div>";
    }
    var objectCounts = {};
    for (var oi = 0; oi < OBJECT_DEFS.length; oi++) objectCounts[OBJECT_DEFS[oi].id] = 0;
    for (var oj = 0; oj < S.objects.length; oj++) {
      if (objectCounts[S.objects[oj].kind] !== undefined) objectCounts[S.objects[oj].kind] += 1;
    }
    for (var di = 0; di < OBJECT_DEFS.length; di++) {
      var def = OBJECT_DEFS[di];
      html += '<div class="count"><span>● ' + def.name + "</span><b>" + objectCounts[def.id] + "</b></div>";
    }
    countsEl.innerHTML = html;
    try { mapTextEl.value = serializeExportJson(); }
    catch (e) { mapTextEl.value = e.message; }
    drawPreview();
  }

  /* ---------------------------------------------------------------- 输入 */

  function cellAt(event) {
    var rect = canvas.getBoundingClientRect();
    var sx = W / rect.width;
    var sy = H / rect.height;
    var x = Math.floor((event.clientX - rect.left) * sx / TILE);
    var y = Math.floor((event.clientY - rect.top) * sy / TILE);
    if (!inBounds(x, y)) return null;
    return { x: x, y: y };
  }

  function updateCursor(p) {
    if (!p) { cursorEl.textContent = "—"; return; }
    var code = S.map[p.y][p.x];
    var text = p.x + "," + p.y + " · " + code;
    if (isDoorCode(code)) text += " · " + DIRS[S.rot[p.y][p.x] & 3] + (code === "H" ? "（隐藏门）" : "");
    else if (code !== ".") text += " · " + (S.rot[p.y][p.x] & 3) * 90 + "°";
    var objs = objectsAt(p.x, p.y);
    if (objs.length) {
      text += " · " + objs.map(function (obj) { return obj.kind; }).join("+");
    }
    cursorEl.textContent = text;
  }

  function onDown(event) {
    if (event.button !== 0 && event.button !== 2) return;
    event.preventDefault();
    S.pending = cloneState();
    S.beforeText = serialize();
    S.drawing = true;
    S.erase = event.button === 2;
    S.lastCell = null;
    var p = cellAt(event);
    if (p) { paintAt(p); S.lastCell = p.x + "," + p.y; updateCursor(p); }
  }

  function onMove(event) {
    var p = cellAt(event);
    S.hover = p;
    updateCursor(p);
    if (!S.drawing || !p) { scheduleDraw(); return; }
    var key = p.x + "," + p.y;
    if (key === S.lastCell) return;
    S.lastCell = key;
    paintAt(p);
  }

  function onUp() {
    if (!S.drawing) return;
    S.drawing = false;
    if (S.pending && serialize() !== S.beforeText) {
      S.history.push(S.pending);
      if (S.history.length > 100) S.history.shift();
      S.future.length = 0;
    }
    S.pending = null;
    markDirty();
  }

  /* ---------------------------------------------------------------- 选择 */

  function updateSelection() {
    var tiles = paletteEl.querySelectorAll(".tile");
    for (var i = 0; i < tiles.length; i++) {
      tiles[i].classList.toggle("selected", S.layer === "tile" && tiles[i].getAttribute("data-brush") === S.brush);
    }
    var objects = objectPaletteEl.querySelectorAll(".tile");
    for (var oi = 0; oi < objects.length; oi++) {
      objects[oi].classList.toggle("selected", S.layer === "object" && objects[oi].getAttribute("data-object") === S.objBrush);
    }
    var cbtns = cornerRow.querySelectorAll("button");
    var fam = cornerCodesFor(S.brush);
    for (var c = 0; c < cbtns.length; c++) {
      if (isCornerBrush(S.brush)) cbtns[c].textContent = fam[c];
      cbtns[c].classList.toggle("on", isCornerBrush(S.brush) && c === S.corner);
    }
    var abtns = angleRow.querySelectorAll("button");
    for (var a = 0; a < abtns.length; a++) abtns[a].classList.toggle("on", a === (S.angle & 3));
  }

  function updateLayerHint() {
    var hint = $("layerHint");
    if (!hint) return;
    hint.textContent = S.layer === "tile"
      ? "地形层：画地板、墙、门、角块。切到对象层后，左键放对象、右键擦对象。"
      : "对象层：左键放 " + (OBJECT_BY_ID[S.objBrush] ? OBJECT_BY_ID[S.objBrush].name : S.objBrush) + "，右键擦掉这一格的所有对象。";
  }

  function selectLayer(layer) {
    if (S.mode === "layout") layer = "object";
    if (S.mode === "map") layer = "tile";
    S.layer = layer === "object" ? "object" : "tile";
    updateSelection();
    updateLayerHint();
    updateAngleHint();
    drawPreview();
    scheduleDraw();
  }

  function selectBrush(code) {
    if (S.mode === "layout") return;
    S.layer = "tile";
    S.brush = code;
    updateSelection();
    updateLayerHint();
    drawPreview();
    updateAngleHint();
  }

  function selectObject(kind) {
    if (S.mode === "map" || (S.mode === "layout" && kind === "exit")) return;
    if (!OBJECT_BY_ID[kind]) return;
    S.layer = "object";
    S.objBrush = kind;
    updateSelection();
    updateLayerHint();
    drawPreview();
    updateAngleHint();
  }

  function setCorner(index) {
    S.corner = index & 3;
    updateSelection();
    drawPreview();
  }

  function setAngle(index) {
    S.angle = index & 3;
    updateSelection();
    drawPreview();
    updateAngleHint();
  }

  function cycle() {
    if (S.layer === "object") {
      var index = 0;
      for (var i = 0; i < OBJECT_DEFS.length; i++) if (OBJECT_DEFS[i].id === S.objBrush) index = i;
      selectObject(OBJECT_DEFS[(index + 1) % OBJECT_DEFS.length].id);
      return;
    }
    if (isCornerBrush(S.brush)) setCorner(S.corner + 1);
    else setAngle(S.angle + 1);
  }

  function updateAngleHint() {
    angleRow.hidden = S.layer === "object";
    cornerRow.hidden = S.layer !== "tile" || !isCornerBrush(S.brush);
    var hint = $("angleHint");
    if (S.layer === "object") {
      hint.textContent = "对象层不使用旋转；坐标按整格保存（×32 px）。";
    } else if (isDoorCode(S.brush)) {
      hint.textContent = S.brush === "H"
        ? "隐藏门：只占 1 格（靠房间那侧），背后是隐藏房墙；角度就是方向 0=N 1=E 2=S 3=W。"
        : "普通门：方向 0=N 1=E 2=S 3=W；占两格，第二格自动清空。";
    } else if (isCornerBrush(S.brush)) {
      hint.textContent = "角块先翻转再旋转；旋转值会写进 rot 表，和 C 侧的 corner_char 家族一致。";
    } else {
      hint.textContent = "墙和地板按这个角度旋转；门的旋转值是方向，不是贴图角度。";
    }
  }

  /* ---------------------------------------------------------------- 视图 */

  var ZOOM_MIN = 0.1;
  var ZOOM_MAX = 8;

  function setZoom(value, manual, anchor) {
    var next = Number(value);
    if (!isFinite(next)) return;
    next = Math.max(ZOOM_MIN, Math.min(ZOOM_MAX, next));
    var previous = S.zoom;
    var view = canvasWrap.getBoundingClientRect();
    var padX = canvasWrap.clientWidth;
    var padY = canvasWrap.clientHeight;
    var contentX = W / 2;
    var contentY = H / 2;
    var oldRect = null;

    if (anchor && previous > 0) {
      oldRect = canvas.getBoundingClientRect();
      contentX = (anchor.x - oldRect.left) / previous;
      contentY = (anchor.y - oldRect.top) / previous;
    }

    S.zoom = next;
    canvas.style.width = (W * S.zoom) + "px";
    canvas.style.height = (H * S.zoom) + "px";
    /* Scrollable gutters on all sides keep the pointer anchor reachable even
       below viewport size; centered flex overflow only scrolls one way. */
    mapStage.style.width = (W * next + padX * 2) + "px";
    mapStage.style.height = (H * next + padY * 2) + "px";
    canvas.style.left = padX + "px";
    canvas.style.top = padY + "px";
    zoomSlider.value = String(Math.round(S.zoom * 100));
    zoomReadout.textContent = (Math.round(S.zoom * 1000) / 10) + "%";

    var screenX = anchor ? anchor.x - view.left : padX / 2;
    var screenY = anchor ? anchor.y - view.top : padY / 2;
    canvasWrap.scrollLeft = padX + contentX * next - screenX;
    canvasWrap.scrollTop = padY + contentY * next - screenY;

    if (manual) S.autoFit = false;
  }

  function zoomAtCenter(value) {
    var rect = canvasWrap.getBoundingClientRect();
    setZoom(value, true, {
      x: rect.left + rect.width / 2,
      y: rect.top + rect.height / 2
    });
  }

  function onWheel(event) {
    event.preventDefault();
    var delta = Math.abs(event.deltaY) >= Math.abs(event.deltaX || 0)
      ? event.deltaY : event.deltaX;
    if (!delta) return;
    if (event.deltaMode === 1) delta *= 16;
    if (event.deltaMode === 2) delta *= canvasWrap.clientHeight;
    var factor = Math.max(0.5, Math.min(2, Math.exp(-delta * 0.0015)));
    setZoom(S.zoom * factor, true, { x: event.clientX, y: event.clientY });
  }

  function fit() {
    var cw = canvasWrap.clientWidth - 48;
    var ch = canvasWrap.clientHeight - 48;
    if (cw < 100 || ch < 100) return;
    setZoom(Math.min(cw / W, ch / H));
    S.autoFit = true;
  }

  /* ---------------------------------------------------------------- 文件 */

  function setStatus(message, kind) {
    statusEl.textContent = message;
    statusEl.className = "status" + (kind === false ? " err" : kind === true ? " ok" : kind === "busy" ? " busy" : "");
  }

  async function apiGet(path) {
    return fetch(path, { cache: "no-store" });
  }

  async function apiPost(path, body) {
    return fetch(path, {
      method: "POST",
      headers: { "Content-Type": "text/plain;charset=utf-8" },
      body: body,
      cache: "no-store"
    });
  }

  async function readJson(response) {
    var text = await response.text();
    try { return JSON.parse(text); } catch (e) { return { ok: false, error: text.slice(0, 300) || "服务端返回了非 JSON" }; }
  }

  async function runValidate() {
    if (!S.served) return;
    try {
      var response = await apiPost("/api/validate", serialize());
      var json = await readJson(response);
      if (json.issues) {
        renderIssues(json.issues);
      } else if (json.error) {
        renderIssues([{ level: "error", message: json.error, x: -1, y: -1 }]);
      }
    } catch (e) {
      setStatus("校验请求失败：" + e.message, false);
    }
  }

  var validateTimer = null;
  function scheduleValidate() {
    if (!S.served) return;
    if (validateTimer) clearTimeout(validateTimer);
    validateTimer = setTimeout(runValidate, 600);
  }

  function renderIssues(list) {
    var issues = list || [];
    var errors = 0;
    var warnings = 0;
    var cells = {};
    for (var i = 0; i < issues.length; i++) {
      var it = issues[i];
      if (it.level === "error") errors++; else warnings++;
      if (it.x >= 0) {
        var key = it.x + "," + it.y;
        if (it.level === "error" || !cells[key]) cells[key] = it.level === "error" ? "error" : "warning";
      }
    }
    S.issueCells = cells;
    issuesEl.innerHTML = "";
    if (!issues.length) {
      var empty = document.createElement("div");
      empty.className = "issue empty";
      empty.textContent = "没有发现问题";
      issuesEl.appendChild(empty);
    } else {
      issues.slice(0, 80).forEach(function (it) {
        var button = document.createElement("button");
        button.type = "button";
        button.className = "issue " + it.level;
        var tag = document.createElement("span");
        tag.className = "tag";
        tag.textContent = it.level === "error" ? "错误" : "警告";
        var where = document.createElement("span");
        where.className = "where";
        where.textContent = it.x >= 0 ? "(" + it.x + "," + it.y + ")" : "";
        var message = document.createElement("span");
        message.textContent = it.message;
        button.appendChild(tag);
        button.appendChild(where);
        button.appendChild(message);
        if (it.x >= 0) button.addEventListener("click", function () { flash(it.x, it.y); });
        issuesEl.appendChild(button);
      });
    }
    issueBadge.textContent = issues.length ? "错误 " + errors + " · 警告 " + warnings : "";
    issueSummary.textContent = errors ? "有 " + errors + " 个错误，保存 / 生成 C 会被拒绝"
      : warnings ? "只有警告，可以保存" : "校验通过";
    issueSummary.className = "summary " + (errors ? "err" : warnings ? "warn" : "ok");
    scheduleDraw();
  }

  function flash(x, y) {
    S.flash = { x: x, y: y, until: Date.now() + 1400 };
    scheduleDraw();
    window.setTimeout(scheduleDraw, 1450);
  }

  function numberedFile(shape, id) {
    return shape + "_" + String(id).padStart(3, "0") + ".json";
  }

  function hasLayoutId(value) {
    return value !== null && value !== undefined && value !== "";
  }

  function updateWorkspaceControls() {
    var layout = S.mode === "layout";
    $("editMode").value = S.mode;
    $("poolShape").value = S.shape;
    $("layoutControls").hidden = !layout;
    $("btnExportMap").hidden = layout;
    $("legacyFileSection").hidden = true;
    ["templateSection", "sizeSection"].forEach(function (id) { $(id).hidden = layout; });
    var activeId = hasLayoutId(S.layoutId) ? S.layoutId : S.candidateId;
    $("layoutNumber").value = hasLayoutId(activeId) ? activeId : "";
    $("layoutFileHint").textContent = hasLayoutId(activeId)
      ? "output/layouts/" + numberedFile(S.shape, activeId) : "编号已用完";
    $("btnSaveLayout").disabled = !hasLayoutId(activeId);
    $("workspaceHint").textContent = layout
      ? "地形已锁定，仅摆放普通对象。L 型统一左下空缺；特殊房暂不参与。"
      : "仅编辑地形。导出到 output/maps/，不会替换布局所用的正式固定空间。";
    if (paletteEl.parentElement) paletteEl.parentElement.hidden = layout;
    if (objectPaletteEl.parentElement) objectPaletteEl.parentElement.hidden = !layout;
    S.layer = layout ? "object" : "tile";
    if (S.objBrush === "exit") S.objBrush = "slime";
  }

  async function refreshLayoutCatalog() {
    var shape = S.shape;
    var result = await readJson(await apiGet("/api/layout_catalog?map=" + encodeURIComponent(shape)));
    if (!result.ok) throw new Error(result.error || "布局列表读取失败");
    if (shape !== S.shape) return result;
    var list = $("layoutList");
    list.innerHTML = "";
    result.layouts.forEach(function (entry) {
      var option = document.createElement("option");
      option.value = entry.id;
      option.textContent = entry.file;
      list.appendChild(option);
    });
    if (!result.layouts.length) {
      var empty = document.createElement("option");
      empty.value = ""; empty.textContent = "暂无布局，点击新建"; list.appendChild(empty);
    }
    if (hasLayoutId(S.layoutId)) list.value = String(S.layoutId);
    S.candidateId = result.next_id;
    updateWorkspaceControls();
    return result;
  }

  var workspaceEpoch = 0;
  async function loadWorkspace(mode, shape) {
    var epoch = ++workspaceEpoch;
    S.loading = true;
    try {
      var template = {"1x1": "template_1x1", "L": "template_L", "2x2": "template_2x2"}[shape];
      if (!template) throw new Error("本轮只支持普通房型 1x1、L、2x2");
      var result = await readJson(await apiGet("/api/template?size=" + template));
      if (epoch !== workspaceEpoch) return;
      if (!result.ok) throw new Error(result.error || "固定房型读取失败");
      S.mode = mode; S.shape = shape; S.layoutId = null;
      applyState(result.rows, result.rot, []);
      S.history = []; S.future = [];
      await refreshLayoutCatalog();
      if (epoch !== workspaceEpoch) return;
      S.savedText = serialize();
      updateWorkspaceControls();
      updateSelection(); updateLayerHint(); updateAngleHint();
      changed();
      setStatus(mode === "layout" ? "已新建 " + shape + " 布局，保存时分配房型内编号" : "地图模式：" + shape + "，导出不改变正式空间");
    } catch (e) {
      if (epoch === workspaceEpoch) { updateWorkspaceControls(); setStatus(e.message, false); }
    } finally {
      if (epoch === workspaceEpoch) S.loading = false;
    }
  }

  function switchWorkspace(mode, shape) {
    if (S.dirty && !window.confirm("当前修改尚未保存，放弃修改并载入所选房型？")) {
      updateWorkspaceControls(); return;
    }
    loadWorkspace(mode, shape);
  }

  async function loadNumberedLayout() {
    var rawId = $("layoutList").value;
    if (rawId === "") return;
    var id = Number(rawId);
    if (!Number.isInteger(id) || id < 0) return;
    if (S.dirty && !window.confirm("放弃当前未保存修改并读取布局？")) return;
    try {
      var result = await readJson(await apiGet("/api/load_layout?map=" + encodeURIComponent(S.shape) + "&id=" + id));
      if (!result.ok) throw new Error(result.error || "读取布局失败");
      if (await importRaw(JSON.stringify(result.layout), result.layout.name, true)) {
        S.layoutId = id;
        S.history = []; S.future = [];
        updateWorkspaceControls();
        setStatus("已读取 " + numberedFile(S.shape, id));
      }
    } catch (e) { setStatus(e.message, false); }
  }

  async function saveNumberedLayout() {
    if (S.loading) return;
    if (!S.served) { setStatus("布局保存需要通过 serve.py 打开", false); return; }
    try {
      var epoch = workspaceEpoch, snapshot = serialize(), shape = S.shape;
      var id = hasLayoutId(S.layoutId) ? S.layoutId : S.candidateId;
      if (!hasLayoutId(id)) throw new Error("本房型编号已用完");
      var result = await readJson(await apiPost("/api/save_layout?map=" + encodeURIComponent(shape) +
        "&id=" + id + "&create=" + (hasLayoutId(S.layoutId) ? "0" : "1"), snapshot));
      if (!result.ok) throw new Error(result.error || "保存失败");
      if (epoch !== workspaceEpoch) return;
      S.layoutId = result.layout_id;
      S.savedText = snapshot;
      await refreshLayoutCatalog();
      markDirty(); saveDraftNow(S.dirty);
      setStatus("已保存 output/layouts/" + result.file + "；应用布局生成后进入运行工程");
    } catch (e) { setStatus(e.message, false); }
  }

  async function exportMap() {
    if (S.loading) return;
    try {
      var epoch = workspaceEpoch, snapshot = serialize();
      var result = await readJson(await apiPost("/api/save_map?map=" + encodeURIComponent(S.shape), snapshot));
      if (!result.ok) throw new Error(result.error || "地图导出失败");
      if (epoch !== workspaceEpoch) return;
      S.savedText = snapshot; markDirty(); saveDraftNow(S.dirty);
      setStatus("已导出 output/maps/" + result.file + "，正式固定空间保持原版本");
    } catch (e) { setStatus(e.message, false); }
  }

  async function save() {
    return S.mode === "layout" ? saveNumberedLayout() : exportMap();
  }

  async function emitC(quiet) {
    if (!S.served) { if (!quiet) setStatus("生成 C 需要 serve.py", false); return false; }
    if (S.loading) { if (!quiet) setStatus("正在读取 output/room_map.json，请稍后再生成", "busy"); return false; }
    S.cfile = cFileNameEl.value.trim() || "game_world_map.c";
    var response = await apiPost(
      "/api/emit_c?name=" + encodeURIComponent(S.cfile) + "&map=" + encodeURIComponent(S.file),
      serialize()
    );
    var json = await readJson(response);
    if (!json.ok) {
      if (json.issues) renderIssues(json.issues);
      setStatus("生成 C 失败：" + (json.error || "未知错误"), false);
      return false;
    }
    S.lastC = json.c;
    if (json.issues) renderIssues(json.issues);
    if (!quiet) setStatus("已生成 output/" + json.file + "（" + json.c.length + " 字节）");
    return true;
  }

  /* 模板：内置 1×1 / L / 2×2 + templates/custom/ 下的自定义模板。
   *  ★ 列表走 /api/templates；自定义模板存盘走 /api/template_save，不用手动改 mapfmt.py。 */
  var TEMPLATE_FALLBACK_LABELS = { template_1x1: "1×1 40×23", template_L: "L 形 45×35", template_2x2: "2×2 45×35" };

  function findTemplate(id) {
    for (var i = 0; i < S.templates.length; i++) {
      if (S.templates[i].id === id) return S.templates[i];
    }
    return null;
  }

  function templateOptionLabel(tpl) {
    var label = tpl.label || tpl.id;
    if (!tpl.builtin) label = "自定义 · " + label;
    if (tpl.size && label.indexOf("×") < 0) label += " · " + tpl.size[0] + "×" + tpl.size[1];
    if (tpl.loadable === false) label += "（损坏）";
    return label;
  }

  async function refreshTemplates(selectId) {
    if (!S.served) return;
    try {
      var response = await apiGet("/api/templates");
      var json = await readJson(response);
      if (!json.ok) { setStatus("模板列表读取失败：" + (json.error || "未知错误"), false); return; }
      S.templates = json.templates || [];
      var previous = selectId || templateListEl.value;
      templateListEl.innerHTML = "";
      S.templates.forEach(function (tpl) {
        var option = document.createElement("option");
        option.value = tpl.id;
        option.textContent = templateOptionLabel(tpl);
        option.disabled = tpl.loadable === false;
        templateListEl.appendChild(option);
      });
      if (previous && findTemplate(previous)) templateListEl.value = previous;
    } catch (e) {
      setStatus("模板列表读取失败：" + e.message, false);
    }
  }

  async function loadTemplate(id) {
    if (!S.served) { setStatus("模板需要 serve.py（file:// 下只能手动画）", false); return; }
    var info = findTemplate(id);
    var fallback = (info && info.label) || TEMPLATE_FALLBACK_LABELS[id] || id;
    setStatus("载入模板 " + fallback + "…", "busy");
    try {
      var response = await apiGet("/api/template?size=" + encodeURIComponent(id));
      var json = await readJson(response);
      if (!json.ok) { setStatus("模板载入失败：" + (json.error || "未知错误"), false); return; }
      var label = json.label || fallback;
      var ok = await importRaw(json.text, label, false);
      if (ok) {
        S.templateId = id;
        setStatus("已载入模板：" + label + "（" + json.size[0] + "×" + json.size[1] + "）");
      }
    } catch (e) {
      setStatus("载入模板失败：" + e.message, false);
    }
  }

  async function saveTemplate() {
    if (!S.served) { setStatus("保存模板需要 serve.py", false); return; }
    if (S.loading) { setStatus("正在读取 output 文件，请稍后再存模板", "busy"); return; }
    var raw = templateNameEl.value.trim();
    if (!raw) {
      setStatus("先填模板文件名（英文/数字，例如 my_l_room）", false);
      templateNameEl.focus();
      return;
    }
    var slug = raw.replace(/\.json$/i, "");
    var id = (slug.toLowerCase().indexOf("custom_") === 0 ? slug : "custom_" + slug).toLowerCase();
    if (findTemplate(id) && !window.confirm("自定义模板 " + id + " 已存在，覆盖？")) return;
    var label = templateLabelEl.value.trim() || slug.replace(/^custom_/i, "");
    setStatus("保存模板 " + label + "…", "busy");
    try {
      var response = await apiPost(
        "/api/template_save?name=" + encodeURIComponent(raw) + "&label=" + encodeURIComponent(label),
        serialize()
      );
      var json = await readJson(response);
      if (!json.ok) {
        if (json.issues) renderIssues(json.issues);
        setStatus("保存模板失败：" + (json.error || "未知错误"), false);
        return;
      }
      S.templateId = json.template.id;
      await refreshTemplates(json.template.id);
      setStatus("已保存自定义模板：" + json.template.label + "（" + json.template.file + "）");
    } catch (e) {
      setStatus("保存模板失败：" + e.message, false);
    }
  }

  async function deleteTemplate() {
    var id = templateListEl.value;
    if (!id) { setStatus("先选择一个模板", false); return; }
    var info = findTemplate(id);
    if (info && info.builtin) { setStatus("内置模板不能删除：" + id, false); return; }
    if (!window.confirm("删除自定义模板 " + id + " ？")) return;
    try {
      var response = await apiPost("/api/template_delete?name=" + encodeURIComponent(id), "x");
      var json = await readJson(response);
      if (!json.ok) { setStatus("删除模板失败：" + (json.error || "未知错误"), false); return; }
      if (S.templateId === id) S.templateId = "";
      await refreshTemplates();
      setStatus("已删除模板：" + id);
    } catch (e) {
      setStatus("删除模板失败：" + e.message, false);
    }
  }

  async function importRaw(text, label, fromDisk, record) {
    var data = null;
    if (S.served) {
      var response = await apiPost("/api/import", text);
      var json = await readJson(response);
      if (!json.ok) {
        if (json.issues) renderIssues(json.issues);
        setStatus("导入失败：" + (json.error || "未知错误"), false);
        return false;
      }
      data = {
        rows: json.rows,
        rot: json.rot,
        objects: json.objects || [],
        issues: json.issues || [],
        mapId: json.map, layoutId: json.layout_id,
        format: json.format
      };
    } else {
      if (/room_map_shared|room_rot_shared|room_objects/.test(text)) {
        setStatus("导入 C 需要 serve.py；file:// 下只能导入 txt", false);
        return false;
      }
      try {
        var parsed = parseLocal(text);
        data = {
          rows: parsed.rows,
          rot: parsed.rot,
          objects: parsed.objects,
          issues: [],
          format: text.trim().charAt(0) === "{" ? "json" : "text"
        };
      } catch (e) {
        setStatus("导入失败：" + e.message, false);
        return false;
      }
    }
    if (record !== false) pushHistory();
    var isLayout = /"format"\s*:\s*"map_designer_layout"/.test(text);
    var normal = ["1x1", "L", "2x2"].indexOf(data.mapId) >= 0;
    if (isLayout && !normal) { setStatus("本轮不编辑特殊房布局", false); return false; }
    if (isLayout) S.mode = "layout";
    else if (!normal) S.mode = "map";
    if (normal) S.shape = data.mapId;
    S.layoutId = null;
    if (hasLayoutId(data.layoutId)) S.candidateId = data.layoutId;
    applyState(data.rows, data.rot, data.objects);
    if (S.served && S.mode === "layout") {
      try { await refreshLayoutCatalog(); }
      catch (e) { setStatus(e.message, false); }
      if (hasLayoutId(data.layoutId)) S.candidateId = data.layoutId;
    }
    updateWorkspaceControls();
    updateSelection(); updateLayerHint(); updateAngleHint();
    if (fromDisk) { S.savedText = serialize(); }
    renderIssues(data.issues);
    refreshInfo();
    markDirty();
    scheduleDraw();
    saveDraftSoon();
    var kindLabel = data.format === "c" ? " C" : data.format === "json" ? " JSON" : "文本";
    setStatus("已导入" + kindLabel + (label ? "：" + label : ""));
    return true;
  }

  async function loadName(name, quiet, record) {
    if (!S.served) return false;
    var response = await apiGet("/api/load?name=" + encodeURIComponent(name));
    if (!response.ok) {
      if (!quiet) setStatus("读取失败：" + name, false);
      return false;
    }
    var text = await response.text();
    if (/\.c$/i.test(name)) {
      S.cfile = name;
      cFileNameEl.value = name;
    } else {
      S.file = name;
      fileNameEl.value = name;
    }
    return importRaw(text, "output/" + name, true, record);
  }

  async function refreshList() {
    if (!S.served) return;
    try {
      var response = await apiGet("/api/list");
      var json = await readJson(response);
      if (!json.ok) return;
      var previous = fileListEl.value;
      fileListEl.innerHTML = "";
      var placeholder = document.createElement("option");
      placeholder.value = "";
      placeholder.textContent = "— 选择 output 文件 —";
      fileListEl.appendChild(placeholder);
      json.files.forEach(function (file) {
        var option = document.createElement("option");
        option.value = file.name;
        option.textContent = file.name + "  ·  " + file.size + " B";
        fileListEl.appendChild(option);
      });
      if (previous) fileListEl.value = previous;
      else if (json.files.some(function (file) { return file.name === S.file; })) fileListEl.value = S.file;
    } catch (e) {
      /* 离线 / 服务没起，列表空着就行 */
    }
  }

  async function deleteFile() {
    var name = fileListEl.value;
    if (!name) { setStatus("先在列表里选一个文件，再点删除", false); return; }
    if (!window.confirm("删除 output/" + name + " ？")) return;
    var response = await apiPost("/api/delete?name=" + encodeURIComponent(name), "x");
    var json = await readJson(response);
    if (!json.ok) { setStatus("删除失败：" + (json.error || ""), false); return; }
    await refreshList();
    setStatus(json.existed ? "已删除 output/" + name : "文件本来就不存在：" + name);
  }

  function downloadBlob(text, filename, type) {
    var blob = new Blob([text], { type: type || "text/plain;charset=utf-8" });
    var url = URL.createObjectURL(blob);
    var anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = filename;
    document.body.appendChild(anchor);
    anchor.click();
    document.body.removeChild(anchor);
    window.setTimeout(function () { URL.revokeObjectURL(url); }, 1000);
  }

  async function downloadC() {
    var ok = await emitC(true);
    if (!ok) { setStatus("生成 C 失败，没法下载", false); return; }
    downloadBlob(S.lastC, S.cfile, "text/plain;charset=utf-8");
    setStatus("已下载 " + S.cfile);
  }

  function baseName() {
    return (S.file || "room_map.json").replace(/\.(txt|json)$/i, "");
  }

  function serializeExportJson() {
    if (S.mode === "map") {
      var map = JSON.parse(serializeJson());
      map.name = S.shape; map.objects = [];
      delete map.sha256;
      return JSON.stringify(map, null, 2) + "\n";
    }
    var id = hasLayoutId(S.layoutId) ? S.layoutId : S.candidateId;
    if (!hasLayoutId(id)) throw new Error("本房型编号已用完");
    return JSON.stringify({format: "map_designer_layout", version: 1,
      name: numberedFile(S.shape, id).replace(/\.json$/, ""), map: S.shape,
      layout_id: id, objects: cloneObjects(S.objects)}, null, 2) + "\n";
  }

  function downloadJson() {
    var name = S.mode === "layout" ? numberedFile(S.shape, hasLayoutId(S.layoutId) ? S.layoutId : S.candidateId) : S.shape + ".json";
    downloadBlob(serializeExportJson(), name, "application/json;charset=utf-8");
    setStatus("已下载 " + name);
  }

  function copyText() {
    var text = serializeExportJson();
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(function () { setStatus("已复制 JSON"); },
        function () { mapTextEl.select(); setStatus("浏览器不让写剪贴板，已选中文本框，Ctrl+C 复制", false); });
    } else {
      mapTextEl.select();
      setStatus("已选中文本框，Ctrl+C 复制", false);
    }
  }

  function exportPng() {
    var anchor = document.createElement("a");
    anchor.href = canvas.toDataURL("image/png");
    anchor.download = baseName() + ".png";
    document.body.appendChild(anchor);
    anchor.click();
    document.body.removeChild(anchor);
    setStatus("已导出 PNG（当前画布，含网格和校验框）");
  }

  /* ---------------------------------------------------------------- 草稿 */

  var draftTimer = null;
  function saveDraftNow(dirty) {
    try {
      localStorage.setItem("map_designer_draft", JSON.stringify({
        map: S.map,
        rot: S.rot,
        objects: S.objects,
        dirty: !!dirty,
        mode: S.mode, shape: S.shape, layoutId: S.layoutId, candidateId: S.candidateId
      }));
    } catch (e) { /* 隐私模式 / 配额满，忽略 */ }
  }

  function saveDraftSoon() {
    if (draftTimer) clearTimeout(draftTimer);
    draftTimer = setTimeout(function () { saveDraftNow(S.dirty); }, 300);
  }

  function loadDraft() {
    try {
      var raw = localStorage.getItem("map_designer_draft") || localStorage.getItem("debug_room_map");
      if (!raw) return null;
      var data = JSON.parse(raw);
      var map = Array.isArray(data) ? data : data.map;
      var rot = Array.isArray(data) ? null : data.rot;
      var objects = Array.isArray(data) ? [] : (data.objects || []);
      if (!Array.isArray(map) || !map.length) return null;
      var height = map.length;
      var cols = Array.isArray(map[0]) ? map[0].length : 0;
      if (!(cols >= MIN_COLS && cols <= MAX_COLS && height >= MIN_ROWS && height <= MAX_ROWS)) return null;
      var rows = [];
      for (var y = 0; y < height; y++) {
        if (!Array.isArray(map[y]) || map[y].length !== cols) return null;
        rows.push(map[y].join(""));
      }
      var rotGrid = grid(0, cols, height);
      if (Array.isArray(rot)) {
        for (var ry = 0; ry < height && ry < rot.length; ry++) {
          if (!Array.isArray(rot[ry])) continue;
          for (var rx = 0; rx < cols && rx < rot[ry].length; rx++) rotGrid[ry][rx] = Number(rot[ry][rx]) & 3;
        }
      }
      var cleanObjects = [];
      for (var i = 0; i < objects.length; i++) {
        var obj = objects[i];
        if (!obj || !OBJECT_BY_ID[obj.kind] || !inBounds(obj.x, obj.y, cols, height)) continue;
        cleanObjects.push({ kind: obj.kind, x: obj.x | 0, y: obj.y | 0, arg: obj.arg | 0 });
      }
      var dirty = Array.isArray(data) ? true : data.dirty !== false;
      return { rows: rows, rot: rotGrid, objects: cleanObjects, dirty: dirty,
        mode: data.mode, shape: data.shape, layoutId: data.layoutId, candidateId: data.candidateId };
    } catch (e) {
      return null;
    }
  }

  /* ---------------------------------------------------------------- 图片 */

  function loadImages() {
    var defs = TILES.concat(OBJECT_DEFS.map(function (def) {
      return { id: def.id, image: def.image };
    }));
    var jobs = defs.filter(function (t) { return t.image; }).map(function (t) {
      return new Promise(function (resolve) {
        var image = new Image();
        image.onload = function () { images[t.id] = image; resolve(); };
        image.onerror = function () { images[t.id] = null; resolve(); };
        image.src = t.image;
      });
    });
    return Promise.all(jobs);
  }

  /* ---------------------------------------------------------------- UI */

  /* 把 /api/assets 的 roles 对到笔刷上（roles 的 key 就是 tile.id / object.id） */
  function applyAssets(json) {
    var roles = (json && json.roles) || {};
    var files = (json && json.files) || [];

    /*  角色表里没有的笔刷：拿 id 去 res/pic 的文件名里碰一下（大小写不敏感）。
     *  ★ 这样新丢进 res/pic 的图（比如 hideEntrance）+ 新加的笔刷，
     *    即使服务端还没重启、roles 里还没有这一项，也能直接显示。  */
    function guess(id) {
      var key = String(id).toLowerCase();
      for (var i = 0; i < files.length; i++) {
        if ((files[i].name || "").toLowerCase().indexOf(key) >= 0) {
          var stamp = files[i].mtime_ns || files[i].size || Date.now();
          return files[i].url + "?v=" + stamp;
        }
      }
      return "";
    }

    TILES.forEach(function (tile) {
      if (tile.id === "empty") return;
      var hit = roles[tile.id];
      tile.image = hit ? hit.url : guess(tile.id);
    });
    OBJECT_DEFS.forEach(function (def) {
      var hit = roles[def.id];
      def.image = hit ? hit.url : guess(def.id);
    });
    S.assets = json || null;
  }

  async function loadAssets() {
    if (!S.served) return null;
    try {
      var response = await apiGet("/api/assets");
      var json = await readJson(response);
      if (!json.ok) throw new Error(json.error || "读取失败");
      applyAssets(json);
      if (json.missing && json.missing.length) {
        setStatus("这些笔刷在 " + (json.root || "res/pic") + " 里没找到图，先用色块代替：" + json.missing.join("、"), false);
      }
      return json;
    } catch (e) {
      setStatus("素材列表读取失败：" + e.message, false);
      return null;
    }
  }

  /* 换图 / 挪目录后不用刷新页面：重扫 res/pic，重建两个笔刷面板 */
  async function reloadAssets() {
    if (!S.served) { setStatus("重载素材需要 serve.py", false); return; }
    await loadAssets();
    paletteEl.innerHTML = "";
    objectPaletteEl.innerHTML = "";
    buildPalette();
    buildObjectPalette();
    updateSelection();
    await loadImages();
    refreshInfo();
    scheduleDraw();
    var total = S.assets && S.assets.files ? S.assets.files.length : 0;
    var missing = S.assets && S.assets.missing ? S.assets.missing : [];
    setStatus("已重新扫描 " + ((S.assets && S.assets.root) || "res/pic") + "：" + total + " 张图" +
      (missing.length ? "，缺笔刷：" + missing.join("、") : ""));
  }

  function buildPalette() {
    TILES.forEach(function (t, index) {
      var element = document.createElement("div");
      element.className = "tile";
      element.setAttribute("data-brush", t.code);
      element.setAttribute("data-index", String(index));
      element.title = index + " · " + t.name + "（地图字符 " + t.code + "）";

      var code = document.createElement("span");
      code.className = "code";
      code.textContent = String(index);
      element.appendChild(code);

      if (t.image) {
        var image = document.createElement("img");
        image.src = t.image;
        image.alt = t.name;
        element.appendChild(image);
      } else {
        var swatch = document.createElement("span");
        swatch.style.cssText = "display:block;width:42px;height:42px;background:" + (t.color || "#111");
        element.appendChild(swatch);
      }

      var name = document.createElement("span");
      name.className = "name";
      name.textContent = t.name;
      element.appendChild(name);

      element.addEventListener("click", function () { selectBrush(t.code); });
      paletteEl.appendChild(element);
    });
  }

  function buildObjectPalette() {
    OBJECT_DEFS.forEach(function (def) {
      var element = document.createElement("div");
      element.className = "tile object";
      element.setAttribute("data-object", def.id);
      element.title = def.name + " [" + def.id + "]";

      var code = document.createElement("span");
      code.className = "code";
      code.textContent = String(def.code);
      element.appendChild(code);

      var image = document.createElement("img");
      image.src = def.image;
      image.alt = def.name;
      element.appendChild(image);

      var name = document.createElement("span");
      name.className = "name";
      name.textContent = def.name;
      element.appendChild(name);

      element.addEventListener("click", function () { selectObject(def.id); });
      objectPaletteEl.appendChild(element);
    });
  }

  function buildCornerRow() {
    CORNER_CODES.forEach(function (code, index) {
      var button = document.createElement("button");
      button.type = "button";
      button.textContent = code;
      button.title = CORNER_NAMES[index];
      button.addEventListener("click", function () {
        selectBrush(isCornerBrush(S.brush) ? S.brush : "C");
        setCorner(index);
      });
      cornerRow.appendChild(button);
    });
  }

  function buildAngleRow() {
    for (var i = 0; i < 4; i++) {
      var button = document.createElement("button");
      button.type = "button";
      button.textContent = (i * 90) + "° " + DIRS[i];
      button.title = "墙/地板旋转 " + (i * 90) + "°；门的方向 " + DIRS[i];
      button.setAttribute("data-angle", String(i));
      button.addEventListener("click", (function (index) {
        return function () { setAngle(index); };
      })(i));
      angleRow.appendChild(button);
    }
  }

  function bindUi() {
    canvas.addEventListener("mousedown", onDown);
    canvas.addEventListener("mousemove", onMove);
    canvas.addEventListener("mouseleave", function () { S.hover = null; updateCursor(null); scheduleDraw(); });
    canvas.addEventListener("contextmenu", function (event) { event.preventDefault(); });
    window.addEventListener("mouseup", onUp);

    canvasWrap.addEventListener("wheel", onWheel, { passive: false });

    $("btnFit").addEventListener("click", fit);
    $("btn100").addEventListener("click", function () { zoomAtCenter(1); });
    zoomSlider.addEventListener("input", function () {
      var rect = canvasWrap.getBoundingClientRect();
      setZoom(Number(zoomSlider.value) / 100, true, {
        x: rect.left + rect.width / 2,
        y: rect.top + rect.height / 2
      });
    });

    $("fwView").addEventListener("change", function () {
      S.fwView = this.checked;
      drawPreview();
      scheduleDraw();
    });

    $("btnClear").addEventListener("click", function () {
      if (!window.confirm(S.mode === "layout" ? "清空当前布局的对象？" : "清空整张地图和对象层？")) return;
      pushHistory();
      if (S.mode !== "layout") { S.map = grid("."); S.rot = grid(0); }
      S.objects = [];
      changed();
      setStatus("已清空（可以 Ctrl+Z 撤销）");
    });
    $("btnUndo").addEventListener("click", undo);
    $("btnRedo").addEventListener("click", redo);

    $("btnAssets").addEventListener("click", function () { reloadAssets(); });
    $("btnTplLoad").addEventListener("click", function () { loadTemplate(templateListEl.value); });
    $("btnTplSave").addEventListener("click", saveTemplate);
    $("btnTplDelete").addEventListener("click", deleteTemplate);

    $("btnSize").addEventListener("click", function () {
      var cols = Number(sizeColsEl.value);
      var rows = Number(sizeRowsEl.value);
      if (cols === S.cols && rows === S.rows) {
        setStatus("尺寸没变：" + cols + " × " + rows);
        return;
      }
      if (!window.confirm("画布改成 " + cols + " × " + rows + " 格？当前地图和对象会被清空（Ctrl+Z 可撤销）。")) {
        updateSizeUi();
        return;
      }
      resizeCanvas(cols, rows);
    });
    $("refFrame").addEventListener("change", function () {
      S.showRef = this.checked;
      scheduleDraw();
    });

    $("btnValidate").addEventListener("click", runValidate);
    $("btnSave").addEventListener("click", save);
    $("btnSaveLayout").addEventListener("click", saveNumberedLayout);
    $("btnExportMap").addEventListener("click", exportMap);
    $("btnNewLayout").addEventListener("click", function () { switchWorkspace("layout", S.shape); });
    $("btnLoadLayout").addEventListener("click", loadNumberedLayout);
    $("editMode").addEventListener("change", function () { switchWorkspace(this.value, S.shape); });
    $("poolShape").addEventListener("change", function () { switchWorkspace(S.mode, this.value); });
    $("btnLoad").addEventListener("click", function () { loadName(fileNameEl.value.trim() || "room_map.json"); });
    $("btnDelete").addEventListener("click", deleteFile);
    $("btnEmitC").addEventListener("click", function () { emitC(false); });
    $("btnDownloadC").addEventListener("click", downloadC);
    $("btnDownloadJson").addEventListener("click", downloadJson);
    $("btnCopy").addEventListener("click", copyText);
    $("btnPng").addEventListener("click", exportPng);
    $("btnImportText").addEventListener("click", function () { importRaw(importTextEl.value, "文本框", false); });

    fileListEl.addEventListener("change", function () {
      var name = fileListEl.value;
      if (!name) return;
      if (/\.c$/i.test(name)) cFileNameEl.value = name;
      else fileNameEl.value = name;
    });

    $("filePicker").addEventListener("change", function (event) {
      var file = event.target.files && event.target.files[0];
      if (!file) return;
      var reader = new FileReader();
      reader.onload = function () {
        importTextEl.value = String(reader.result || "");
        importRaw(importTextEl.value, file.name, false);
      };
      reader.onerror = function () { setStatus("读文件失败：" + file.name, false); };
      reader.readAsText(file, "utf-8");
    });

    window.addEventListener("keydown", onKey);
    window.addEventListener("beforeunload", function (event) {
      if (!S.dirty) return;
      event.preventDefault();
      event.returnValue = "";
    });
    window.addEventListener("resize", function () {
      if (S.autoFit || S.zoom * W > canvasWrap.clientWidth - 48) fit();
    });
  }

  function onKey(event) {
    var target = event.target;
    if (target && (target.tagName === "INPUT" || target.tagName === "TEXTAREA" || target.tagName === "SELECT")) return;
    var key = (event.key || "").toLowerCase();

    if (event.ctrlKey || event.metaKey) {
      if (key === "z") { event.preventDefault(); if (event.shiftKey) redo(); else undo(); return; }
      if (key === "y") { event.preventDefault(); redo(); return; }
      if (key === "s") { event.preventDefault(); save(); return; }
      if (key === "0") { event.preventDefault(); zoomAtCenter(1); return; }
      return;
    }

    if (key === "o") selectLayer(S.layer === "tile" ? "object" : "tile");
    else if (key === "b") selectBrush("B");
    else if (key === "k") selectBrush("K");
    else if (key === "s") { /* 旧石头地板，已废弃 */ }
    else if (key === "w") selectBrush("W");
    else if (key === "d") selectBrush("D");
    else if (key === "h") selectBrush("H");
    else if (key === "e") selectBrush(".");
    else if (key === "r") cycle();
    else if (key === "f") fit();
    else if (key === "+" || key === "=") zoomAtCenter(S.zoom * 1.25);
    else if (key === "-") zoomAtCenter(S.zoom / 1.25);
    else if (key >= "0" && key <= "9") {
      var index = Number(key);
      if (S.layer === "object") {
        if (OBJECT_DEFS[index]) selectObject(OBJECT_DEFS[index].id);
      } else if (TILES[index]) {
        selectBrush(TILES[index].code);
      }
    } else return;
    event.preventDefault();
  }

  /* ---------------------------------------------------------------- 启动 */

  function boot() {
    buildCornerRow();
    buildAngleRow();
    bindUi();
    updateSelection();
    updateLayerHint();
    updateAngleHint();
    setZoom(1);
    syncCanvas();
    updateSizeUi();
    if ($("refFrame")) $("refFrame").checked = S.showRef;
    window.requestAnimationFrame(fit);

    var draft = loadDraft();
    var useDraft = !!(draft && (!S.served || draft.dirty));
    if (useDraft) {
      S.mode = draft.mode === "layout" ? "layout" : "map";
      S.shape = ["1x1", "L", "2x2"].indexOf(draft.shape) >= 0 ? draft.shape : "1x1";
      S.layoutId = hasLayoutId(draft.layoutId) ? draft.layoutId : null;
      S.candidateId = hasLayoutId(draft.candidateId) ? draft.candidateId : 1;
      applyState(draft.rows, draft.rot, draft.objects);
      S.history.length = 0;
      S.future.length = 0;
      S.savedText = "";
      setStatus("已恢复本地草稿（有未保存修改）；要读磁盘上的文件请点「读取」。");
    }
    updateWorkspaceControls();

    /* 素材列表 → 笔刷 → 图片，全部来自 res/pic（不再写死文件名） */
    loadAssets().then(function () {
      buildPalette();
      buildObjectPalette();
      updateWorkspaceControls();
      updateSelection();
      return loadImages();
    }).then(function () {
      var missing = TILES.filter(function (t) { return t.image && !images[t.id]; }).map(function (t) { return t.image; })
        .concat(OBJECT_DEFS.filter(function (d) { return d.image && !images[d.id]; }).map(function (d) { return d.image; }));
      if (missing.length) setStatus("素材加载失败：" + missing.join("、") + "（检查 res/pic/）", false);
      refreshInfo();
      scheduleDraw();
    });

    refreshInfo();
    markDirty();
    scheduleDraw();

    if (S.served) {
      apiGet("/api/health").then(function (r) {
        if (!r.ok) throw new Error("HTTP " + r.status);
        setStatus("已连接 serve.py · 页面 " + PAGE_VERSION);
      }).catch(function () {
        setStatus("连不上 serve.py（页面 " + PAGE_VERSION + "）——检查终端里的服务是否还在跑", false);
      });
      refreshList();
      refreshTemplates();
      if (!useDraft) {
        loadWorkspace("layout", "1x1");
      } else refreshLayoutCatalog().catch(function (e) { setStatus(e.message, false); });
      scheduleValidate();
    } else {
      setStatus("file:// 模式：只能编辑和导入导出文本；保存 / 生成 C / 自动校验请用 serve.py 打开。", false);
    }
  }

  boot();
})();
