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
│  │     ├─ APB0/APB0_Decoder.v             地址译码（槽 0 = KEY / 槽 1 = lineBuffer）
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
│  ├─ 03_SoC_LED/
│  └─ 05_SoC_AXI/                           # ★ 当前主线：SoC + DDR3 + 视频显示通路
│                                            #   RTL/Accelerator_Top.v  顶层
│                                            #   RTL/Axi_Mux.v          多主汇聚（X3）
│                                            #   RTL/Peripherial/LineBuffer/   显示通路
│                                            #   RTL/Peripherial/APB0/         APB 子系统
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

### 已完成（均已在硬件上验证）

- [x] 资料收集与整理
- [x] `docs/spec` 规格文档（总线逻辑 / 地址空间规划 / 渲染流程 / AXI4 解析 / **视频显示通路**）
- [x] HDMI 输出链路（1080p60 已出图，见 `project/02_HDMI/README_HDMI.md`）
- [x] SoC + RISC-V 跑通（`project/03_SoC_LED`）
- [x] 自定义 APB 外设：按键（槽 0，ID 自检通过）
- [x] 按键 → LED 跑通（板载 4 键 4 灯）
- [x] 板载调试链路（JTAG co-debug + UART 串口）
- [x] DDR3 接入 + `Axi_Mux` 多主（`X3`：S0 = SoC / S1 = lineBuffer / S2 = 空闲）
- [x] **视频显示通路 lineBuffer（APB 槽 1 + AXI S1）**：
      DDR 取数 → 跨域双 bank → 128→24 拆包 → 1080p60 TMDS，CPU 只写寄存器
      - 全屏显示、双缓冲翻页（DONE 交帧）
      - 分屏（左右两块独立半幅）+ 左右**独立**翻页
      - 全屏 ↔ 分屏 **运行时切换**
      - ENABLE 门控、帧率统计（FPS_ACC / FPS_CPU）
      - 实现逻辑、寄存器表、帧缓存格式、握手约定见 **`docs/spec/视频显示通路.md`**
- [x] 仿真：5 个 TB（PingPong / 拆包 / 控制器 / APB+handler / **整链**）全绿
- [x] 板级调试手段：按键控制显示切换 + 串口观测（`user_lineBuffer` 驱动 + 6 种测试模式）

### 进行中 / 待做

- [ ] **加速器接入**（最后做）：APB 槽 2（`0xf8102000`~）+ AXI `S2`（已留空接常量）
      + 两个握手信号（`lineBuffer_frame_start` / `acc_frame_done`），
      接口约定见 `docs/spec/视频显示通路.md` §8
- [ ] 带宽压力测试（要等加速器：CPU 写入只有 ~17MB/s，压不出带宽竞争）
- [ ] 渲染功能（BitBlt / Solid Fill / Alpha / Sprite —— 赛题要求）
- [ ] 按归档约定，把本次验证过的模块搬进 `RTL/` 和 `code/`（测通一个放一个）
- [ ] **素材通路（Flash → DDR → 显示）**：演示要一次烧 bitstream + hex + 素材三段。
      素材形态：人物/特效 = ARGB8888 真彩；地图/背景 = 索引色（8bpp），调色板放 BRAM 查表。
      位置已定：Flash 素材区 `0x3C0000` 起 4.25 MB；DDR 素材区 `0x01A0_0000` 起（唯一可变区）；
      表头格式与加载流程见 `docs/spec/地址空间规划.md` §6.4~§6.6。
      - **检查点 A（不需要屏幕）**：打包脚本吐出表头 + 一张确定的测试图 → 烧进 Flash `0x3C0000`
        → app 读表头、`spiFlash_f2m` 搬到 DDR、刷 D-cache → CPU 回读几个像素逐字节比对，串口打印
      - **检查点 B**：CPU 把 DDR 里那张素材按 RGB888 贴进帧缓存 → 屏上出图（先不做 alpha）
      - 要写的东西：PC 打包脚本（约 60 行 Python）、素材加载器（约 100 行 C）、贴图（约 30 行 C）
      - 待确认：Efinity Programmer 收 `.bin` 还是 `.hex`；素材搬运用单线（~2MB/s）还是四线（~4×）
- [ ] 真机 Demo（V1 基础 → V2 高阶 → V3 游戏）

> **checker 说明**：DDR 自检 checker 曾用来验证 AXI 通路，**已从系统摘除**
> （省约 2K LUT）。它腾出来的位置已经改派：**APB 槽 1 与 AXI S1 都归 lineBuffer**，
> 加速器统一约定用 **APB 槽 2 + AXI S2** —— 同一个外设在两条总线上编号一致。
> 源文件保留在 `project/05_SoC_AXI/RTL/Peripherial/BRAM/` 与 `.../APB0/BRAM/`，
> 需要时把顶层例化、Mux 接线和 APB 译码接回即可（并另选一个空槽）。
