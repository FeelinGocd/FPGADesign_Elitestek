# MAP DESIGNER · 房间设计器（tools/map_designer）

## 普通房布局池工作流（页面 2026-10-25f）

房间空间固定，由 `maps/1x1.json`、`maps/L.json`、`maps/2x2.json` 和
`maps/hidden.json` 持有。它们从本次适配前的正式地图提取，保留原来的地形、门与角块。
**L 型设计只用左下空缺，不另画其它朝向。** 内置模板直接读取这些固定空间。

本轮只设计普通房型 **1×1、L、2×2**。隐藏房、商店、道具房与 Boss 房暂不编辑，
也不从普通布局池选取对象方案。`maps/hidden.json` 和旧特殊房数据仅为现有游戏兼容保留。

1. 默认进入 **布局模式**，选择房型，点 **新建布局**。
2. 地形、门和画布尺寸已锁定，只放普通怪物、网、石头、木箱、板条箱；对象只能放在地板上。
3. 点 **保存布局**，按房型自动编号，保存为 `output/layouts/1x1_001.json`、`L_001.json`、`2x2_001.json`。
   每种房型独立从 001 递增。读取已有方案后保存为更新；新建不会覆盖已有编号。
   `1x1_000.json` 是保留的起点空布局，不能放对象，也不会被普通房间抽选。
4. 在仓库根目录执行 `python tools/map_designer/serve.py --emit-layouts --apply`。
   默认只更新 `project` 的生成数据；`--emit-layouts --check` 可检查是否同步。
5. 用户重新编译、下载固件。初始房固定使用空的 `1x1_000`；普通 1×1 和 L 房按空间选择正编号方案；同一间房在本层保持选择。
   有编号方案时不再抽中旧参考空布局；没有编号方案时回退到原有布局。
   起始房、隐藏房和 Boss 房保留原指定方案，道具房/商店仍走原专用逻辑。

**2×2 本轮支持设计和导出。当前关卡的 2×2 仍是 Boss 专用，本次没有增加普通 2×2 的拓扑生成。**
普通 2×2 方案不会被 Boss 房误选，之后接入普通大房生成时再使用。

文件按职责分开：

| 位置 | 用途 |
|---|---|
| `maps/1x1.json`、`maps/L.json`、`maps/2x2.json` | 正式固定空间，布局引用的依据 |
| `output/maps/<房型>.json` | **地图模式**导出的纯空间文件；不含对象，不自动替换正式空间 |
| `output/layouts/<房型>_<编号>.json` | **布局模式**导出的正式对象方案，批量生成器自动读取 |
| `output/` 根目录旧 JSON | 兼容原有参考布局，保留已有内容 |

地图模式用于独立编辑/导出空间，布局模式重新加载正式固定空间。改地图草稿不会让已有布局的墙、门
或坐标悄悄变化；正式房型变更需作为地图池变更单独处理。

方案示例（格子坐标以 0 开始，使用模板原坐标，不裁剪、不居中、不翻转）：

```json
{
  "format": "map_designer_layout",
  "version": 1,
  "name": "L_001",
  "map": "L",
  "layout_id": 1,
  "objects": [{"kind": "slime", "x": 25, "y": 9, "arg": 0}]
}
```

读取方案时由服务端补回固定空间，所以请通过 `serve.py` 打开设计器。
运行时如果地图拓扑采用 L 型其它朝向，地形、对象生成、绘制、碰撞与擦除统一镜像；
破坏状态仍用设计原坐标记录。多套 L 对象方案共享同一份 C 地形表与地形缓存身份。
每方案最多 200 个对象，总布局最多 127 张；实际怪物数量仍受游戏对象容量约束。

`layout_id` 是稳定设计编号，不是生成 C 的数组下标。服务端检查文件名、房型引用与编号一致，
编号限 001..999，总运行布局仍最多 127 张（含旧参考布局）。
**下载 JSON / 复制 JSON / Ctrl+S** 都遵循当前模式：布局输出对象方案，地图输出空间。
旧完整 JSON 可导入，但参与布局生成时地形和旋转必须匹配固定地图池。
不要为替换房内对象修改 `maps/`。以下自由画布及旧完整地图内容属于兼容参考，以本节工作流为准。

更新后需要重启 `serve.py` 并刷新页面，以加载新的编号接口。验证命令：

```powershell
python -m unittest discover -s tools/map_designer -p 'test_*.py'
node tools/map_designer/test_editor.js
# 可选：本机已安装 Chrome/Edge 和 websocket-client 时，运行隔离目录的真实浏览器检查
python tools/map_designer/check_workflow_browser.py
```

