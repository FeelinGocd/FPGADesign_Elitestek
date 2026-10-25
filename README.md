# 2026 FPGA 创新设计竞赛 · 赛题二：基于 RISC-V 与 FPGA 异构架构的 2D 图形渲染加速引擎

> 全国大学生嵌入式芯片与系统设计竞赛 '2026 · FPGA 赛道（易灵思）
> 平台：Ti60F225（Sapphire™ RISC-V 软核 + DDR3 + HDMI）

## 关键时间节点

| 安排 | 时间 |
|---|---|
| 竞赛报名时间 | 8月~9月22日24点 |
| 作品设计时间 | 9月~11月4日 |
| 作品提交截止时间 | 11月4日18点 |
| 初赛评审时间 | 11月4日-9日 |
| 决赛入围公示 | 11月10日~12日 |
| 决赛时间 | 11月20日~22日 |

---

## 一句话现状

**赛题基础要求 + 三项高阶挑战全部做完并上板验证**：BitBlt（块拷贝 / 纯色填充）、**1280×720@60** 显示、
双缓冲翻页、**颜色键控**、**X/Y 镜像 + 90° 旋转**、**硬件 Alpha 混合**、以及自加的**图片合成 / 颜色合成（染色·加光·乘系数·正片叠底）**；
**分屏 CPU vs 硬件帧率对比实测 5 : 60**；游戏 demo 上板可跑（四方向角色 + 192 发子弹池 + Boss 弹幕 + 道具拾取 + 脏区重绘 + 屏上 HUD 实时显示 FPS / 命令数 / 在场子弹数）。

整机资源：**FF 12211（22%）/ LUT 20263（3%）/ BRAM 243 / DSP 34**（2026-10-17 综合）。

---

## 目录结构

```
FPGADesign_Elitestek/
│
├─ RTL/                          ★ 发布 RTL（发布工程直接读这里）
│  ├─ GamingPlayUnit_Top.v       顶层
│  ├─ Peripherial/GPU_2D/        2D 加速器（cmd / pixel / process / factory / interface / lineBuffer）
│  ├─ Peripherial/APB0/          APB 子系统（按键 / GPU_2D 寄存器）
│  ├─ Peripherial/AXI4/          AXI4 主/从
│  ├─ Axi_Mux.v                  多主汇聚（X4）
│  └─ ddr3_controller/           官方 DDR3 控制器
│
├─ project/
│  ├─ GamingPlayUnit/            ★★ 发布工程
│  │  ├─ GamingPlayUnit.xml      设计文件表（指向仓库 ../../RTL/）
│  │  ├─ hex/                    ★ 素材初值（assets.hex / palette.hex，$readmemh 按工程目录找）
│  │  └─ embedded_sw/.../standalone/GamingPlayUnit/
│  │        src/                 应用入口（main.c / game_demo.h / compare_demo / IRQ_frame）
│  │        game/                ★ 游戏 demo 按功能拆分（game.h + 状态/世界/HUD/角色/子弹/Boss/道具/主循环）
│  │        peripheral/          驱动（APB0 的 key / lineBuffer / gpu2d，CInstruction）
│  │        makefile              这份是模板版（★ IDE 向导生成的那份不含 peripheral/）
│  └─ verified/                  已验证工程归档（不再改动）
│     ├─ 01_LED/  02_HDMI/  04_SoC_APB/  05_SoC_AXI/
│     └─ 06_SoC_2DGPU/           开发阶段的完整工程（历史留档）
│
├─ docs/
│  ├─ 赛题/                      官方选题指南（赛题一~四 + 自由选题）
│  ├─ 芯片手册/                  Ti60 数据手册 v3.8
│  ├─ 参考/                      AXI总线解析.md（协议速查）
│  ├─ 易灵思资料/                官方技术文档与参考工程（大文件不入库）
│  └─ spec/                      ★ 自建规格（6 份 + 归档）
│     ├─ 设计总纲.md             系统分层 / 地址 / 带宽 / 时钟 / 纪律 / 里程碑
│     ├─ 硬件_显示通路.md        lineBuffer：DDR → HDMI 720p60
│     ├─ 硬件_加速器.md          GPU_2D：CI 命令接口 / 数据通路 / alpha / ck / 镜像 / 旋转 / 合成
│     ├─ 软件_驱动与API.md       C 侧 API / 时序纪律 / 图层 / 脏区
│     ├─ 实测数据.md             ★ 所有量过的数（资源 / 带宽 / 命令开销 / TB / 上板）
│     ├─ 硬件_RTL规范.md         RTL 写法约定（命名 / 文件结构 / 顶层只连线 / 改 RTL 的纪律）
│     ├─ 功能拓展表.md           已实现能力清单 + 明确不做的功能及理由
│     └─ _归档/                  整理前的旧稿（只读留档）
│
├─ sim/
│  ├─ tb/                        19 个 TB + run_tb.ps1 / run_regress.ps1
│  └─ host/                      宿主端算法仿真（脏区）
│
├─ res/
│  ├─ pic/                       ★ 素材原图（打包的输入）
│  │  ├─ character/bigFish/      大肥鱼 9 帧（56×72）
│  │  ├─ character/TheBoss/      Boss 3 帧（80×96）
│  │  ├─ base/{num,Eng}/         数字 0-9 + 字母 A-Z（16×24）
│  │  ├─ base/{alpha,background,specialEffect}/
│  │  ├─ character/{bullet,mobs}/ 子弹 / 小怪
│  │  ├─ accessory/               道具
│  │  └─ _原始图/                ★ 不参与打包的原图
│  ├─ func/                      素材打包产物归档（output/，脚本在 tools/）
│  └─ example/                   里程碑快照（RTL + Code + project 三件套）
│
├─ code/                         C 代码归档（导入方法见 code/README_如何导入C文件到工程.md）
├─ tools/                        构建/仿真/调试脚本与排查记录
│  ├─ img2hex/                   素材 → assets.hex / palette.hex / assets_desc.h / report.txt
└─ AI_WorkSpace/                 AI 协作工作区（缓存 / 日志 / 输出，不入库）
```

