# MAP DESIGNER · 房间设计器（tools/map_designer）

给 FPGA 游戏 demo 用的固定房间设计器：**40 × 23 格 / 32 px / 1280 × 720**。
除了地形，还能放**怪物出生点**和**交互方块**，保存成文本，再生成固件用的 C 数据文件。

~~~text
 浏览器编辑  ──保存──▶  output/room_map.txt
      │                 output/room_map.json   ← 约定文件（存档 / 加载）
      │                        │
      │                   ──可选生成 C──▶  output/game_world_map.c
      │                        │
      └──── 导入 ◀─────────────┘        需要时再拷进 code/ → Efinity 工程
~~~

服务端负责**格式解析、校验、C 生成**（`mapfmt.py`），前端只负责画和发请求，
所以命令行、CI、浏览器三处用的是同一套规则。

> 本工具只改 `tools/map_designer/`。生成的 C 什么时候拷进 `code/` 由你决定，
> 工具本身不会动固件源码。

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

## 三个文件

| 文件 | 是什么 | 谁写 |
| --- | --- | --- |
| `output/room_map.txt` | 可读源文件，v3 文本（模板 + 23 行地形 + 对象层 + 旋转） | 页面保存 / 手动编辑 |
| `output/room_map.json` | **约定文件**，JSON（tiles / rotation / objects / room_type），用于存档和加载 | 页面保存 / 手动编辑 |
| `output/game_world_map.c` | **可选导出**，给固件的 C 数据（地图 / 旋转 / 对象数组） | 页面「生成 C」/ `--emit-c` |

三个文件都留在 `output/`；**保存默认只写 .txt + .json，不会改 `code/`**。
C 文件只有勾选「保存时同时生成 C（可选导出）」或点「生成 C」时才会写。

**导入导出**都在页面右侧：

* **导入**：选 `.txt` / `.json` / `.c` 文件（也可以直接贴到文本框）。C 文件既可以是
  生成的 `game_world_map.c`，也可以直接选工程里的 `game_world.c`。
* **导出**：`下载 .txt`、`下载 .json`、`下载 .c`；PNG 按钮导出当前画布，方便写汇报。
* 日常存档推荐 `.json`：字段固定、好解析，也方便以后换固件 / 换工具。

## 图层

页面左侧有两个图层：

1. **地形层**：`.` 空 / `B` 基岩地板 / `W` 木墙 / `D` 门 / `C X Y Z` 角块。
   操作和旧版编辑器一样。
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

## 房间模板

文本第二行是模板类型：

~~~text
# TEMPLATE empty
~~~

可选 `start` / `empty` / `item` / `shop`。生成 C 时会写成：

~~~c
#define MAP_TEMPLATE_ROOM 1   /* empty */
#define MAP_TEMPLATE_NAME "empty"
~~~

固件可以用 `MAP_TEMPLATE_ROOM` 和当前 `ROOM_EMPTY` 比较，决定这一间房是否
使用固定布局；没有对象时退回原来的随机刷怪。把页面右侧「房间模板」切到
别的类型，就能给起点房 / 道具房做另一套固定设计。

## 校验

保存 / 生成 C 之前服务端会先校验，**有 error 就拒绝写入**（warning 只提示）。
面板里点一条问题会闪一下对应格子。

会检查：

* 23 行 × 40 字符，字符只能是 `. B W D C X Y Z`；
* 门必须在 **2 格厚的墙带**里，不能在竞技场内部、不能在墙角；
* 门的旋转值就是方向：**0=N 1=E 2=S 3=W**，和所在墙对不上会报错；
* 门的另一格必须是空白：纵向门是下一行，横向门是右一格；
* 墙带里的空白如果不是门洞，会提示「漏出一个洞」；
* 房间外（黑边）画了东西会提示，上板看不到；
* 空白格上的旋转会提示（C 侧不读它）；
* 对象必须在竞技场内的 `B` 地板上，不能在墙 / 门 / 空白上；
* 同一格重复放同种对象、同格叠两个阻挡物、小怪 / 网 / 阻挡物超过
  `N_MOBS` / `N_WEBS` / `N_STONES` 会提示。