给 FPGA 游戏 demo 用的固定房间设计器：**画布尺寸自由**，32 px/格，默认
**40 × 23（1280 × 736）**，上限 **45 × 35（1440 × 1120，Boss 房）**，下限 8 × 8。
除了地形，还能放**怪物出生点**和**交互方块**，保存成 JSON，再按需生成固件用的 C 数据文件。

~~~text
 浏览器编辑  ──保存──▶  output/room_map.json   ← 约定文件（存档 / 加载）
      │                        │
      │                   ──可选生成 C──▶  output/game_world_map.c
      │                        │
      └──── 导入 ◀─────────────┘        --emit-layouts --apply → project 工程
~~~

服务端负责**格式解析、校验、C 生成**（`mapfmt.py`），前端只负责画和发请求，
所以命令行、CI、浏览器三处用的是同一套规则。

> 页面保存和单图导出只写 `tools/map_designer/output/`。
> `--emit-layouts --apply` 默认更新 `project/` 的布局数据；`code/` 是归档，
> 只有显式指定 `--target archive` 或 `--target all` 才会写入。

## 快速开始

~~~powershell
uv run tools/map_designer/serve.py --open
~~~

不自动开浏览器就去掉 `--open`，然后打开：

~~~text
http://127.0.0.1:8766/tools/map_designer/index.html
~~~

> 默认端口是 **8766**。8765 通常被 `tools/item_designer` 占用；
> 8766 被占用时工具会自动尝试 8767–8775，并打印实际地址。
> 也可以手动指定：`--port 8767`。

需要保存 / 生成 C / 自动校验时**必须通过 serve.py 打开**；直接双击
`index.html`（file://）只能编辑和导入导出文本。

## 两个文件

| 文件 | 是什么 | 谁写 |
| --- | --- | --- |
| `output/<name>.json` | **唯一存档**（name / size / tiles / rotation / objects） | 页面保存 / 手动编辑 |
| `output/game_world_map.c` | **可选导出**（手动），给固件的 C 数据（地形表 / 旋转 / 对象数组） | 页面「生成 C」/ `--emit-c` |

**保存只写一份 .json**，不会再顺手生成 .txt 或 C —— 写盘的两个动作都必须是显式的。
老的 `.txt` 仍然能导入（格式没删），只是不再生成新的。文件都留在 `output/`，不会改 `code/`。

**导入导出**都在页面右侧：

* **导入**：选 `.txt` / `.json` / `.c` 文件（也可以直接贴到文本框）。C 文件只认
  导出的 `game_world_map.c`（工程里的地形现在是生成的，没有表可导）。
* **导出**：下载 JSON、下载 PNG；展开 C 数据导出可生成或下载 C。旧 TXT 仍支持导入。
* 日常存档推荐 `.json`：字段固定、好解析，也方便以后换固件 / 换工具。

## 图层

页面左侧有两个图层：

1. **地形层**：`.` 空 / `B` 木地板 / `K` 隐藏房地板 / `W` 木墙 / `w` 隐藏房墙 /
   `D` 门 / `H` 隐藏门（1 格）/ `C X Y Z` 角块 / `c x y z` 隐藏房角块。操作和旧版编辑器一样。
   旧的 `S` 石头地板已废弃（素材删了）：老存档仍能解析，编辑器里按木地板显示。
2. **对象层**：左键放对象，右键擦掉这一格的所有对象。对象按整格保存，
   坐标为 `x,y`（32 px 格），不是像素。

对象类型：

| kind | 含义 | C 侧目前的行为 |
| --- | --- | --- |
| `slime` | 史莱姆出生点 | 固定刷一只 slime |
| `bug` | 甲虫出生点 | 固定刷一只 bug |
| `web` | 蜘蛛网 | 踩上去减速 |
| `stone` | 石头 | 阻挡玩家 / 弹幕 |
| `box` | 木箱 | 和石头共用阻挡逻辑，贴图不同 |
| `crate` | 板条箱 | 和石头共用阻挡逻辑，贴图不同 |
| `exit` | 出口标记 | 只导出数据，固件暂未消费 |

> `box` / `crate` 目前没有独立的“打碎”逻辑，先用石头的碰撞。以后要加
> 可破坏物，只需要在固件里按 `kind` 分流，地图格式不用再改。

## 画布尺寸

右栏「画布尺寸」填宽 × 高（格），点「应用」：

