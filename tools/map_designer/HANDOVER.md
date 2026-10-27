# MAP DESIGNER 交接文档

目录：tools/map_designer/
页面版本：2026-10-25d
日期：2026-10-25

## 当前接手状态（优先于下文历史记录）

- 最新范围（页面 2026-10-25f）：仅普通 1×1、L、2×2；特殊房暂不编辑。
  地图/布局两种模式分开，布局锁定地形；编号方案存 `output/layouts/<房型>_001.json`，
  地图导出存 `output/maps/<房型>.json`，不自动替换正式 maps/。
- `1x1_000.json` 是起点专用空布局；普通房只从正编号方案确定性选择，旧非空参考布局不会再作为起点。
- 编号由服务端在各房型内递增分配，文件名与 map/layout_id/name 必须一致。
  生成的 g_layout_ids 保存稳定设计编号，与 C 数组下标分离；有编号方案时跳过旧参考方案。
  起始/隐藏/Boss 使用原指定布局，道具房/商店走原内容逻辑，防止混入普通方案。
- 普通 2×2 暂只支持设计/导出；当前生成器的 2×2 是 Boss 专用，本轮未新增普通大房拓扑。
  新增编号接口需重启 serve.py。完整流程与测试命令见 README 顶部。

- 布局池适配（页面 2026-10-25e）：`maps/` 持有固定空间，内置模板直接读取；L 设计只用左下空缺。
  「保存为布局池方案」生成 `map_designer_layout` v1，只记录 map 与 objects；旧完整 JSON 兼容读取。
- 四份正式 output 已转为紧凑方案，原地形完整移入 maps/，没有改变房间空间与现有对象。
  批量应用会拒绝不匹配固定地图池的地形；多对象方案共享逐格地图，不再重复导出地形表。
- 运行时按固定空间自动选方案：普通/L/隐藏/Boss 接入，起点固定，道具房/商店保持现有专用逻辑。
  L 对象生成、绘制、碰撞、炸碎擦除及换房旧覆盖标记已接入镜像，破坏键仍使用原坐标。
  上板前由用户重新编译固件；不需要为纯对象布局更新素材或 FPGA bit 流。

- 正式贴图已接通设计器 → JSON → project C 工程。旧 Minecraft 图片已停用，旧 S 存档仍可解析，编辑器按木地板显示。
- 普通门 D 占两格，第二格是空白；隐藏入口 H 只占靠地板的一格，外侧保留隐藏房墙。预览、旋转、校验、生成和运行时触发位置均按此区分。
- 隐藏房使用 K/w/c/x/y/z；标准隐藏房导出到 map=2 参数框架，修改过地形或角块的房间按原图导出地形表。对象仍是独立 objects 层。
- 左侧为贴图、预览、状态和校验建议；右侧为文件、模板与导入导出。移除重复图层按钮、150% 快捷按钮和旧 TXT 导出，旧格式导入仍保留。
- 三套内置模板使用 B 木地板。正式素材是 background_floor_wood/hidden、background_wall_wood/hidden、background_corner_wood/hidden，以及 background_interact_hidden。
- 布局生成默认只写 project；code 是归档，只有显式 --target archive/all 才会写。
- 滚轮在画布任意位置以光标为中心双向缩放，范围 10%–800%，支持横向滚轮与触控板。
- 验证：Python 74 项（1 项跳过）、Node 编辑器回归、Chrome 九位置双向缩放、C 门与镜像检查均通过，尚需上板验证。测试使用独立框架数据，不依赖用户正在编辑的 output 地图。
- 下文 2026-10-24 的 S/Minecraft 素材、两份同步导出等内容是历史状态，以此节及 README 为准。

## 1. 当前状态

这是一个给 FPGA 游戏 demo 用的房间设计器：

- 画布 8x8 到 45x35 格，默认 40x23，32 px/格；
- 地形层：空、木地板、木墙、门、角块、隐藏门、基岩、石头；
- 对象层：小怪出生点、蜘蛛网、石头、木箱、板条箱、出口；
- 保存为 JSON v4（数字笔刷），可重新加载；
- 可选导出 C 数据 / 布局数据；
- 支持 L 形 / 凹形房（异形地形）；
- 模板内置 1×1 / L / 2×2，也可以把当前图直接存成自定义模板。

