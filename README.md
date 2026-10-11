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

**赛题要求的功能性指标已全部做完并在硬件上验证**：BitBlt（块拷贝 / 纯色填充）、1080p60 显示、双缓冲翻页、
**颜色键控**、**X/Y 镜像**、**硬件 Alpha 混合（12 个 DSP）**；
另外自加了 **图片合成**与**颜色合成**（第 2 层给一个纯色 ⇒ 染色 / 加光 / 乘系数 / 正片叠底，多花 12 个 DSP —— 整机 32），
demo 里就是"撞击闪红"（白→红 3 下）；分屏 **CPU vs 硬件帧率对比实测 5 : 60**。
工程也已整理成"发布工程 + 已验证工程归档"的形式（见下）。剩下的是**高负载游戏 Demo** 与演示材料。

---

## 目录结构

```
FPGADesign_Elitestek/
│
├─ RTL/                          ★ 发布 RTL（发布工程直接读这里）
│  ├─ GamingPlayUnit_Top.v       顶层
│  ├─ Peripherial/GPU_2D/        2D 加速器（cmd / pixel / factory / process / interface / lineBuffer）
│  ├─ Peripherial/APB0/          APB 子系统（按键 / GPU_2D 寄存器）
│  ├─ Peripherial/AXI4/          AXI4 主/从
│  ├─ Axi_Mux.v                  多主汇聚（X4）
│  └─ ddr3_controller/           官方 DDR3 控制器
│
├─ project/
│  ├─ GamingPlayUnit/            ★★ 发布工程（工程形式）
│  │  ├─ GamingPlayUnit.xml      设计文件表（指向仓库 ../../RTL/）
│  │  ├─ hex/                    ★ 素材初值（assets.hex / palette.hex，$readmemh 按工程目录找）
│  │  └─ embedded_sw/.../standalone/GamingPlayUnit/
│  │        src/                 应用（main.c / demo_* / IRQ_frame）
│  │        peripheral/          驱动（APB0 的 key / lineBuffer / gpu2d，CInstruction）
│  │        makefile              这份是模板版（★ IDE 向导生成的那份不含 peripheral/）
│  └─ verified/                  已验证工程归档（不再改动）
│     ├─ 01_LED/  02_HDMI/  04_SoC_APB/  05_SoC_AXI/
│     └─ 06_SoC_2DGPU/           开发阶段的完整工程（TB 从它的 RTL 编译）
│
├─ docs/
│  ├─ 赛题/                      官方选题指南（赛题一~四 + 自由选题）
│  ├─ 芯片手册/                  Ti60 数据手册 v3.8
│  ├─ 参考/                      AXI总线解析.md（协议速查）
│  ├─ 易灵思资料/                官方技术文档与参考工程（大文件不入库）
│  └─ spec/                      ★ 自建规格
│     ├─ 设计总纲.md             系统分层 / 地址 / 带宽 / 时钟 / 纪律 / 里程碑
│     ├─ 硬件_显示通路.md        lineBuffer：DDR → HDMI 1080p60
│     ├─ 硬件_加速器.md          GPU_2D：CI 命令接口 / 数据通路 / alpha / ck / 镜像 / 图片合成 / 颜色合成
│     ├─ 软件_驱动与API.md       C 侧 API / 时序纪律 / 图层 / 脏区 / 自测模式
│     ├─ 实测数据.md             ★ 所有量过的数（资源 / 带宽 / 命令开销 / TB / 上板）
│     ├─ 功能拓展表.md           候选功能三态表（✅ / ❌ / ⬜）
│     ├─ 硬件_RTL规范.md         RTL 写法约定（命名 / 文件结构 / 顶层只连线 / 改 RTL 的纪律）
│     ├─ 待实现清单.md           （临时）下一步要做的功能清单
│     └─ _归档/                  整理前的 14 份旧稿（只读留档）
│
├─ sim/
│  ├─ tb/                        19 个 TB（含 1 个历史遗留的 tb_combine）+ run_tb.ps1 + ALPHA_RESULT.md
│  └─ host/                      宿主端算法仿真（脏区）
│
├─ res/
│  ├─ pic/                       ★ 素材原图（打包的输入）
│  │  ├─ character/              角色跑动帧（#0,#1）
│  │  ├─ base/num/               数字 0-9（#2..#11）
│  │  ├─ alpha/                  带 α 的素材：玻璃（#12）
│  │  ├─ logo/                   比赛 logo 256×92（#13）
│  │  └─ _原始图/                ★ 不参与打包的原图（比如 5087×1772 的 logo 原稿）
│  ├─ func/                      pic2pram.py（素材 → PRAM/调色板）+ 预览/对拍脚本 + output/
│  └─ example/                   里程碑快照（RTL + Code + project 三件套）
│
├─ code/                         C 代码归档（导入方法见 code/README_如何导入C文件到工程.md）
├─ tools/                        构建 / 仿真 / 调试脚本与排查记录
└─ AI_WorkSpace/                 AI 协作工作区（缓存 / 日志 / 输出，不入库）
```