* 范围 **8×8 ~ 45×35**；上限就是固件的 `MAP_MAX_COLS` / `MAP_MAX_ROWS`；
* **换尺寸保留左上角重叠部分**（变小时裁掉越界内容，变大时补空白；越界对象会被丢掉），
  整个操作算一步，`Ctrl+Z` 可撤销；
* 想要干净的起手骨架就在右栏「模板」下拉框里选 **1×1 / L / 2×2**：直接画好地板、
  2 格墙带、四扇门（方向、门洞都正确），在上面接着改就行；
  **三套内置模板使用正式木地板 `B`**；
* 也可以把当前图存成**自定义模板**：填文件名和显示名，点「当前图存为模板」；
  地形、旋转、对象会一起存到 `templates/custom/custom_<文件名>.json`，之后从下拉框直接载入，
  同名可覆盖，内置模板不可删除；自定义模板只是起手骨架，不会自动进 `output/`，
  要生成布局就把图另存成 `output/<房间>.json`。
* 导入 txt / JSON / C 时尺寸从文件内容读，页面自动跟着变；
* 竞技场由**地板 `B` / `K` / `S` 的包围盒**推出来，墙带是包围盒外扩 2 格 —— 你画在哪，
  游戏里的可走区域就在哪，不再有写死的 288,128 → 1024,544；
* 1280×720 参考框只用于对照 720p 可见窗口，不落盘、不参与校验。

## 房间框架（固件侧是"参数"，不是表）

固件的矩形框架包括**普通 1×1（40×23）**、**隐藏房 1×1（40×23）**和 **2×2（45×35）**。设计器的模板按**形状**分三档：`template_1x1` / `template_L` / `template_2x2`（数据在 `templates/*.json`）。
墙 / 地板 / 角块 / 门都由 `game/game_frame.c` 按参数生成，不再整张表存：

~~~c
/* g_maps[] 里的一行 ⤵ 尺寸 / 竞技场 / 四扇门 / 各边木纹角度 / 角块开关 */
{ 0, 0,
  { 40, 23, 9, 4, 31, 16, { 20, 10, 20, 10 }, { 0, 1, 2, 3 }, 1 },
  1280, 720 },
~~~

所以：

* 三套模板的地板统一用 `B` 木地板，和固件 1×1 框架一致。
* `--emit-frame` 只对标准矩形房（40×23）有效；L / 2×2 仍按异形地形表导出。
* 纯 `B` 的标准房画完点 **`--emit-frame`** 可以拿到要贴进 `g_maps[]` 的那一行：

~~~powershell
uv run tools/map_designer/serve.py --emit-frame --map tools/map_designer/output/room2x2_map1.txt
~~~

* 加了墙 / 挖了角、改得不是标准矩形 → 会判为"异形房"（L 形也支持）：
  `--emit-frame` 不适用，但 `--emit-layouts` 会自动多带一张逐格地形表；
* 标准矩形框架只存参数；隐藏房保留独立的地板、墙、角块皮肤及单格入口，不额外生成逐格地形表。

## 布局数据：零配置生成

规矩一句话：**一份房间 = 一份文件，`output/<房间名>.json` 就是那间房**（地形 + 对象都在里面，文件名 = 房间名，也是代码里的 `LAY_<房间名>`）。想留草稿就存到 `output/_archive/` 或别处，`output/` 根下只放要进代码的房间。

~~~powershell
uv run tools/map_designer/serve.py --emit-layouts              # 预览，不写文件
uv run tools/map_designer/serve.py --emit-layouts --apply      # 生成到 project 工程树
uv run tools/map_designer/serve.py --emit-layouts --check      # 只查生成物是否过期
~~~

生成两个文件，**都是机器写的，别手改**（头部有源文件哈希）：

| 文件 | 内容 |
| --- | --- |
| `game/game_layout_data.c` | `L_<名字>[]` 对象数组 + `g_layouts[]`（名字 / 个数 / 地图下标） |
| `game/game_layout_data.h` | `LAY_<名字>` 下标宏 + `LAYOUT_COUNT` |

异形地形的 C 表仅分配实际行数，旋转值四格打包为一字节；
JSON 存档仍使用原来的逐格 0..3。固件统一从 game_world_rot_at 解码，
隐藏入口位置、角块和对象层不受存储格式影响。

手写的只剩 `game_layout.c` 里的 `g_kinds[]`：**哪种房间用哪些布局**、门贴图、小地图颜色。

