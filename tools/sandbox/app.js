"use strict";

/* ==========================================================================
   DEBUG 房间编辑器
   --------------------------------------------------------------------------
   · 地图 40×23，32 px/格，画布 1280×720（最后一行 16 px）
   · 编辑结果保存成 output/room_map.txt；生成 C 写 output/game_world_map.c
   · 校验 / C 生成 / 解析都在 mapfmt.py（服务端），前端只负责画和发请求
   ========================================================================== */
(function () {
  var COLS = 40;
  var ROWS = 23;
  var TILE = 32;
  var W = 1280;
  var H = 720;
  var ARENA = { x0: 288, y0: 128, x1: 1024, y1: 544 };
  var ASSET_BASE = "../../res/pic/base/background/";
  var CORNER_CODES = ["C", "X", "Y", "Z"];
  var CORNER_NAMES = ["C · 原样", "X · 水平翻", "Y · 垂直翻", "Z · 双向翻"];
  var DIRS = ["N", "E", "S", "W"];
  var CODES = [".", "B", "W", "D", "C", "X", "Y", "Z"];
  var DOOR_SLOTS = [[20, 2, 0], [32, 10, 1], [20, 17, 2], [7, 10, 3]];

  var TILES = [
    { id: "empty", code: ".", name: "空白", color: "#090e14" },
    { id: "floor", code: "B", name: "基岩地板", image: "minecraft_bedrock.png" },
    { id: "wall", code: "W", name: "木墙", image: "minecraft_wood.png" },
    { id: "door", code: "D", name: "门（2 格）", image: "background_door_normal.png" },
    { id: "corner", code: "C", name: "角块", image: "background_corner_wood.png" }
  ];

  function $(id) { return document.getElementById(id); }

  var canvas = $("map");
  var ctx = canvas.getContext("2d");
  var preview = $("preview");
  var pctx = preview.getContext("2d");
  var paletteEl = $("palette");
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
  var fileListEl = $("fileList");
  var alsoEmitEl = $("alsoEmit");
  var zoomSlider = $("zoomSlider");
  var zoomReadout = $("zoomReadout");
  var canvasWrap = $("canvasWrap");
  var images = {};

  var S = {
    map: grid("."),
    rot: grid(0),
    brush: "W",
    corner: 0,
    angle: 0,
    zoom: 1,
    fwView: false,
    history: [],
    future: [],
    pending: null,
    beforeText: "",
    savedText: "",
    dirty: false,
    file: "room_map.txt",
    cfile: "game_world_map.c",
    lastC: "",
    issueCells: {},
    issues: [],
    flash: null,
    hover: null,
    drawing: false,
    erase: false,
    lastCell: null,
    served: /^https?:$/.test(location.protocol)
  };

  /* ---------------------------------------------------------------- 工具 */

  function grid(value) {
    var out = [];
    for (var y = 0; y < ROWS; y++) {
      var row = [];
      for (var x = 0; x < COLS; x++) row.push(value);
      out.push(row);
    }
    return out;
  }

  function cloneGrid(g) {
    var out = [];
    for (var y = 0; y < g.length; y++) out.push(g[y].slice());
    return out;
  }

  function inBounds(x, y) { return x >= 0 && x < COLS && y >= 0 && y < ROWS; }

  function doorPair(x, y, angle) { return (angle & 1) ? [x + 1, y] : [x, y + 1]; }

  function tileOf(code) {
    for (var i = 0; i < TILES.length; i++) if (TILES[i].code === code) return TILES[i];
    if (CORNER_CODES.indexOf(code) >= 0) return TILES[4];
    return TILES[0];
  }

  function currentCode() { return S.brush === "C" ? CORNER_CODES[S.corner] : S.brush; }

  function cloneState() { return { map: cloneGrid(S.map), rot: cloneGrid(S.rot) }; }

  function applyState(rows, rot) {
    S.map = rows.map(function (row) { return row.split(""); });
    S.rot = rot.map(function (row) { return row.slice(0, COLS); });
    S.hover = null;
    S.flash = null;
  }

  function serialize() {
    var body = [];
    var rot = [];
    for (var y = 0; y < ROWS; y++) {
      body.push(S.map[y].join(""));
      rot.push(S.rot[y].map(function (v) { return String(v & 3); }).join(""));
    }
    return "# DEBUG_ROOM_MAP v2\n" + body.join("\n") + "\n# ROTATION\n" + rot.join("\n") + "\n";
  }

  function parseLocal(raw) {
    var text = String(raw).replace(/^\uFEFF/, "").replace(/\r\n/g, "\n").replace(/\r/g, "\n");
    var lines = text.split("\n");
    var marker = -1;
    for (var i = 0; i < lines.length; i++) {
      if (lines[i].trim().toUpperCase() === "# ROTATION") { marker = i; break; }
    }
    var head = marker >= 0 ? lines.slice(0, marker) : lines;
    var rowsText = [];
    for (var j = 0; j < head.length; j++) {
      var line = head[j].replace(/\s+$/, "");
      if (!line) continue;
      var t = line.trim();
      if (t.charAt(0) === "#") continue;
      rowsText.push(line);
    }
    if (rowsText.length !== ROWS) throw new Error("地图需要 " + ROWS + " 行，实际 " + rowsText.length + " 行");
    var rows = [];
    for (var y = 0; y < ROWS; y++) {
      if (rowsText[y].length !== COLS) throw new Error("第 " + (y + 1) + " 行长度 " + rowsText[y].length + "，需要 " + COLS);
      for (var x = 0; x < COLS; x++) {
        if (".BWDCXYZ".indexOf(rowsText[y].charAt(x)) < 0) throw new Error("第 " + (y + 1) + " 行第 " + (x + 1) + " 列有非法字符");
      }
      rows.push(rowsText[y]);
    }
    var rot = grid(0);
    if (marker >= 0) {
      var rotLines = [];
      for (var k = marker + 1; k < lines.length; k++) {
        var rl = lines[k].trim();
        if (rl && rl.charAt(0) !== "#") rotLines.push(rl);
      }
      if (!rotLines.length) throw new Error("# ROTATION 后面没有内容");
      var sparse = /[\s,]/.test(rotLines[0]);
      if (sparse) {
        for (var s = 0; s < rotLines.length; s++) {
          var parts = rotLines[s].split(/[\s,]+/);
          if (parts.length !== 3) throw new Error("稀疏旋转要写成 x,y,r");
          var sx = +parts[0], sy = +parts[1], sr = +parts[2];
          if (!(sx >= 0 && sx < COLS && sy >= 0 && sy < ROWS && sr >= 0 && sr <= 3)) throw new Error("稀疏旋转越界");
          rot[sy][sx] = sr;
        }
      } else {
        if (rotLines.length < ROWS) throw new Error("旋转表需要 " + ROWS + " 行");
        for (var ry = 0; ry < ROWS; ry++) {
          if (rotLines[ry].length !== COLS) throw new Error("旋转表第 " + (ry + 1) + " 行长度不对");
          for (var rx = 0; rx < COLS; rx++) {
            var ch = rotLines[ry].charAt(rx);
            if ("0123".indexOf(ch) < 0) throw new Error("旋转表第 " + (ry + 1) + " 行有非 0..3 字符");
            rot[ry][rx] = +ch;
          }
        }
      }
    }
    return { rows: rows, rot: rot };
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
    S.map = cloneGrid(snap.map);
    S.rot = cloneGrid(snap.rot);
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
    if (!inBounds(x, y)) return;
    if (code === "D") { placeDoor(x, y); return; }
    removeDoorAt(x, y);
    S.map[y][x] = code;
    S.rot[y][x] = code === "." ? 0 : (S.angle & 3);
  }

  function placeDoor(x, y) {
    var angle = S.angle & 3;
    var p = doorPair(x, y, angle);
    if (!inBounds(p[0], p[1])) {
      setStatus("门需要两格，这个位置放不下（" + DIRS[angle] + " 方向）", false);
      return;
    }
    removeDoorAt(x, y);
    removeDoorAt(p[0], p[1]);
    S.map[y][x] = "D";
    S.rot[y][x] = angle;
    S.map[p[1]][p[0]] = ".";
    S.rot[p[1]][p[0]] = 0;
  }

  function removeDoorAt(x, y) {
    if (!inBounds(x, y)) return;
    if (S.map[y][x] === "D") {
      var p = doorPair(x, y, S.rot[y][x]);
      S.map[y][x] = ".";
      S.rot[y][x] = 0;
      if (inBounds(p[0], p[1])) { S.map[p[1]][p[0]] = "."; S.rot[p[1]][p[0]] = 0; }
      return;
    }
    var dirs = [[1, 0], [-1, 0], [0, 1], [0, -1]];
    for (var i = 0; i < dirs.length; i++) {
      var nx = x + dirs[i][0], ny = y + dirs[i][1];
      if (!inBounds(nx, ny) || S.map[ny][nx] !== "D") continue;
      var q = doorPair(nx, ny, S.rot[ny][nx]);
      if (q[0] === x && q[1] === y) { S.map[ny][nx] = "."; S.rot[ny][nx] = 0; }
    }
  }

  function paintAt(p) {
    if (S.erase) {
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
    var fx = code === "X" || code === "Z";
    var fy = code === "Y" || code === "Z";
    drawTileImage(target, images[t.id], px, py, TILE, drawAngle(S.rot[y][x]), fx, fy);
  }

  function paintDoor(target, x, y) {
    var angle = S.rot[y][x] & 3;
    var horiz = (angle & 1) !== 0;
    var px = x * TILE, py = y * TILE;
    var fw = horiz ? 2 * TILE : TILE;
    var fh = horiz ? TILE : 2 * TILE;
    var spriteAngle = drawAngle(angle);

    if (images.wall) {
      for (var i = 0; i < 2; i++) {
        var wx = px + (horiz ? i * TILE : 0);
        var wy = py + (horiz ? 0 : i * TILE);
        drawTileImage(target, images.wall, wx, wy, TILE, spriteAngle, false, false);
      }
    }
    if (!images.door) return;

    var img = images.door;
    var dw = img.naturalWidth || 40;
    var dh = img.naturalHeight || 64;
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

  function drawGrid(target) {
    var x, y, lx, ly;
    target.save();
    target.lineWidth = 1;
    target.strokeStyle = "rgba(210,230,250,.10)";
    target.beginPath();
    for (x = 0; x <= COLS; x++) {
      lx = x * TILE + 0.5;
      target.moveTo(lx, 0);
      target.lineTo(lx, H);
    }
    for (y = 0; y <= ROWS; y++) {
      ly = y * TILE + 0.5;
      if (ly > H) break;
      target.moveTo(0, ly);
      target.lineTo(W, ly);
    }
    target.stroke();

    target.strokeStyle = "rgba(90,170,255,.20)";
    target.beginPath();
    for (x = 0; x <= COLS; x += 4) {
      lx = x * TILE + 0.5;
      target.moveTo(lx, 0);
      target.lineTo(lx, H);
    }
    for (y = 0; y <= ROWS; y += 4) {
      ly = y * TILE + 0.5;
      if (ly > H) break;
      target.moveTo(0, ly);
      target.lineTo(W, ly);
    }
    target.stroke();
    target.restore();
  }

  function drawOverlay(target) {
    target.save();

    target.strokeStyle = "rgba(89,169,255,.55)";
    target.lineWidth = 2;
    target.strokeRect(ARENA.x0 + 1, ARENA.y0 + 1, ARENA.x1 - ARENA.x0 - 2, ARENA.y1 - ARENA.y0 - 2);

    target.setLineDash([5, 5]);
    target.strokeStyle = "rgba(89,169,255,.30)";
    target.lineWidth = 1;
    for (var i = 0; i < DOOR_SLOTS.length; i++) {
      var slot = DOOR_SLOTS[i];
      var horiz = (slot[2] & 1) !== 0;
      target.strokeRect(slot[0] * TILE + 0.5, slot[1] * TILE + 0.5, horiz ? 2 * TILE - 1 : TILE - 1, horiz ? TILE - 1 : 2 * TILE - 1);
    }
    target.setLineDash([]);

    for (var key in S.issueCells) {
      if (!Object.prototype.hasOwnProperty.call(S.issueCells, key)) continue;
      var p = key.split(",");
      target.strokeStyle = S.issueCells[key] === "error" ? "rgba(255,110,110,.95)" : "rgba(231,200,106,.9)";
      target.lineWidth = 2;
      target.strokeRect(Number(p[0]) * TILE + 1, Number(p[1]) * TILE + 1, TILE - 2, TILE - 2);
    }

    if (S.hover) {
      target.strokeStyle = "rgba(255,255,255,.55)";
      target.lineWidth = 1;
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

    for (var y = 0; y < ROWS; y++) {
      for (var x = 0; x < COLS; x++) {
        var code = S.map[y][x];
        if (code !== "." && code !== "D") paintCell(ctx, code, x, y);
      }
    }
    for (var dy = 0; dy < ROWS; dy++) {
      for (var dx = 0; dx < COLS; dx++) {
        if (S.map[dy][dx] === "D") paintDoor(ctx, dx, dy);
      }
    }
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

    var code = currentCode();
    var t = tileOf(code);
    var angle = drawAngle(S.angle);
    var fx = code === "X" || code === "Z";
    var fy = code === "Y" || code === "Z";

    if (code === ".") {
      pctx.fillStyle = t.color;
      pctx.fillRect(16, 16, 64, 64);
    } else if (images[t.id]) {
      var img = images[t.id];
      var dw = img.naturalWidth || 32;
      var dh = img.naturalHeight || 32;
      var scale = 80 / Math.max(dw, dh);
      var ow = dw * scale;
      var oh = dh * scale;
      pctx.save();
      pctx.translate(48, 48);
      pctx.rotate(angle * Math.PI / 2);
      if (fx) pctx.scale(-1, 1);
      if (fy) pctx.scale(1, -1);
      pctx.imageSmoothingEnabled = false;
      pctx.drawImage(img, -ow / 2, -oh / 2, ow, oh);
      pctx.restore();
    }

    var text = t.name + "\n字符：" + code;
    if (code === "D") text += "\n方向：" + DIRS[S.angle & 3] + "（" + (S.angle & 3) * 90 + "°）";
    else if (t.id === "corner") text += "\n翻转：" + CORNER_NAMES[S.corner] + "\n旋转：" + (S.angle & 3) * 90 + "°";
    else if (code !== ".") text += "\n旋转：" + (S.angle & 3) * 90 + "°";
    $("previewText").textContent = text;
  }

  function refreshInfo() {
    var counts = {};
    for (var i = 0; i < CODES.length; i++) counts[CODES[i]] = 0;
    for (var y = 0; y < ROWS; y++) {
      for (var x = 0; x < COLS; x++) counts[S.map[y][x]] += 1;
    }
    var html = "";
    for (var k = 0; k < CODES.length; k++) {
      var code = CODES[k];
      html += '<div class="count"><span>' + code + " · " + tileOf(code).name + "</span><b>" + counts[code] + "</b></div>";
    }
    countsEl.innerHTML = html;
    mapTextEl.value = serialize();
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
    if (code === "D") text += " · " + DIRS[S.rot[p.y][p.x] & 3];
    else if (code !== ".") text += " · " + (S.rot[p.y][p.x] & 3) * 90 + "°";
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
      tiles[i].classList.toggle("selected", tiles[i].getAttribute("data-brush") === S.brush);
    }
    var cbtns = cornerRow.querySelectorAll("button");
    for (var c = 0; c < cbtns.length; c++) cbtns[c].classList.toggle("on", S.brush === "C" && c === S.corner);
    var abtns = angleRow.querySelectorAll("button");
    for (var a = 0; a < abtns.length; a++) abtns[a].classList.toggle("on", a === (S.angle & 3));
  }

  function selectBrush(code) {
    S.brush = code;
    updateSelection();
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
    if (S.brush === "C") setCorner(S.corner + 1);
    else setAngle(S.angle + 1);
  }

  function updateAngleHint() {
    var hint = $("angleHint");
    if (S.brush === "D") hint.textContent = "门：角度就是方向 0=N 1=E 2=S 3=W；D 只占一格，另一格会自动清空。";
    else if (S.brush === "C") hint.textContent = "角块先翻转再旋转；旋转值会写进 rot 表，和 C 侧的 corner_transform 一致。";
    else hint.textContent = "墙和地板按这个角度旋转；门的旋转值是方向，不是贴图角度。";
  }

  /* ---------------------------------------------------------------- 视图 */

  function setZoom(value, manual) {
    S.zoom = Math.max(0.35, Math.min(2.5, value));
    canvas.style.width = (W * S.zoom) + "px";
    canvas.style.height = (H * S.zoom) + "px";
    zoomSlider.value = String(Math.round(S.zoom * 100));
    zoomReadout.textContent = Math.round(S.zoom * 100) + "%";
    if (manual) S.autoFit = false;
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

  async function save() {
    if (!S.served) { setStatus("保存需要 serve.py 打开页面", false); return; }
    S.file = fileNameEl.value.trim() || "room_map.txt";
    setStatus("保存中…", "busy");
    var response = await apiPost("/api/save?name=" + encodeURIComponent(S.file), serialize());
    var json = await readJson(response);
    if (!json.ok) {
      if (json.issues) renderIssues(json.issues);
      setStatus("保存失败：" + (json.error || "未知错误"), false);
      return;
    }
    S.savedText = json.text;
    markDirty();
    saveDraftNow(false);
    if (json.issues) renderIssues(json.issues);
    var message = "已保存 output/" + json.file;
    if (alsoEmitEl.checked) {
      var ok = await emitC(true);
      if (ok) message += "，并生成 output/" + S.cfile;
    }
    await refreshList();
    refreshInfo();
    setStatus(message);
  }

  async function emitC(quiet) {
    if (!S.served) { if (!quiet) setStatus("生成 C 需要 serve.py", false); return false; }
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
      data = { rows: json.rows, rot: json.rot, issues: json.issues || [], format: json.format };
    } else {
      if (/room_map_shared|room_rot_shared/.test(text)) {
        setStatus("导入 C 需要 serve.py；file:// 下只能导入 txt", false);
        return false;
      }
      try {
        var parsed = parseLocal(text);
        data = {
          rows: parsed.rows,
          rot: parsed.rot,
          issues: [],
          format: "text"
        };
      } catch (e) {
        setStatus("导入失败：" + e.message, false);
        return false;
      }
    }
    if (record !== false) pushHistory();
    applyState(data.rows, data.rot);
    if (fromDisk) { S.savedText = serialize(); }
    renderIssues(data.issues);
    refreshInfo();
    markDirty();
    scheduleDraw();
    saveDraftSoon();
    setStatus("已导入" + (data.format === "c" ? " C" : "文本") + (label ? "：" + label : ""));
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

  function downloadTxt() {
    downloadBlob(serialize(), S.file, "text/plain;charset=utf-8");
    setStatus("已下载 " + S.file);
  }

  function copyText() {
    var text = serialize();
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(text).then(function () { setStatus("已复制地图文本"); },
        function () { mapTextEl.select(); setStatus("浏览器不让写剪贴板，已选中文本框，Ctrl+C 复制", false); });
    } else {
      mapTextEl.select();
      setStatus("已选中文本框，Ctrl+C 复制", false);
    }
  }

  function exportPng() {
    var anchor = document.createElement("a");
    anchor.href = canvas.toDataURL("image/png");
    anchor.download = S.file.replace(/\.txt$/i, "") + ".png";
    document.body.appendChild(anchor);
    anchor.click();
    document.body.removeChild(anchor);
    setStatus("已导出 PNG（当前画布，含网格和校验框）");
  }

  /* ---------------------------------------------------------------- 草稿 */

  var draftTimer = null;
  function saveDraftNow(dirty) {
    try {
      localStorage.setItem("debug_room_map", JSON.stringify({ map: S.map, rot: S.rot, dirty: !!dirty }));
    } catch (e) { /* 隐私模式 / 配额满，忽略 */ }
  }

  function saveDraftSoon() {
    if (draftTimer) clearTimeout(draftTimer);
    draftTimer = setTimeout(function () { saveDraftNow(S.dirty); }, 300);
  }

  function loadDraft() {
    try {
      var raw = localStorage.getItem("debug_room_map");
      if (!raw) return null;
      var data = JSON.parse(raw);
      var map = Array.isArray(data) ? data : data.map;
      var rot = Array.isArray(data) ? null : data.rot;
      if (!Array.isArray(map) || map.length !== ROWS) return null;
      var rows = [];
      for (var y = 0; y < ROWS; y++) {
        if (!Array.isArray(map[y]) || map[y].length !== COLS) return null;
        rows.push(map[y].join(""));
      }
      var rotGrid = grid(0);
      if (Array.isArray(rot) && rot.length === ROWS) {
        for (var ry = 0; ry < ROWS; ry++) {
          if (!Array.isArray(rot[ry])) continue;
          for (var rx = 0; rx < COLS && rx < rot[ry].length; rx++) rotGrid[ry][rx] = Number(rot[ry][rx]) & 3;
        }
      }
      var dirty = Array.isArray(data) ? true : data.dirty !== false;
      return { rows: rows, rot: rotGrid, dirty: dirty };
    } catch (e) {
      return null;
    }
  }

  /* ---------------------------------------------------------------- 图片 */

  function loadImages() {
    var jobs = TILES.filter(function (t) { return t.image; }).map(function (t) {
      return new Promise(function (resolve) {
        var image = new Image();
        image.onload = function () { images[t.id] = image; resolve(); };
        image.onerror = function () { images[t.id] = null; resolve(); };
        image.src = ASSET_BASE + t.image;
      });
    });
    return Promise.all(jobs);
  }

  /* ---------------------------------------------------------------- UI */

  function buildPalette() {
    TILES.forEach(function (t) {
      var element = document.createElement("div");
      element.className = "tile";
      element.setAttribute("data-brush", t.code);
      element.title = t.name + " [" + t.code + "]";

      var code = document.createElement("span");
      code.className = "code";
      code.textContent = t.code;
      element.appendChild(code);

      if (t.image) {
        var image = document.createElement("img");
        image.src = ASSET_BASE + t.image;
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

  function buildCornerRow() {
    CORNER_CODES.forEach(function (code, index) {
      var button = document.createElement("button");
      button.type = "button";
      button.textContent = code;
      button.title = CORNER_NAMES[index];
      button.addEventListener("click", function () { selectBrush("C"); setCorner(index); });
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

    canvasWrap.addEventListener("wheel", function (event) {
      event.preventDefault();
      setZoom(S.zoom * (event.deltaY < 0 ? 1.1 : 1 / 1.1), true);
    }, { passive: false });

    $("btnFit").addEventListener("click", fit);
    $("btn100").addEventListener("click", function () { setZoom(1, true); });
    $("btn150").addEventListener("click", function () { setZoom(1.5, true); });
    zoomSlider.addEventListener("input", function () { setZoom(Number(zoomSlider.value) / 100, true); });

    $("fwView").addEventListener("change", function () {
      S.fwView = this.checked;
      drawPreview();
      scheduleDraw();
    });

    $("btnClear").addEventListener("click", function () {
      if (!window.confirm("清空整张地图？")) return;
      pushHistory();
      S.map = grid(".");
      S.rot = grid(0);
      changed();
      setStatus("已清空（可以 Ctrl+Z 撤销）");
    });
    $("btnUndo").addEventListener("click", undo);
    $("btnRedo").addEventListener("click", redo);

    $("btnValidate").addEventListener("click", runValidate);
    $("btnSave").addEventListener("click", save);
    $("btnLoad").addEventListener("click", function () { loadName(fileNameEl.value.trim() || "room_map.txt"); });
    $("btnRefresh").addEventListener("click", refreshList);
    $("btnDelete").addEventListener("click", deleteFile);
    $("btnEmitC").addEventListener("click", function () { emitC(false); });
    $("btnDownloadC").addEventListener("click", downloadC);
    $("btnDownloadTxt").addEventListener("click", downloadTxt);
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
      return;
    }

    if (key === "b") selectBrush("B");
    else if (key === "w") selectBrush("W");
    else if (key === "d") selectBrush("D");
    else if (key === "e") selectBrush(".");
    else if (key === "r") cycle();
    else if (key === "0") fit();
    else if (key === "+" || key === "=") setZoom(S.zoom + 0.25, true);
    else if (key === "-") setZoom(S.zoom - 0.25, true);
    else if (key >= "1" && key <= "4") {
      if (S.brush === "C") setCorner(Number(key) - 1);
      else setAngle(Number(key) - 1);
    } else return;
    event.preventDefault();
  }

  /* ---------------------------------------------------------------- 启动 */

  function boot() {
    buildPalette();
    buildCornerRow();
    buildAngleRow();
    bindUi();
    updateSelection();
    updateAngleHint();
    setZoom(1);
    window.requestAnimationFrame(fit);

    var draft = loadDraft();
    var useDraft = !!(draft && (!S.served || draft.dirty));
    if (useDraft) {
      applyState(draft.rows, draft.rot);
      S.history.length = 0;
      S.future.length = 0;
      S.savedText = "";
      setStatus("已恢复本地草稿（有未保存修改）；要读磁盘上的文件请点「读取」。");
    }

    loadImages().then(function () {
      var missing = TILES.filter(function (t) { return t.image && !images[t.id]; }).map(function (t) { return t.image; });
      if (missing.length) setStatus("素材加载失败：" + missing.join("、") + "（检查 res/pic/base/background/）", false);
      refreshInfo();
      scheduleDraw();
    });

    refreshInfo();
    markDirty();
    scheduleDraw();

    if (S.served) {
      refreshList();
      if (!useDraft) loadName("room_map.txt", true, false);
      scheduleValidate();
    } else {
      setStatus("file:// 模式：只能编辑和导入导出文本；保存 / 生成 C / 自动校验请用 serve.py 打开。", false);
    }
  }

  boot();
})();