> **改哪里（最容易搞混的一条）**
>
> - **改 RTL 改仓库根 `RTL/`** —— 发布工程 `GamingPlayUnit` 的设计文件表指向它；改完**要重新综合**
> - **改固件改 `project/GamingPlayUnit/embedded_sw/.../standalone/GamingPlayUnit/src|peripheral`**（只重编 app，不用重新综合）
> - **改素材改 `res/pic/*` 然后跑 `tools/img2hex/pic2pram.py`** —— 它会自动把 hex 放到每个工程的 hex 位置；
>   素材和调色板是 **BRAM 初值**，所以**一改就必须重新综合 + 重编固件**

---

## 素材怎么加（`tools/img2hex/pic2pram.py`）

```
uv sync                                # 第一次 / 换机器：建 .venv 并装依赖（pyserial / pillow）
uv run tools/img2hex/pic2pram.py               # 打 res/pic 下默认那几个目录（新目录要加进脚本的 PIC_DIRS_DEFAULT）
```

> PC 侧的 Python 工具统一用 **uv** 管理：依赖声明在仓库根的 `pyproject.toml`，锁文件 `uv.lock` 入库。
> 编辑器（VS Code）请把解释器选成 **`.venv\Scripts\python.exe`**（`.vscode/settings.json` 里已经配好默认值），
> 否则 `import serial` / `from PIL import Image` 会报"无法解析导入"。

产物：`assets.hex`（PRAM 初值）、`palette.hex`（一项 = `AARRGGBB`）、`assets_desc.h`（C 侧素材表，用 `ASSET_*` 名字）、
`report.txt`（这次转换的账）。脚本会顺手把这些放到：

| 放哪 | 为什么 |
|---|---|
| `project/GamingPlayUnit/hex/` | 发布工程的综合初值（`assets.hex` / `palette.hex`） |
| `project/verified/06_SoC_2DGPU/project/` | 已验证工程的同类副本（RTL 的 `INIT_FILE` 写在工程根） |
| `tools/img2hex/output/` | `report.txt` + `assets_desc.h`（工具自己的账和素材表） |
| 各 C 工程的 `peripheral/APB0/gpu2d/` | 固件 `#include "assets_desc.h"` |

**三条规矩（踩过才写下来的）**

1. **α 只能用固定档位**（本工程是 0 / 128 / 255）。硬件是"一个索引 = 一种 (颜色, α)"，
   α 一旦是连续值（抗锯齿、被缩放出来的），调色板 256 项会被拆爆 —— 脚本会直接报错退出并指出是哪张图。
2. **尺寸在绘图工具里定好**，宽高都**对齐到 4**，最大边 **256**。`--max` 一旦触发缩放，α 就变连续值（同上）。
3. **新素材加在目录顺序的最后**（`PIC_DIRS_DEFAULT` 的顺序 = 素材编号），这样已有素材的编号不动。
4. **不透明区里不许有纯白**（键色是白，会被 ck 抠掉）—— 脚本会自检，不合格直接报错。
5. **朝左的帧不用画**：命名带 `_left` 的会被跳过，要用时开 `FLIP_X`。

当前用量：**99 张素材、PRAM 27264 / 32768 字（83.2%）、调色板 161 / 256 色**（`tools/img2hex/output/report.txt`，2026-10-25 重画木地板/墙/墙角后重跑）。

---

## 关键资料索引

- **赛题二要求**：`docs/赛题/…2026FPGA赛道选题指南-易灵思.pdf`（第 7–9 页；纯文本提取件 `AI_WorkSpace/output/赛题原文提取.txt`）
- **设计规格**：`docs/spec/` —— 先看 `设计总纲.md`，再看对应的部件文件；**改 RTL 之前先看 `硬件_RTL规范.md`**
- **实测数字**：`docs/spec/实测数据.md`（引用前先看日期）
- **芯片资源**：`docs/芯片手册/ti60-ds-v3.8芯片手册.pdf`（60800 LE / 256 块 10K BRAM / 160 DSP / 4 PLL）
- **官方参考例程**：`docs/易灵思资料/参考工程包/`（重点：02/03_hdmi、08_soc、10_sc431hai2hdmi、04_GE、17_uart）
- **软核与加速**：`docs/易灵思资料/TinyML和RISCV相关/`（Sapphire RISC-V、TinyML、Custom Instruction、DSP 原语）
- **开发板**：`docs/易灵思资料/开发板/`（原理图 + DEMO 板软硬件设计说明）

