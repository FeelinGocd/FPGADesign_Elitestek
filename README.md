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

## 目录结构

| 目录 | 内容 | 说明 |
|---|---|---|
| `docs/赛题/` | 官方选题指南 | 赛题一~四 + 自由选题要求 |
| `docs/芯片手册/` | Ti60 数据手册 v3.8 | 芯片资源 / 性能 / 时序参考 |
| `docs/开发板资料/` | EVSOC TinyML 用户手册、TinyML 介绍/框架、RISC-V Custom Instruction、钛金 DSP 原语 | 官方技术文档（PDF） |
| `docs/开发板资料/参考工程包/` | Demo.zip、Ti60F225_DemoBoard_v4 完整资料包 | 17 个官方参考工程（大文件，不入库） |
| `docs/spec/` | 自建规格：架构设计、寄存器映射、接口契约 | 软硬件并行开发的契约文档 |
| `RTL/bitblt/` | BitBlt 引擎：命令解析 / Block Copy / Solid Fill / Alpha 混合 / ColorKey | 自研 RTL |
| `RTL/video/` 或 `RTL/HDMI/` | HDMI 时序生成与输出、行缓存 | 自研 RTL（命名二选一，待定） |
| `RTL/mem/` | AXI4 主端、突发传输、FIFO、DDR 仲裁 | 自研 RTL |
| `RTL/soc/` 或 `RTL/RISCV/` | Sapphire RISC-V SoC 集成 | 官方 + 自研（命名二选一，待定） |
| `RTL/top/` | 顶层例化与系统集成 | 集成负责人独占 |
| `sim/tb/` | 各模块 testbench（`<模块>_tb.v`） | |
| `sim/scripts/` | 仿真运行 / 回归脚本 | |
| `constr/` | 引脚与时序约束（PDC / SDC） | 赛题要求提交 |
| `sw/drivers/` | RISC-V C 驱动：blit / fill / video / uart 等 | |
| `sw/app/` | 游戏 / GUI Demo 逻辑、FPS 统计与叠加 | |
| `project/` | Efinity 工程（本地生成，不入库） | |
| `tools/` | 构建 / 仿真 / 资料处理脚本 | |
| `AI_WorkSpace/` | AI 协作工作区（缓存 / 日志 / 输出，不入库） | |

## 关键资料索引

- **赛题二要求**：`docs/赛题/…2026FPGA赛道选题指南-易灵思.pdf`（第 7–9 页）
  - 基础：BitBlt（Block Copy / Solid Fill）+ 640×480@60 VGA/HDMI + AXI 突发/FIFO + 双缓冲 + CPU vs 硬件帧率对比
  - 高阶：硬件 Alpha 混合、Color Keying、高同屏 Sprite 游戏 60FPS
- **芯片资源**：`docs/芯片手册/ti60-ds-v3.8芯片手册.pdf`
  - 60,800 LE / 62,016 XLR / 2.6 Mbit BRAM（256×10K）/ 160 DSP / 4 PLL
  - MIPI、LVDS 最高 1.5 Gbps；BRAM / DSP Fmax 最高 1 GHz
- **参考工程**：`docs/开发板资料/参考工程包/`（解压后重点：02/03_hdmi、08_soc、10_sc431hai2hdmi、04_GE、17_uart）
- **软核与加速**：`docs/开发板资料/`（Sapphire RISC-V、TinyML、Custom Instruction、DSP 原语）

## 开发环境

- Efinity® IDE（RTL → bitstream）
- Sapphire RISC-V SDK（C 语言开发）
- 仿真：ModelSim / Questa（官方参考工程 `src/` 内含 `sim.do`）

## 进度

- [x] 资料收集与整理
- [ ] docs/spec 规格文档（架构 / 寄存器映射 / 接口契约）
- [ ] RTL 各模块
- [ ] 仿真验证
- [ ] 真机 Demo（V1 基础 → V2 高阶 → V3 游戏）