当前服务地址：

    http://127.0.0.1:8766/tools/map_designer/index.html

注意：8765 默认是 tools/item_designer，两个工具不要抢同一端口。

## 2. 启动和测试

在仓库根目录：

    uv run tools/map_designer/serve.py --open

默认端口 8766；被占用会自动试 8767 到 8775，并打印实际地址。

检查生成物（默认找 output/room_map.json；没有就指定一个）：

    uv run tools/map_designer/serve.py --check --map tools/map_designer/output/map_1x1.json

跑测试：

    uv run python -m unittest discover -s tools/map_designer -p "test_*.py" -v

当前结果：66 个测试 OK，1 个 skipped。

## 3. 文件职责

    index.html                 页面骨架
    style.css                  深色三栏样式
    app.js                     画布、地形/对象编辑、缩放、撤销、导入导出
    serve.py                   本地服务、CLI、素材扫描
    mapfmt.py                  格式解析、校验、JSON/C/布局生成（唯一实现）
    check_game_layout.py       检查生成布局的对象位置
    test_mapfmt.py             单元测试
    templates/                 内置 1x1 / 2x2 / L 模板（JSON v4）
    templates/custom/          自定义模板（页面保存到这里的 custom_*.json）
    output/                    当前房间存档（JSON v4）
    output/_archive/           旧文件归档

## 4. 存档格式 v4（数字笔刷）

保存按钮只写一个 JSON，不再写 .txt。

示例：

~~~json
{
  "format": "map_designer",
  "version": 4,
  "name": "map_1x1",
  "size": {"cols": 40, "rows": 23, "tile": 32},
  "tile_encoding": "index",
  "tiles": [
    [0, 0, 0, 3, 7, 7],
    [0, 0, 0, 3, 7, 7]
  ],
  "rotation": [
    [0, 0, 0, 0, 0, 0]
  ],
  "objects": [
    {"kind": "slime", "x": 12, "y": 8, "arg": 0}
  ],
  "sha256": "..."
}
~~~

tiles 每格一个整数，tile_encoding 固定 index。

数字对应关系：

    0  空
    1  木地板（旧字母 B）
    2  木墙（旧字母 W）
    3  门（旧字母 D）
    4  角块原样（旧字母 C）
    5  隐藏门（旧字母 H）
    6  基岩地板（旧字母 K）
    7  石头地板（旧字母 S）
    8  角块 X
    9  角块 Y
    10 角块 Z

8 到 10 只是角块翻转变体，面板上仍是一个角块笔刷，用 R 循环。

排版规则：tiles 和 rotation 一行一格，不把每个数字拆成单独一行。

兼容性：

- 旧 JSON 的 tiles 字符串仍能导入；
- 旧 v1 / v2 / v3 文本仍能导入；
- 旧 C 源码仍能导入；
- .txt 只是旧格式导出，按钮标了「旧版」，不是主存档。

## 5. 模板

### 5.1 内置模板

    template_1x1    现场生成                       地板 S 石头
    template_2x2    templates/template_2x2.json    地板 S 石头
    template_L      templates/template_L.json      地板 S 石头

三套内置模板已统一成石头地板 S，不是木地板 B。

因此它们不再是标准矩形框架，导出布局时会带逐格地形表。

--emit-frame 只对纯 B 标准框架有效；模板要出帧参数，先把 S 换回 B。

### 5.2 自定义模板

右栏「模板」区域：

1. 画好一张图（地形、旋转、对象都会保存）；
2. 填「文件名」，例如 my_l_room，只允许字母 / 数字 / 下划线 / 短横线；
3. 填「显示名」，可以中文，例如 我的 L 房；不填就用文件名；
4. 点「当前图存为模板」。

实际写入：

    templates/custom/custom_<文件名>.json

同名再存会覆盖，并且只覆盖自定义目录，不会碰内置模板。

页面上的模板下拉框会同时列出内置和自定义模板：

- 内置模板：可以选择、载入，不能删除；
- 自定义模板：可以载入、覆盖、删除；
- 列表读取 / 保存 / 删除都走服务端 API，不用改 mapfmt.py，也不用重启服务。

自定义模板仍然是普通的 JSON v4，只是多一个可选的 label 字段：