---

## 开发环境

| 工具 | 版本 | 来源 | 备注 |
|---|---|---|---|
| Efinity | 2026.1.132.3.9 | elitestek.com | 无 |
| Efinity RISCV IDE | 2026.1 | elitestek.com | 与 Efinity 配套 |
| RISC-V 工具链 | riscv-none-elf-gcc 13.4.0 | IDE 自带 | `EfinityRISCV\toolchain` |
| make | — | IDE 自带 | `EfinityRISCV\build_tools\bin\make.exe` |
| OpenOCD | 0.11.0+dev | IDE 自带 | `EfinityRISCV\openocd` |
| ModelSim | SE-64 10.5 | 本机 | `sim/tb/run_tb.ps1 -TbDir <目录> -TbTop <顶层>` |
| 开发板驱动 / 下载工具 | — | 群文件 | 见 `tools/JTAG_UART_驱动问题排查.md` |
| **uv**（PC 侧 Python 工具） | 0.11.x | astral.sh | 依赖看 `pyproject.toml`；`uv sync` 后 `uv run <脚本>` |

> 命令行编固件：在 app 目录下 `make all BSP=efinix/EfxSapphireSoc`（工具链与 build_tools 要在 PATH 里），产物在 `build/`；
> 烧 Flash 用 `build/<工程名>.hex`（当前固件 ~68 KB，RAM 用 ~90 KB / 124 KB，约 71%）。
> 仓库根有 `.vscode/`（编译器路径 / 头文件路径 / 编 . 清 . 打包任务），VS Code 打开仓库根就能直接跳转和编译。

---

## 进度

### 已完成（均已在硬件上验证）

- [x] 资料收集与整理；`docs/spec` 规格文档
- [x] HDMI 输出链路（720p60 出图，物理行跨距保持 5760）
- [x] SoC + RISC-V 跑通；自定义 APB 外设（按键）；JTAG co-debug + UART
- [x] DDR3 接入 + `Axi_Mux` 多主（**X4**：S0 CPU / S1 显示 / S2 加速器读 / S3 加速器写）
- [x] **视频显示通路 lineBuffer**：DDR 取数 → 跨域双 bank → 拆包 → TMDS
      全屏 / 双缓冲翻页 / 分屏（左右独立翻页）/ 全屏↔分屏运行时切换 / ENABLE 门控 / 帧率统计
- [x] **M0~M1.5** SoC IP 打开 CFU；CI 从端 → 命令 FIFO → 纯色填充 → AXI 写 → 交帧；APB 槽 2 寄存器 + C 驱动
- [x] **M2** 块拷贝（读通路 + 行缓冲 + 合成器）；整链逐字节 err=0
- [x] **M3** 片上素材 + 调色板查表（PRAM 128 KB，命令只带字偏移，零 DDR 读）
- [x] **颜色键控 ck**（`flags[4]` + APB 键色寄存器）
- [x] **X / Y 镜像**（`flags[6:5]`）+ **硬件 90° 旋转**（`flags[7]`，按列取样）
- [x] **Alpha 混合**（`MODE=2`，α 走调色板高字节，背景 = 读回目的，**12 个 DSP**）
- [x] **图片合成**（一条命令 2 张 PRAM 素材图逐字节合成）
- [x] **颜色合成**（`MODE=3` 纯色层：插值染色 / 加光 / 乘系数 / 正片叠底 + 强度，**12 个 DSP**）—— demo 的"受击闪红"
- [x] **脏区 + 分层重放**（每块缓冲各记一份旧位置；层号 + 稳定排序；HUD 作为最高层）
- [x] **M4** 分屏 CPU vs 硬件帧率对比 Demo —— **上板实测 CPU 5 fps : 加速器 60 fps**
- [x] **M5** 高负载游戏 Demo：四方向角色（每方向序列表 + 镜像复用一套素材）+ **192 发子弹池** + Boss 3 帧 + 4 种弹幕 + 道具拾取 + 受击闪红 + 脏区重绘 + 屏上 HUD（FPS / 本帧命令数 / 在场子弹数）—— 已上板运行正常
- [x] 仿真：**19 个 TB**（`sim/tb/run_regress.ps1` 一键全跑）；`tb_m2loopback` 是端到端那一条，**11 段 / 936 行任务**逐字节比对
- [x] **工程整理**：发布工程 `GamingPlayUnit`（RTL 收进 `RTL/`、素材初值收进 `hex/`、固件移入 `embedded_sw/` 并编译通过）；旧工程存入 `project/verified/`

### 收尾

- [ ] 演示视频 / 报告 / 资源与性能分析（带宽账、DSP 使用、资源占用表）