> **改哪里（最容易搞混的一条）**
>
> - **改 RTL 改仓库根 `RTL/`** —— 发布工程 `GamingPlayUnit` 的设计文件表指向它；改完**要重新综合**
> - **改固件改 `project/GamingPlayUnit/embedded_sw/.../standalone/GamingPlayUnit/src|peripheral`**（只重编 app，不用重新综合）
> - **改素材改 `res/pic/*` 然后跑 `res/func/pic2pram.py`** —— 它会自动把 hex 放到每个工程的 hex 位置；
>   素材和调色板是 **BRAM 初值**，所以**一改就必须重新综合**
> - `project/verified/` 是**冻结的已验证工程**（TB 从 `verified/06_SoC_2DGPU/RTL` 编译），平时不用动

---

## 素材怎么加（`res/func/pic2pram.py`）

```
python res/func/pic2pram.py            # 打 res/pic 下默认那几个目录（新目录要加进脚本的 PIC_DIRS_DEFAULT）
```

产物：`assets.hex`（PRAM 初值）、`palette.hex`（一项 = `AARRGGBB`）、`assets_desc.h`（C 侧素材表）、
`report.txt`（这次转换的账）、`preview_*.png`（量化后的预览）。脚本会顺手把这些放到：

| 放哪 | 为什么 |
|---|---|
| `res/func/output/` | 归档 + 给 C 用 |
| `<每个工程>/hex/` 或工程根 | `$readmemh` 的**相对路径按工程目录解析**，少一份就报 `VERI-1012 cannot open file` |
| 各 C 工程的 `peripheral/APB0/gpu2d/` | 固件 `#include "assets_desc.h"` |

**三条规矩（踩过才写下来的）**

1. **α 只能用固定档位**（0 / 128 / 255 这种）。硬件是"一个索引 = 一种 (颜色, α)"，
   α 一旦是连续值（抗锯齿、被缩放出来的），调色板 256 项会被拆爆 —— 脚本会直接报错退出并指出是哪张图。
2. **尺寸在绘图工具里定好**，宽高都**对齐到 4**。`--max` 一旦触发缩放，α 就变连续值（同上）。
3. **新素材加在目录顺序的最后**（`PIC_DIRS_DEFAULT` 的顺序 = 素材编号），这样已有素材的编号不动。

当前用量：**14 张素材、PRAM 12870 / 32768 字（39%）、调色板 26 / 256 项**。

---

## 关键资料索引

- **赛题二要求**：`docs/赛题/…2026FPGA赛道选题指南-易灵思.pdf`（第 7–9 页；纯文本提取件 `AI_WorkSpace/output/赛题原文提取.txt`）
  - 基础：BitBlt（Block Copy / Solid Fill）+ 640×480@60 显示 + AXI 突发 / FIFO + 双缓冲 + **CPU vs 硬件帧率对比**
  - 高阶：**硬件 Alpha 混合**（用 DSP）、**颜色键控**、**高同屏元素数游戏 Demo 60 FPS**
- **设计规格**：`docs/spec/` —— 先看 `设计总纲.md`，再看对应的部件文件；**改 RTL 之前先看 `硬件_RTL规范.md`**
- **实测数字**：`docs/spec/实测数据.md`（引用前先看日期；设计估算与实测分开写）
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

> 命令行编固件（可选）：在 app 目录下 `make BSP=efinix/EfxSapphireSoc`，产物在 `build/`；
> 烧 Flash 用的是 `build/<工程名>.hex`（当前 15 KB 左右，RAM 用 ~20 KB / 124 KB）。