四个合法门槽位（画布上有一圈蓝色虚线框）：

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
........................................   ← 23 行，每行 40 个字符
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
0000000000000000000000000000000000000000   ← 23 行，每行 40 个 0..3
...
~~~

也兼容：

* **v2**：`# DEBUG_ROOM_MAP v2`，没有 `# OBJECTS`（对象为空）；
* **v1**：只有 23 行地图，没有 `# ROTATION`（旋转全 0）；
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
  "version": 3,
  "room_type": "empty",
  "size": { "cols": 40, "rows": 23, "tile": 32 },
  "tiles": ["...", "..."],
  "rotation": [[0, 0, 0]],
  "objects": [{ "kind": "slime", "x": 12, "y": 8, "arg": 0 }],
  "sha256": "..."
}
~~~

这份 JSON 就是工具和固件之间的约定文件：存档、回读、以后换编辑器都靠它。
**工具不会写 `code/`，也不会改固件源码。**

`output/game_world_map.c` 只是**可选导出**，本身也是一份独立数据文件
（`static` 数组 + 头文件保护 + `_Static_assert`），包含：

* `room_map_shared` / `room_rot_shared`：地形和旋转；
* `room_objects`：对象层，每行 `{ kind, x, y, arg }`；
* `MAP_TEMPLATE_ROOM` / `MAP_TEMPLATE_NAME`：模板类型；
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

> 按 `code/README_如何导入C文件到工程.md` 的约定：`code/` 是唯一真源，
> 改完再**手工拷贝**进 Efinity 工程（工程不引用仓库外部路径）。
> 生成文件放在 `output/` 是工作区，什么时候拷进 `code/` 由你决定；
> `--check --c` 可以核对拷过去的那一份是不是最新的。

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
# 只检查：地图合法 + 约定 JSON / C 是否最新（CI / pre-commit 用，失败退出码 1）
uv run tools/map_designer/serve.py --check

# 从 room_map.txt 重新生成约定 JSON（存档 / 加载用）
uv run tools/map_designer/serve.py --emit-json

# 从 room_map.txt 重新生成 C（可选导出）
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

* 左键绘制 / 放对象，右键擦除 / 删对象；拖动连续画，一次拖动算一步撤销。
* `O` 切换地形 / 对象层。
* `B` / `W` / `D` / `E` 选地板 / 墙 / 门 / 空白（自动切回地形层）。
* 对象层 `1`–`7` 直接选 slime / bug / web / stone / box / crate / exit。
* 地形层 `1`–`4` 选角度；选角块时 `R` 循环 C/X/Y/Z。
* `R` 对象层循环对象，地形层循环旋转。
* `Ctrl+Z` 撤销，`Ctrl+Y` 重做，`Ctrl+S` 保存。
* `0` 适应窗口，滚轮缩放，右上角滑杆 35%–250%。
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
| `output/` | 地图 txt、约定 JSON、可选的 C 导出 |

## 已知边界

* 素材路径写死 `res/pic/base/background/`、`res/pic/base/background_interact/`、
  `res/pic/character/mobs/`；换素材名要改 `app.js` 里的 `TILES` / `OBJECT_DEFS`。
  素材加载失败会在状态栏提示。
* 门类型（normal / accessory / boss / shop）是固件按房间邻居算的，地图里只有 `D`，
  编辑器不区分。
* `boss` 大房间是 45×35，当前页面按普通房 40×23 画；选 `boss` 会提示，
  生成 C 前请确认尺寸。
* 对象坐标只有整格，不能做半格 / 像素级微调；需要更细的摆位时再给格式加
  像素偏移字段。
* 最后一格只有 16 px（23 × 32 = 736 > 720），画布按固件一样裁掉，网格线也只画到 720。
