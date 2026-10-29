# img2hex —— 素材 → PRAM / 调色板 / C 素材表

这套脚本是**离线**跑的，不是游戏固件的一部分。依赖声明在仓库根的 `pyproject.toml`
（`pillow`），用 `uv` 管理：

```powershell
uv sync                                   # 第一次 / 换机器
uv run tools/img2hex/pic2pram.py          # 重新打包 res/pic → hex / 调色板 / assets_desc.h
```

## 脚本

| 脚本 | 作用 |
|---|---|
| `pic2pram.py` | `res/pic/*` → `assets.hex` / `palette.hex`（直接写进 Efinity 工程）、`assets_desc.h` / `report.txt`（`tools/img2hex/output/`）；并复制 `assets_desc.h` 到各 C 工程的 `peripheral/APB0/gpu2d/` |

产物落点：

- `project/GamingPlayUnit/hex/`：发布工程的 `assets.hex` / `palette.hex`
- `project/verified/06_SoC_2DGPU/project/`：已验证工程的副本（RTL 的 `INIT_FILE` 写在工程根）
- `tools/img2hex/output/`：`report.txt` 和 `assets_desc.h`
- 各 C 工程 `peripheral/APB0/gpu2d/`：`assets_desc.h`

不再生成 `preview_*.png` / `alpha_demo_*.png`。

## 规矩（改素材前先看）

1. α 只用固定档位（0 / 128 / 255）；连续 α 会把 256 项调色板拆爆，脚本会报错。
2. 尺寸对齐到 4，最大边 256；`--max` 触发缩放也会把 α 变成连续值。
3. 新素材加在目录顺序的最后（`PIC_DIRS_DEFAULT`），已有编号不动。
4. 不透明区不能有纯白（键色 = 白，会被 ck 抠掉）；脚本会自检。
5. **hex / palette 是 BRAM 初值**：改完必须重新综合；`assets_desc.h` 下标会变，必须重编固件，两者一起烧。

当前 C 描述表每项保存四个 `uint16_t`（PRAM 字基址、行跨距、宽、高），共 8 字节。
生成器逐字段检查 0..65535，超范围立即报错，不能通过截断填入描述表；基址不是 DDR 字节地址。
素材名称保留在枚举、注释和 report.txt，不再作为字符串编入游戏。
仅修改描述表存储格式时，hex 与调色板内容不变，无需为此重综合；
固件仍必须重新编译。可用 `python tools/img2hex/check_assets.py --sources-only`
检查源图、hex 与 C 生成表是否一致。

---

## 加一张图 / 换一张图 —— 完整流程

### 0. 先决定图放哪个目录

`PIC_DIRS_DEFAULT` 的**目录顺序 = 素材编号顺序**。新图放进已有目录时，会按文件名**插在该目录内部**，
后面的编号会整体后移 —— 这就是"为什么加一张图要重新综合"的根源。

| 目录 | 大致编号段（以当前表为准；2026-10-25 共 99 张） |
|---|---|
| `character/bigFish` | 0..8 |
| `character/TheBoss` | 9..12 |
| `UI/num` | 13..34 |
| `UI/Eng` | 35..60 |
| `background` | 61..67 |
| `specialEffect` | 68..72 |
| `bullet` | 73..75 |
| `accessory` | 76..82 |
| `character/mobs` | 83..86 |
| `background_interact` | 87..98 |

> 号段每次跑完以 `report.txt` 为准，别背。
> **想"只追加、不动别人"**：新开一个目录，并把它加在 `PIC_DIRS_DEFAULT` 的**最后一行**。

### 1. 跑

```bash
python tools/img2hex/pic2pram.py                 # 正常转换（要 Pillow）
python tools/img2hex/pic2pram.py --budget 8192   # 只看 PRAM 预算够不够，不写文件
python tools/img2hex/pic2pram.py --dir res/pic/背景 res/pic/角色   # 临时换目录（不改默认表）
```

一次写 4 处（脚本自己做的，不用手拷）：

| 产物 | 去哪 |
|---|---|
| `assets.hex` + `palette.hex` | `project/verified/06_SoC_2DGPU/project/`、`project/GamingPlayUnit/hex/` |
| `assets_desc.h` | 3 个 `peripheral/APB0/gpu2d/`（含固件用的那份） |
| `report.txt` | `tools/img2hex/output/` |

### 2. 看报告（30 秒）

`tools/img2hex/output/report.txt`：

- 头两行：扫了哪些目录、写进了哪些工程；
- `PRAM : 共 N 字 = X KB` —— **上限 32768 字 / 128 KB**，超了就得缩图或拆；
- 表格里找到你的图：**宽 / 高 / 跨距 / 基址**，基址就是 `ASSET_*` 里那个数；
- `透明` / `不透明纯白` 两列：纯白不为 0 会被 ck 抠洞（脚本会自检报警）。

### 3. 四件套，少一件都不行

1. `assets.hex` / `palette.hex` 写进工程 ✔（脚本）
2. `assets_desc.h` 拷到 3 个 `gpu2d/` ✔（脚本）
3. **重新综合** —— hex 是 BRAM 初值，只重编软件没用
4. **重编固件**，然后**固件 + 比特流一起烧**（只烧一个 = 两边对不上，纹理全串）

### 4. 编号变了怎么办

跑完和上一次的 `report.txt` 比一下：**只要有一条编号变了，第 3 步就非做不可**。

C 里用的是生成出来的 `ASSET_<名字>` 宏，所以：

- **改名 / 拆图**会让旧宏消失 → 编译直接报错，按提示改名字就行
  （例：`background_interact_box` 拆成了 `_box_close` / `_box_open`，`game_stone.c` 要跟着改）；
- **只是编号后移** 不会报错，但如果不重新综合，板上就会"地板变成墙、子弹变成金币"这种错位。
