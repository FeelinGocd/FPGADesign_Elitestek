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

```
FPGADesign_Elitestek/
│
├─ docs/                                    # 文档区（软硬件并行契约 + 官方资料）
│  ├─ 赛题/                                 官方选题指南（赛题一~四 + 自由选题要求）
│  ├─ 芯片手册/                             Ti60 数据手册 v3.8（资源 / 性能 / 时序）
│  ├─ 易灵思资料/                           官方技术文档（TinyML / RISC-V Custom Instruction / DSP 原语）
│  │  ├─ TinyML和RISCV相关/                 EVSOC_tinyml 用户手册、TinyML / Custom Instruction / DSP 原语 PDF
│  │  ├─ 参考工程包/                        Demo.zip、Ti60F225_DemoBoard_v4 完整资料包（17 个官方参考工程 · 大文件不入库）
│  │  └─ 开发板/                            Ti60F225A_V4 原理图 + DEMO 板软硬件设计说明 PDF
│  └─ spec/                                 自建规格：架构设计 / 寄存器映射 / 接口契约
│
├─ RTL/                                     # RTL 区（自研模块，源码主体）
│  ├─ bitblt/                               BitBlt 引擎：命令解析 / Block Copy /
│  │                                        Solid Fill / Alpha 混合 / ColorKey（占位）
│  ├─ HDMI/                                 HDMI 输出驱动：时序 / 编码 / 复位同步
│  │  ├─ Pixel_Timing/                      像素时序生成（pixel_timing.v）
│  │  └─ TMDS/                              8b10b 编码（dvi_encoder_Top.v / tmds_encoder.v）
│  ├─ mem/                                  AXI4 主端、突发传输、FIFO、DDR 仲裁（占位）
│  ├─ soc/                                  Sapphire RISC-V SoC 集成（官方）（占位）
│  └─ video/                                显示/帧缓冲读路径、行缓存 / PingPongBuffer（占位）
│
├─ sim/
│  └─ tb/                                   各模块 testbench（tb_<模块名>.v）
│
├─ src/                                     # 官方参考例程的可编辑工作副本
│  ├─ example/
│  │  ├─ 01_LED/                            实例 LED 参考例程（RTL + Efinity 配置）
│  │  └─ 02_HDMI/                           实例 02/03 HDMI 参考例程（RTL + Efinity 配置 + README_HDMI 接口记录）
│  └─ pic/                                  图片素材
│
├─ project/                                 # Efinity 工程（本地生成，不入库）
│
├─ tools/                                   构建 / 仿真 / 资料处理脚本
│
└─ AI_WorkSpace/                            AI 协作工作区：缓存 / 日志 / 输出（不入库）
```

> 归档约定：`project/`（Efinity 生成物）与 `AI_WorkSpace/`（AI 缓存/日志/输出）、`docs/易灵思资料/参考工程包/` 大文件均不入库（见根目录 `.gitignore`）。

## 关键资料索引

- **赛题二要求**：`docs/赛题/…2026FPGA赛道选题指南-易灵思.pdf`（第 7–9 页）
  - 基础：BitBlt（Block Copy / Solid Fill）+ 640×480@60 VGA/HDMI + AXI 突发/FIFO + 双缓冲 + CPU vs 硬件帧率对比
  - 高阶：硬件 Alpha 混合、Color Keying、高同屏 Sprite 游戏 60FPS
- **芯片资源**：`docs/芯片手册/ti60-ds-v3.8芯片手册.pdf`
  - 60,800 LE / 62,016 XLR / 2.6 Mbit BRAM（256×10K）/ 160 DSP / 4 PLL
- **官方参考例程**：`docs/易灵思资料/参考工程包/`（解压后重点：02/03_hdmi、08_soc、10_sc431hai2hdmi、04_GE、17_uart）
- **软核与加速**：`docs/易灵思资料/TinyML和RISCV相关/`（Sapphire RISC-V、TinyML、Custom Instruction、DSP 原语）
- **开发板**：`docs/易灵思资料/开发板/`（原理图 SCH + DEMO 板软硬件设计说明）

## 开发环境

| 工具 | 版本 | 下载/来源 | 备注 |
|---|---|---|---|
| Elitestek® | 2026.1 | https://www.elitestek.com/support/ | 无 |
| Elitestek®补丁 |  | https://www.elitestek.com/support/ | 无 |
| Efinity RISCV IDE | 2026.1 | https://www.elitestek.com/support/ | 与 Efinity 配套 |
| 开发板驱动/下载工具 | — | 群文件 | — |

## 进度

- [x] 资料收集与整理
- [x] docs/spec 规格文档（架构 / 寄存器映射 / 接口契约）
- [x] HDMI 输出链路（RTL/HDMI：pixel_timing + dvi_encoder + LVDS TX 硬核，1080p 彩条已出图；详见 `project/03_HDMI/README_HDMI.md`）
- [ ] RTL 各模块（bitblt / mem / soc / video 占位未实现）
- [ ] 仿真验证
- [ ] 真机 Demo（V1 基础 → V2 高阶 → V3 游戏）