加一张图：设计器存成 `layout_xxx.json` → `--emit-layouts --apply` → 想让普通房抽到它，
就在 `g_kinds[ROOM_EMPTY]` 的布局池里加一个 `LAY_xxx`。

生成前会整体校验：有坐标越界 / 不在地板 `B/K/S` 上 / 重名 / 对象超过 200 个 → **整批拒绝生成**；
`exit` 这种固件没有的对象会跳过并提示。

**地图下标不是写死的**：工具读 `game_world.c` 的 `g_maps[]`，按尺寸和皮肤匹配；
因此普通房和隐藏房尺寸相同也能分别映射。以后加 1×2：

1. `g_maps[]` 里加一行（尺寸 / 竞技场 / 四门 / 木纹 / 世界像素尺寸）；
2. 设计器用同样尺寸画，存成 `layout_map1x2.json`；
3. `--emit-layouts --apply` 就自动映射到新下标，工具本身不用改。

布局文件建议按"占格"命名：`map1x1` / `map1x2` / `map2x2`。

## 地图就是地图（没有房间类型）

**所有房间共用同一个 map 池**，所以格式里没有"房间类型"这回事：

* 不再写 `# TEMPLATE` / `MAP_TEMPLATE_*`；旧文件里的 `# TEMPLATE empty` 会被
  忽略（兼容读取，不报错、不影响图）；
* 生成 C 只写 `MAP_NAME`、尺寸、竞技场、地形、旋转、对象；
* 哪张图给哪种房间用，是**固件侧**布局池的事；设计器只管把图做好。

~~~c
#define MAP_NAME "room_map"
#define MAP_COLS 40
#define MAP_ROWS 23
#define MAP_ARENA_X0 9
~~~

## 素材（自动从 res/pic 读）

页面启动时拉 `/api/assets`：服务端扫一遍 `res/pic/`（跳过 `_` 和 `.` 开头的目录），
按文件名匹配到各笔刷 —— 木地板 `background_floor_wood`、隐藏房地板 `background_floor_hidden`、
墙 `background_wall_wood`、隐藏房墙 `background_wall_hidden`、角块 `background_corner_wood`、
隐藏房角块 `background_corner_hidden`、门 `background_door_normal`、隐藏门 `background_interact_hidden`、
怪 `mobs_slime` / `mobs_bug`、
交互物 `background_interact_web|stone|crate|box_close|eixt`。

所以**换图 / 挪目录都不用改代码**：把文件放回 `res/pic` 下、名字里带上面那些片段即可；
某个笔刷找不到图时，页面上是色块 + 名字，状态栏会列出缺的角色。
角色表在 `mapfmt.py` 的 `ASSET_ROLES`，加新笔刷时在那里加一行。

## 校验

保存时会先校验，但允许把半成品存下来（error 只提示）；**生成 C / 布局时才拦 error**。
面板里点一条问题会闪一下对应格子。

会检查：

* 尺寸在 8×8 ~ 45×35 之间；各行字符数必须一致，字符只能是 `. B W D H K S C X Y Z w c x y z`；
* **矩形房**：门必须在 **2 格厚的墙带**里，不能在竞技场内部、不能在墙角；
* **L 形 / 凹形房**：竞技场包围盒里出现非 B 就判为异形，只给一条提示，
  不再逐格报「会挡住玩家」；门按实际几何检查，不会因为形状被误判；
* 门的旋转值就是方向：**0=N 1=E 2=S 3=W**，和所在墙对不上会报错；
* 普通门 `D` 的另一格必须是空白：纵向门是下一行，横向门是右一格；
* **隐藏门 `H` 只占 1 格**（放在靠竞技场那侧的墙带格），没有"另一格"，背后是隐藏房墙；
* 墙带里的空白如果不是门洞，会提示「漏出一个洞」；
* 房间外（黑边）画了东西会提示，上板看不到；
* 空白格上的旋转会提示（C 侧不读它）；
* 对象必须在竞技场内的 `B` 地板上，不能在墙 / 门 / 空白上；
* 同一格重复放同种对象、同格叠两个阻挡物、小怪 / 网 / 阻挡物超过
  `N_MOBS` / `N_WEBS` / `N_STONES` 会提示。

四个合法门槽位 = 各边墙带正中（画布上有一圈蓝色虚线框）；下面是 40×23 默认画布的
值，换尺寸后按同样规则重算，右栏「房间约束」里实时显示：

~~~text
N (20, 2, rot 0)    E (32, 10, rot 1)
S (20, 17, rot 2)   W (7, 10, rot 3)
~~~