---

## 进度

### 已完成（均已在硬件上验证）

- [x] 资料收集与整理；`docs/spec` 规格文档（6 份 + 归档）
- [x] HDMI 输出链路（1080p60 出图）
- [x] SoC + RISC-V 跑通；自定义 APB 外设（按键）；JTAG co-debug + UART
- [x] DDR3 接入 + `Axi_Mux` 多主（**X4**：S0 CPU / S1 显示 / S2 加速器读 / S3 加速器写）
- [x] **视频显示通路 lineBuffer**：DDR 取数 → 跨域双 bank → 拆包 → 1080p60 TMDS
      全屏 / 双缓冲翻页 / 分屏（左右独立翻页）/ 全屏↔分屏运行时切换 / ENABLE 门控 / 帧率统计
- [x] **M0** SoC IP 打开 CFU（自定义指令通道）
- [x] **M1** CI 从端 → 命令 FIFO → **纯色填充** → AXI 写 → 交帧翻页（上板）
- [x] **M1.5** APB 槽 2 寄存器 + C 驱动（ID / 计数器 / 状态）
- [x] **M2** **块拷贝**（读通路 + 行缓冲 + 合成器）；整链逐字节 err=0
- [x] **M3** **片上素材 + 调色板查表**（PRAM 128 KB，命令只带字偏移，零 DDR 读）
- [x] **颜色键控 ck**（`flags[4]` + APB 键色寄存器）
- [x] **X / Y 镜像**（`flags[6:5]`，人物左右跑）
- [x] **Alpha 混合**（`MODE=2`，α 走调色板高字节，背景 = 读回目的，**12 个 DSP**）—— 上板验证通过
- [x] **图片合成**（一条命令 2 张 PRAM 素材图逐字节合成）—— 上板验证通过
- [x] **颜色合成**（`MODE=3` 纯色层：插值染色 / 加光 / 乘系数 / 正片叠底 + 强度，**12 个 DSP**）—— 上板验证通过（demo 的"撞击闪红"）
- [x] **脏区 + 分层重放**（每块缓冲各记一份旧位置；层号 + 稳定排序；HUD 作为最高层）
- [x] **M4** 分屏 CPU vs 硬件帧率对比 Demo —— **上板实测 CPU 5 fps : 加速器 60 fps**
- [x] 仿真：**19 个 TB**（含 1 个历史遗留的 `tb_combine`），最近一次全量回归全部通过（清单见 `docs/spec/实测数据.md` §4）；`tb_m2loopback` 是端到端那一条，现在 **10 段 / 880 行**逐字节比对
- [x] **工程整理**：抽取发布工程 `GamingPlayUnit`（RTL 收进仓库 `RTL/`、素材初值收进 `hex/`、
      固件移入 `embedded_sw/.../standalone/GamingPlayUnit` 并**编译通过**）；旧工程存入 `project/verified/`

### 进行中 / 待做

- [ ] **M5 高负载游戏 Demo**（赛题高阶第 3 条）：同屏元素数阶梯加压 + 稳定 60 FPS + 屏上 HUD 显示元素数 / FPS
      —— 方案见 `docs/spec/功能拓展表.md` §8、待办见 `docs/spec/待实现清单.md`
- [ ] **命令开销基线测量**：各命令（FILL / sprite / spriteCK / spriteAlpha）× 尺寸的单价曲线
      —— 没有它就无法回答"元素数极限在哪"（`docs/spec/实测数据.md` §3.3）
- [ ] **演示视频 / 报告 / 资源与性能分析**（含带宽账、DSP 使用、资源占用表）
- [ ] 素材通路（Flash → DDR）扩容：目前素材走片上 PRAM（换素材要重新综合），
      大素材库需要 Flash 素材区 + 开机装载（`docs/spec/设计总纲.md` §5.2）

> **checker 说明**：DDR 自检 checker 曾用来验证 AXI 通路，**已从系统摘除**（省约 2K LUT）。
> 它腾出来的位置已经改派：APB 槽 1 与 AXI S1 都归 lineBuffer，加速器统一用 **APB 槽 2 + AXI S2/S3**。

