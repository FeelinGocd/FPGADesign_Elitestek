# DEBUG 房间编辑器（tools/sandbox）

给 FPGA 游戏 demo 用的房间布局工具：**40 × 23 格 / 32 px / 1280 × 720**，
保存成文本，再生成固件用的 C 数据文件。

~~~text
 浏览器编辑  ──保存──▶  output/room_map.txt
      │                        │
      │                   ──生成 C──▶  output/game_world_map.c
      │                        │
      └──── 导入 ◀─────────────┘        拷进 code/ → Efinity 工程
~~~

服务端负责**格式解析、校验、C 生成**（`mapfmt.py`），前端只负责画和发请求，
所以命令行、CI、浏览器三处用的是同一套规则。

## 快速开始

~~~powershell
uv run tools/sandbox/serve.py --open
~~~

不自动开浏览器就去掉 `--open`，然后打开：

~~~text
http://127.0.0.1:8765/tools/sandbox/index.html
~~~

需要保存 / 生成 C / 自动校验时**必须通过 serve.py 打开**；直接双击
`index.html`（file://）只能编辑和导入导出文本。

## 两个产物

| 文件 | 是什么 | 谁写 |
| --- | --- | --- |
| `output/room_map.txt` | 地图源文件，v2 文本（23 行地图 + 23 行旋转） | 页面保存 / 手动编辑 |
| `output/game_world_map.c` | 固件用的 C 数据（两张 static 表） | 页面「生成 C」/ `--emit-c` |

两个文件都留在 `output/`；**导入导出**都在页面右侧：

* **导入**：选 `.txt` 或 `.c` 文件（也可以直接贴到文本框）。C 文件既可以是
  生成的 `game_world_map.c`，也可以直接选工程里的 `game_world.c`。
* **导出**：`下载 .txt` 和 `下载 .c`；PNG 按钮导出当前画布，方便写汇报。
* 保存时勾着「同时生成 C」，一次点击同时更新两个文件。

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
* 空白格上的旋转会提示（C 侧不读它）。

四个合法门槽位（画布上有一圈蓝色虚线框）：

~~~text
N (20, 2, rot 0)    E (32, 10, rot 1)
S (20, 17, rot 2)   W (7, 10, rot 3)
~~~

> 门的位置不是由 D 的坐标随便决定的：固件的 `door_xy()` 按墙带和贴图尺寸算，
> 所以 D 放错格子会出现「门洞在别处」的黑洞。编辑器用校验把这件事挡住。

## 命令行

~~~powershell
# 只检查：地图合法 + 生成的 C 是否最新（CI / pre-commit 用，失败退出码 1）
uv run tools/sandbox/serve.py --check

# 从 room_map.txt 重新生成 C
uv run tools/sandbox/serve.py --emit-c

# 检查某个已经拷进 code/ 的副本是否和地图一致
uv run tools/sandbox/serve.py --check --c code/logic/game/game_world_map.c

# 换端口 / 只监听本机
uv run tools/sandbox/serve.py --port 8766
~~~

单元测试：

~~~powershell
uv run python -m unittest discover -s tools/sandbox -p "test_*.py" -v
~~~

## 地图文本格式

v2（现有文件）：

~~~text
# DEBUG_ROOM_MAP v2
........................................   ← 23 行，每行 40 个字符
...
# ROTATION
0000000000000000000000000000000000000000   ← 23 行，每行 40 个 0..3
...
~~~

也兼容：

* **v1**：只有 23 行地图，没有 `# ROTATION`（旋转全 0）；
* **稀疏旋转**：`# ROTATION sparse` 之后每行 `x,y,r`，只写非 0 的格子：

~~~text
# ROTATION sparse
20,3,2
7,10,3
~~~

字符含义：

| 字符 | 含义 |
| --- | --- |
| `.` | 空白（黑） |
| `B` | 基岩地板 |
| `W` | 木墙 |
| `D` | 门，占 1×2（横向门是 2×1，方向由 rot 决定） |
| `C` / `X` / `Y` / `Z` | 角块：原样 / 水平翻 / 垂直翻 / 双向翻 |

## 生成的 C 怎么用

`output/game_world_map.c` 是一份**独立的数据文件**（`static` 数组 + 头文件保护 +
`_Static_assert`），两种用法：

1. **替换数组**：把 `room_map_shared` / `room_rot_shared` 两张表拷进
   `code/logic/game/game_world.c`，替换原来的两段。
2. **include**（推荐，`game_world.c` 更干净）：把这个文件放到
   `code/logic/game/game_world_map.c`，删掉 `game_world.c` 里的两段数组，改成

   ~~~c
   #include "game_world_map.c"
   ~~~

   数组是 `static`，不会和其他编译单元抢符号。

> 按 `code/README_如何导入C文件到工程.md` 的约定：`code/` 是唯一真源，
> 改完再**手工拷贝**进 Efinity 工程（工程不引用仓库外部路径）。
> 生成文件放在 `output/` 是工作区，什么时候拷进 `code/` 由你决定；
> `--check --c` 可以核对拷过去的那一份是不是最新的。

## 编辑器

* 左键绘制，右键擦除；拖动连续画，一次拖动算一步撤销。
* `B` / `W` / `D` / `E` 选地板 / 墙 / 门 / 空白。
* `R` 循环旋转（选角块时循环 C/X/Y/Z）；`1`–`4` 直接选。
* `Ctrl+Z` 撤销，`Ctrl+Y` 重做，`Ctrl+S` 保存。
* `0` 适应窗口，滚轮缩放，右上角滑杆 35%–250%。
* 「固件视角」勾上后按 `tile_rot()` 的 **1↔3 互换**显示（木纹/角块方向），
  默认关闭，保持和旧版编辑器一致的观察方式。

## 文件说明

| 文件 | 作用 |
| --- | --- |
| `index.html` | 页面骨架 |
| `style.css` | 样式（深色三栏） |
| `app.js` | 画布、编辑、撤销、导入导出、校验展示 |
| `serve.py` | 本地服务 + `--check` / `--emit-c` |
| `mapfmt.py` | 格式解析、校验、C 生成 / 回读（唯一实现） |
| `test_mapfmt.py` | 单元测试 |
| `output/` | 地图 txt 和生成的 C |

## 已知边界

* 素材路径写死 `res/pic/base/background/`；换素材名要改 `app.js` 里的
  `TILES` 表。素材加载失败会在状态栏提示。
* 门类型（normal / accessory / boss）是固件按房间邻居算的，地图里只有 `D`，
  编辑器不区分。
* 最后一格只有 16 px（23 × 32 = 736 > 720），画布按固件一样裁掉，网格线也只画到 720。