> 门的位置不是由 D 的坐标随便决定的：固件的 `door_xy()` 按墙带和贴图尺寸算，
> 所以 D 放错格子会出现「门洞在别处」的黑洞。编辑器用校验把这件事挡住。

## 地图文本格式（v3）

~~~text
# MAP_DESIGNER v3
# TEMPLATE empty
........................................   ← 有 N 行，每行 C 个字符（默认 23 × 40）
...
# OBJECTS
slime 12,8
bug 20,10
web 15,12
stone 10,6
box 25,15
crate 27,15
exit 20,6
# ROTATION
0000000000000000000000000000000000000000   ← N 行，每行 C 个 0..3
...
~~~

也兼容：

* **v2**：`# DEBUG_ROOM_MAP v2`，没有 `# OBJECTS`（对象为空）；
* **v1**：只有地形行，没有 `# ROTATION`（旋转全 0）；
* **稀疏旋转**：`# ROTATION sparse` 之后每行 `x,y,r`，只写非 0 的格子；
* **旧 C**：只有 `room_map_shared` / `room_rot_shared` 的 `game_world.c`。

对象行格式：

~~~text
kind x,y          # arg 默认 0
kind x,y,arg      # arg 0..255，当前保留给以后用
~~~

## 约定 JSON 与可选 C

**默认存档/加载用 `output/room_map.json`**。它是纯数据，不包含 C 代码：

~~~json
{
  "format": "map_designer",
  "version": 4,
  "name": "room_map",
  "size": { "cols": 40, "rows": 23, "tile": 32 },
  "tile_encoding": "index",
  "tiles": [[0, 1, 7, 7], [0, 1, 7, 7]],
  "rotation": [[0, 0, 0]],
  "objects": [{ "kind": "slime", "x": 12, "y": 8, "arg": 0 }],
  "sha256": "..."
}
~~~

这份 JSON 就是工具和固件之间的约定文件：存档、回读、以后换编辑器都靠它。
**工具不会写 `code/`，也不会改固件源码。**

`tiles` 里的数字就是面板上的笔刷编号：

~~~text
0 空       1 木地板 B   2 木墙 W    3 门 D
4 角块 C   5 隐藏门 H   6 隐藏房地板 K
8 角块 X   9 角块 Y    10 角块 Z
11 隐藏房墙 w   12 隐藏房角块 c   13/14/15 = 隐藏房角块 x/y/z
（7 = 旧石头地板 S，已废弃；编号保留是为了旧存档还能读）
~~~

旧的 `.txt` 文本用 `. B W D H K S C X Y Z` 字母，隐藏房皮肤再加 `w c x y z` 小写；JSON 存档已全面改成数字。

`output/game_world_map.c` 只是**可选导出**，本身也是一份独立数据文件
（`static` 数组 + 头文件保护 + `_Static_assert`），包含：

* `room_map_shared` / `room_rot_shared`：地形和旋转；
* `room_objects`：对象层，每行 `{ kind, x, y, arg }`；
* `MAP_NAME`：这份数据叫什么（用文件名，方便以后进 map 池）；
* `ROOM_OBJECT_COUNT`：对象条数。

如果固件决定用 C 数据文件，推荐把它当成独立文件 include，而不是把数组
直接贴进 `game_world.c`：

~~~c
#define MAP_STORAGE_ROWS MAP_MAX_ROWS
#define MAP_STORAGE_COLS MAP_MAX_COLS
#include "game_world_map.c"
~~~

`MAP_STORAGE_ROWS/COLS` 让普通房的 23×40 地图装进固件按 35×45 建的
静态数组；没写到的行 / 列由 C 自动补零，运行时仍按 `MAP_ROWS/MAP_COLS`
使用，所以不会读到补零区域。数组是 `static`，不会和其他编译单元抢符号。

> 当前开发以 `project/` 下的 C 工程为准，`code/` 保留为提交归档。
> 页面单图导出的 C 留在 `output/`；游戏布局使用
> `--emit-layouts --apply` 更新，`--emit-layouts --check` 检查是否过期。

### 固件侧最小消费方式

生成文件把对象按 `kind` 分开存；固件里可以这样读（只是示例，不在本工具内改）：

~~~c
extern int game_world_object_count(void);
extern int game_world_object_get(int i, int *kind, int *tx, int *ty, int *arg);

