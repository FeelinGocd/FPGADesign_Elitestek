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
├─ docs/                                    # 文档区
│  ├─ 赛题/                                 官方选题指南（赛题一~四 + 自由选题要求）
│  ├─ 芯片手册/                             Ti60 数据手册 v3.8（资源 / 性能 / 时序）
│  ├─ 易灵思资料/                           官方技术文档
│  │  ├─ TinyML和RISCV相关/                 EVSOC_tinyml 用户手册、Custom Instruction、DSP 原语
│  │  ├─ 参考工程包/                        Demo / Ti60F225_DemoBoard_v4（大文件不入库）
│  │  └─ 开发板/                            Ti60F225A_V4 原理图 + DEMO 板软硬件设计说明
│  └─ spec/                                 自建规格：架构设计 / 总线逻辑 / 渲染流程
│
├─ RTL/                                     # 自研 RTL 模块（仓库副本）
│  ├─ bitblt/                               BitBlt 引擎（占位）
│  ├─ HDMI/                                 HDMI 输出驱动【已完成】
│  │  ├─ Pixel_Timing/                      像素时序生成
│  │  └─ TMDS/                              8b10b 编码
│  ├─ mem/                                  AXI4 主端 / 突发 / FIFO / DDR 仲裁（占位）
│  ├─ soc/                                  SoC 集成
│  │  └─ Peripherial/                       ★ 自定义 APB 外设【已完成】
│  │     ├─ APBX_Interface.v                外设接口模板（参考用）
│  │     ├─ APB0/APB0_Decoder.v             地址译码（槽 0 = KEY）
│  │     ├─ APB0/APB0_Interconnect.v        外设互联 + 返回复用
│  │     ├─ APB0/key/APB0_key.v             按键寄存器（APB 侧）
│  │     └─ APB0/key/key.v                  同步 / 消抖 / 边沿
│  └─ video/                                显示 / 帧缓冲读路径（占位）
│
├─ code/                                    # ★ C 代码（给最终工程逐步积累）
│  ├─ peripheral/                           外设驱动（和 RTL/soc/Peripherial 一一镜像）
│  │  └─ APB0/key/                          user_key.h / user_key.c
│  ├─ app/                                  应用代码（给最终工程，预留）
│  ├─ logic/                                算法 / 逻辑代码（预留）
│  └─ README_如何导入C文件到工程.md          ★★ 用法看这份
│
├─ res/                                     # ★ 最小可复现集（里程碑快照）
│  ├─ example/                              每个例程 = RTL + Code + project 三件套
│  │  ├─ 01_LED/                            LED 参考例程
│  │  ├─ 02_HDMI/                           HDMI 输出（含 README_HDMI.md）
│  │  ├─ 03_SoC_LED/                        SoC GPIO 点灯（含 README_SoC_LED.md）
│  │  └─ 04_SoC_APB0_key/                   APB 总线按键外设
│  │     ├─ RTL/                            该里程碑的 RTL
│  │     ├─ Code/                           该里程碑的 C 代码（main.c / userDef.h / makefile）
│  │     └─ project/project.peri.xml        该里程碑的引脚约束
│  ├─ pic/                                  图片素材
│  └─ ftdi.cfg / ftdi_ti.cfg                OpenOCD 的 FTDI 配置（复现要用）
│
├─ project/                                 # Efinity 工程（本地生成，不入库）
│  ├─ 01_LED/
│  ├─ 02_HDMI/
│  └─ 03_SoC_LED/
│
├─ sim/
│  └─ tb/                                   各模块 testbench（tb_<模块名>.v）
│
├─ tools/                                   构建 / 仿真 / 资料处理脚本、排查记录
│
└─ AI_WorkSpace/                            AI 协作工作区：缓存 / 日志 / 输出（不入库）
```

> **归档约定**
>
> - `project/`（Efinity 生成物）、`AI_WorkSpace/`、`docs/易灵思资料/参考工程包/` 大文件 **不入库**（见 `.gitignore`）
> - **改代码改 `project/<工程>/RTL/Peripherial/`** —— Efinity 就从那儿读，是工作副本
> - **`RTL/` 和 `code/` 是给「最终工程」逐步积累的** —— 不按测试工程分目录；测通一个模块就放进来
> - **`res/example/<编号>_<名字>/` 是里程碑快照** —— 自带 `RTL/` + `Code/` + `project/`，换台设备能直接复现
> - C 代码的导入方法见 `code/README_如何导入C文件到工程.md`

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

| 工具 | 版本 | 来源 | 备注 |
|---|---|---|---|
| Elitestek® | 2026.1.132.3.9 | elitestek.com | 无 |
| Elitestek®补丁 |  | elitestek.com | 无 |
| Efinity RISCV IDE | 2026.1 | elitestek.com | 与 Efinity 配套 |
| RISC-V 工具链 | riscv-none-elf-gcc 13.4.0 | IDE 自带 | `EfinityRISCV\toolchain` |
| OpenOCD | 0.11.0+dev | IDE 自带 | `EfinityRISCV\openocd` |
| 开发板驱动/下载工具 | — | 群文件 | 见 `tools/JTAG_UART_驱动问题排查.md` |

## 进度

- [x] 资料收集与整理
- [x] `docs/spec` 规格文档（架构 / 总线逻辑 / 渲染流程）
- [x] HDMI 输出链路（1080p 彩条已出图，见 `project/02_HDMI/README_HDMI.md`）
- [x] SoC + RISC-V 跑通（`project/03_SoC_LED`）
- [x] 自定义 APB 总线外设：按键（`APB0_Interconnect` + `APB0_key`，ID 自检通过）
- [x] 按键 → LED 跑通（板载 4 键 4 灯）
- [x] 板载调试链路（JTAG co-debug + UART 串口）
- [ ] RTL 各模块（bitblt / mem / soc / video 占位未实现）
- [ ] 加速器挂到 APB0（槽 1~15，`0xf8101000` ~ `0xf810FFFF`）
- [ ] 仿真验证
- [ ] 真机 Demo（V1 基础 → V2 高阶 → V3 游戏）