~~~json
{
  "format": "map_designer",
  "version": 4,
  "name": "custom_my_l_room",
  "label": "我的 L 房",
  "size": {"cols": 40, "rows": 23, "tile": 32},
  "tile_encoding": "index",
  "tiles": [[0, 1, 7]],
  "rotation": [[0, 0, 0]],
  "objects": [],
  "sha256": "..."
}
~~~

label 只用于显示，parse_json 会忽略它；手工复制 / 编辑 templates/custom/*.json 也能被识别。

删除只能删 custom_ 开头的文件；直接删 templates/ 下的内置 JSON 会让模板列表显示「损坏」，不要把内置模板当草稿改。

## 6. 编辑器操作

地形层数字键按顺序：

    0 空
    1 木地板
    2 木墙
    3 门
    4 角块
    5 隐藏门
    6 基岩
    7 石头

对象层数字键：

    0 slime
    1 bug
    2 web
    3 stone
    4 box
    5 crate
    6 exit

其他操作：

- 左键画，右键擦；
- R 循环旋转；角块时循环 C/X/Y/Z；
- O 切地形 / 对象层；
- F 适应窗口；
- 滚轮以鼠标为中心自由缩放 10% 到 800%；
- 滑块同样 10% 到 800%；
- Ctrl+0 回到 100%；
- + / - 按 1.25 倍缩放；
- Ctrl+Z 撤销，Ctrl+Y 重做，Ctrl+S 保存。

模板操作：

- 下拉框选模板，点「载入」；
- 填文件名 / 显示名，点「当前图存为模板」；
- 选中自定义模板，点「删除」；
- 内置模板不能删除，也不能被同名覆盖。

存档说明：

- 保存只写 output/<name>.json；
- 保存允许半成品（有 error 也能存），生成 C / 布局时才拦 error；
- 「保存时同时生成 C」默认关闭，C 是可选导出；
- 页面 JSON 文本框显示的就是当前保存格式（数字），不是旧字母文本。

## 7. 校验规则

mapfmt.validate() 负责，页面只显示结果。

- 尺寸 8x8 到 45x35；
- 内部字符必须在 . B W D H K S C X Y Z 内；
- 竞技场 = 地板 B/K/S 的包围盒；
- 矩形房：门必须在 2 格墙带里，方向 0=N / 1=E / 2=S / 3=W；
- 门两格：D 占一格，另一格必须是 .；
- 墙带空洞提示；
- 对象必须在竞技场内的 B/K/S 上；
- 对象重复 / 阻挡物重叠 / 数量超限给 warning；
- L 形 / 凹形：包围盒里有非地板、非门时判为异形，只给一条提示；
  不再逐格报「会挡住玩家」，不再按矩形墙带报漏洞，不再强制 4 扇门。

## 8. 服务端 API

    GET  /api/list                 output 下 .txt/.json/.c
    GET  /api/load?name=           读文件
    GET  /api/assets               扫 res/pic，返回角色和 URL（带 mtime 防缓存）
    GET  /api/templates            内置 + 自定义模板列表
    GET  /api/template?size=       取模板（内置 id 或 custom_*）
    GET  /api/health               健康检查
    POST /api/save?name=           校验并写 JSON（允许半成品）
    POST /api/template_save?name=&label=   当前图存成自定义模板
    POST /api/template_delete?name=        删除自定义模板（内置拒绝）
    POST /api/validate             校验文本 / JSON / C
    POST /api/import               解析成 rows/rot/objects
    POST /api/emit_c?name=&map=    生成可选 C 数据
    POST /api/delete?name=         删除 output 文件

静态只放行 /tools/map_designer/ 和 /res/。

## 9. CLI

    # 起服务
    uv run tools/map_designer/serve.py --open

    # 只检查 JSON / C 是否最新
    uv run tools/map_designer/serve.py --check

    # 从地图重新生成约定 JSON
    uv run tools/map_designer/serve.py --emit-json

    # 可选：生成单张地图 C
    uv run tools/map_designer/serve.py --emit-c

    # 预览布局数据（不写文件）
    uv run tools/map_designer/serve.py --emit-layouts

    # 真正写布局数据（默认仅 project 工程树）
    uv run tools/map_designer/serve.py --emit-layouts --apply

    # 检查布局生成物是否过期
    uv run tools/map_designer/serve.py --emit-layouts --check

--emit-layouts --apply 会写：

    project/GamingPlayUnit/.../game/game_layout_data.c/.h

只有显式指定 --target archive/all 才会写 code/。

## 10. C / 布局导出与固件现状

工具能做：

- emit_c()：单张地图 C 数据（room_map_shared / room_rot_shared / room_objects）；
- emit_layout_data()：output/*.json 生成 game_layout_data.c/.h；
- L 形 / 石头地板 S 会作为异形地形表导出（T_<名字>），不走 g_maps 参数；
- 纯 B 标准房仍可用 --emit-frame 生成 g_maps 一行。

固件还没接 S：

- code/logic/game/game_world.c 的 game_tile_asset() 只有 B/K/W/D/H/C...；
- code/logic/game/game_room.c 地板判断只有 B/K；
- assets_desc.h 里还没有 ASSET_minecraft_stone（新图 res/pic/background/minecraft_stone.png 已存在，但素材表还没重新生成）。

所以：

- 编辑器 / JSON / C 数据里 S 是对的；
- 游戏里如果仍显示木头或黑块，不是编辑器的问题，需要补固件：
  1. 跑素材管线生成 ASSET_minecraft_stone；
  2. game_tile_asset() 加 case 'S'；
  3. game_room.c 地板判断加 S；
  4. 重新 --emit-layouts --apply。

## 11. 素材

res/pic 扫描，文件名匹配角色：

    木地板       minecraft_wood.png
    基岩         minecraft_bedrock.png
    石头地板     minecraft_stone.png
    角块         background_corner_wood.png
    门           background_door_normal.png
    隐藏门       background_interact_hideEnterance.png
    交互石头     background_interact_stone.png

换图后点左侧「重载素材」；URL 已带 ?v=修改时间，res/ 也设置了 no-cache。

## 12. 已知问题 / TODO

1. 固件 S 未接（见第 10 节）。
2. template_1x1 / 2x2 / L 都是石头 S，不再是标准矩形框架，--emit-frame 不适用。
3. 自定义模板在 templates/custom/，不在 output/ 里；它不会自动进 --emit-layouts。要进游戏布局，先把图另存成 output/<房间>.json。
4. 旧 .txt 仍是字母格式；主存档是 JSON v4。如果要求 .txt 也数字，需要另定文本格式。
5. output/*.json 已迁移成 v4 数字；output/_archive/ 是旧归档，没动。
6. 端口 8766 默认；8765 是 item_designer。
7. check_game_layout.py 对标准框架的检查仍以 B/K/S 为地板；异形布局主要看 validate() / emit_layout_data()。

## 13. 最近改动（2026-10-24）

- 地形笔刷新增 S 石头地板，和 K 基岩、对象 stone 分开；
- 三套模板统一石头 S；
- 笔刷角标改成 0 到 7，对象 0 到 6，数字键按顺序选；
- JSON v4：tiles 用整数二维数组，tile_encoding 为 index；
- JSON 每行一格，不再每个数字一行；
- 页面 JSON 文本框 / 复制 / 下载 JSON 都用数字；
- L 形房校验放宽，避免逐格误报；
- 自由缩放 10% 到 800%，滚轮以光标为中心；
- 素材 URL 加 mtime 防缓存；
- 模板下拉列表：内置 + 自定义；
- 当前图可存成 templates/custom/custom_*.json，可载入 / 覆盖 / 删除；
- 自定义模板 JSON 增加可选 label 字段（只用于显示）；
- 页面版本 2026-10-24j。

## 14. 接手检查清单

1. uv run tools/map_designer/serve.py --check 是否通过；
2. 跑 66 个单测；
3. 页面打开 8766，状态栏版本 2026-10-24j；
4. 新建 1x1 模板，确认地板是 S（数字 7）；
5. 保存一个 JSON，确认 tiles 是数字、每行一格；
6. 把当前图存成自定义模板，确认 templates/custom/ 出现 custom_*.json，重新载入 / 删除各一次；
7. 如果要做固件，先补 ASSET_minecraft_stone 和 S 的 tile/floor 分支；
8. 不要随手跑 --emit-layouts --apply，它会写 code/ 和工程树。