for (i = 0; i < game_world_object_count(); ++i) {
    int kind, tx, ty;
    if (!game_world_object_get(i, &kind, &tx, &ty, 0)) continue;
    if (kind == MAP_OBJ_SLIME) game_mobs_spawn_at(kind, tx * TILE, ty * TILE);
    /* ... web / stone / box / crate ... */
}
~~~

这些 `game_world_object_*` / `game_mobs_spawn_at` 是固件侧要加的接口；
本工具只负责生成数据，不修改 `code/`。

## 命令行

~~~powershell
# 只检查：地图合法 + JSON 是否最新（C 是可选导出，没有就跳过；失败退出码 1）
uv run tools/map_designer/serve.py --check

# 老 .txt → .json（迁移老文件用）
uv run tools/map_designer/serve.py --emit-json --map tools/map_designer/output/xxx.txt

# 手动生成 C（可选导出）
uv run tools/map_designer/serve.py --emit-c

# 检查某个已经拷进 code/ 的副本是否和地图一致
uv run tools/map_designer/serve.py --check --c code/logic/game/game_world_map.c

# 换端口 / 只监听本机
uv run tools/map_designer/serve.py --port 8767
~~~

单元测试：

~~~powershell
uv run python -m unittest discover -s tools/map_designer -p "test_*.py" -v
~~~

## 编辑器快捷键

页面版本 2026-10-25d：左侧集中贴图选择、当前预览、状态和校验建议；
右侧集中地图文件、模板和导入导出。选择贴图自动切换地形或对象层。
地图统计、预览设置和高级导出可折叠；校验建议保留在预览下方。

* 左键绘制 / 放对象，右键擦除 / 删对象；拖动连续画，一次拖动算一步撤销。
* `O` 切换地形 / 对象层。
* 地形层数字键 **0–8** 按笔刷顺序选择：
  `0` 空 / `1` 木地板 / `2` 木墙 / `3` 门 / `4` 角块 /
  `5` 隐藏门 / `6` 隐藏房地板 / `7` 隐藏房墙 / `8` 隐藏房角块。
* 对象层数字键 **0–6** 按顺序选 slime / bug / web / stone / box / crate / exit。
* `R` 循环旋转；选角块时循环 C/X/Y/Z（隐藏房角块循环 c/x/y/z）。角度也可以直接点左侧按钮。
* `Ctrl+Z` 撤销，`Ctrl+Y` 重做，`Ctrl+S` 保存。
* `F` 适应窗口；滚轮以光标为中心自由缩放 **10%–800%**；滑块同样是 10%–800%。
* `Ctrl+0` 回到 100%。
* 「固件视角」勾上后按 `tile_rot()` 的 **1↔3 互换**显示（木纹/角块方向），
  默认关闭，保持和旧版编辑器一致的观察方式。

## 文件说明

| 文件 | 作用 |
| --- | --- |
| `index.html` | 页面骨架 |
| `style.css` | 样式（深色三栏） |
| `app.js` | 画布、地形 / 对象编辑、撤销、导入导出、校验展示 |
| `serve.py` | 本地服务 + `--check` / `--emit-c` |
| `mapfmt.py` | 格式解析、校验、C 生成 / 回读（唯一实现） |
| `test_mapfmt.py` | 单元测试 |
| `check_game_layout.py` | 生成布局的对象位置检查 |
| `templates/` | 内置 1×1 / 2×2 / L 模板（JSON v4 数字） |
| `templates/custom/` | 页面保存的自定义模板 `custom_*.json` |
| `HANDOVER.md` | 交接文档（状态、格式、模板、固件待办） |
| `output/` | 房间 JSON v4、可选的 C 导出 |

## 已知边界

* 素材靠文件名片段匹配（见「素材」一节）；一个角色有多张图时按 `ASSET_ROLES`
  的顺序取第一张（例如木箱优先 `box_close`）。要挑别的图，改 `ASSET_ROLES` 的顺序。
  素材加载失败会在状态栏提示。
* 门类型（normal / accessory / boss / shop）是固件按房间邻居算的，地图里只有 `D`，
  编辑器不区分。
* 尺寸只受 `MAP_MAX_COLS/ROWS`（45×35）限制，没有"这张图必须多大"的约束。
* 对象坐标只有整格，不能做半格 / 像素级微调；需要更细的摆位时再给格式加
  像素偏移字段。
* 1280×720 只是**参考框**（右栏可开关），画布按 `cols × rows × 32` 走，
  比 720p 高/矮都不会裁；固件侧相机负责把它映射进 720p 窗口。
