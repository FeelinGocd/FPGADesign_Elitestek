# HDMI 接口记录

> 状态：草稿（内容全部取自当前工程，与 RTL / peri 一致）
> 依据文件：
>   - RTL：`RTL/hdmi_driver/`（HDMI_onBoard_Top / HDMI_Top / Pixel_Timing/pixel_timing.v / TMDS/dvi_encoder_Top.v / TMDS/tmds_encoder.v / asyn_rst_syn.v）
>   - 配置：`HDMI/HDMI.peri.xml`（Interface Designer）＋ `HDMI/outflow/HDMI.pt.sdc`（自动生成）
> 归档位置：待定（先放本目录，候选 docs/spec）

---

## 1. 接口接哪些（peri ↔ RTL 顶层 HDMI_onBoard_Top ↔ 引脚）

| RTL 顶层端口 | 方向 | peri 定义 | 引脚 / 资源 | 说明 |
|---|---|---|---|---|
| `clk_HDMI_x1` | in | PLL_BR0 输出 `clk_HDMI_x1`（÷20） | —（GCLK 网） | 像素时钟 **148.75MHz** |
| `pll_HDMI_locked` | in | PLL `locked_name=pll_HDMI_locked` | PLL_BR0 | 锁定指示；测试顶层临时用它做复位源 |
| `clk_25M` | in | GPIO `clk_25M`（input, gclk, 1.8V） | **GPIOL_P_08** | 板载 25MHz，PLL 参考（core 模式） |
| `tmds_blue_o[9:0]` | out | LVDS `tmds_data0` → `tmds_blue_o` | **GPIOR_PN_13** | 通道0=蓝（已取反） |
| `tmds_green_o[9:0]` | out | LVDS `tmds_data1` → `tmds_green_o` | **GPIOR_PN_11** | 通道1=绿（已取反） |
| `tmds_red_o[9:0]` | out | LVDS `tmds_data2` → `tmds_red_o` | **GPIOR_PN_12** | 通道2=红（已取反） |
| `tmds_clk_o[9:0]` | out | LVDS `tmds_clk` → `tmds_clk_o` | **GPIOR_PN_10** | 时钟通道（已取反） |
| `tmds_data0/1/2_TX_OE`、`tmds_clk_TX_OE` | out | LVDS `oe_name` | — | =1（输出使能） |
| `tmds_data0/1/2_TX_RST`、`tmds_clk_TX_RST` | out | LVDS `reset_name` | — | =0（输出级不复位） |

> 注意：
> - TMDS 引脚顺序**不是** PN_10/11/12/13 连续——data0=PN_13、data1=PN_11、data2=PN_12、clk=PN_10，以 peri/板级为准；
> - RTL 顶层输出的是**取反后**的 10bit（Efinix 硬核极性约定），peri 的 out_bname 与 RTL 端口同名；
> - 通道映射：B→data0（hs/vs 控制码在 data0）、G→data1、R→data2；时钟通道 serialize 后 = 像素率方波。

---

## 2. PLL 设置细则（当前工程 pll_HDMI = PLL_BR0）

| 项 | 值 | 说明 |
|---|---|---|
| PLL 资源 | **PLL_BR0** | TMDS TX 全在右侧 → 右下角 PLL（同象限就近供快钟） |
| 参考时钟 | `clk_25M`，25MHz，GPIO GPIOL_P_08，conn_type=**gclk**，`ref_clock_mode="core"` | 板上晶振走全局钟网进 PLL（非专用 PLLIN 脚） |
| VCO | 25 × 119 = **2975 MHz** | Feedback_25M ÷119（local 反馈） |
| `clk_HDMI_x1` | ÷20 → **148.75 MHz**（phase 0） | 像素/慢钟，进 fabric + LVDS slow clock |
| `clk_HDMI_x5` | ÷4 → **743.75 MHz**（phase_setting=2 = **90°**） | LVDS fast clock（half-rate DDR 用），不进 fabric |
| 相位步进 | 0.5 × VCO 周期 ≈ 0.168ns | phase=2 → 0.336ns = 743.75MHz 的 90° |
| locked | `pll_HDMI_locked` | 测试顶层复位源 |
| SDC | `clk_HDMI_x1` 6.723ns；`clk_HDMI_x5` 1.345ns（waveform {0.336 1.009}） | Interface Designer 自动生成 |

**为什么 fast 要移 90°**：10:1 半速率串行器里，快钟边沿要避开慢钟数据加载时刻并落在串行 bit 单元中间（数据眼居中），保证并转串交接的建立/保持裕量。

**换分辨率时三处联动**：① `pixel_timing.v` 分辨率宏；② PLL 分频（Interface Designer，保持 fast=5×slow 与 90°）；③ SDC 由工具重新生成。当前 1080p：148.75MHz（≈60.1Hz）——25MHz 整数分频无法得到精确 148.5。

---

## 3. LVDS TX 设置记录（Block Editor → Transmitter(TX)，4 路）

来源：Interface Designer 里 LVDS 块的 Transmitter(TX) 页（截图为例：tmds_clk 实例）；除 **LVDS Resource** 与各名字不同外，4 路其余设置一致。

### 3.1 每 lane 的 LVDS Resource 与信号名

| Lane（实例名） | LVDS Resource | Output Pin/Bus | Output Enable Pin | Reset Pin | 备注 |
|---|---|---|---|---|---|
| `tmds_data0` | **GPIOR_PN_13** | `tmds_blue_o` | `tmds_data0_TX_OE` | `tmds_data0_TX_RST` | 蓝（B） |
| `tmds_data1` | **GPIOR_PN_11** | `tmds_green_o` | `tmds_data1_TX_OE` | `tmds_data1_TX_RST` | 绿（G） |
| `tmds_data2` | **GPIOR_PN_12** | `tmds_red_o` | `tmds_data2_TX_OE` | `tmds_data2_TX_RST` | 红（R） |
| `tmds_clk` | **GPIOR_PN_10** | `tmds_clk_o` | `tmds_clk_TX_OE` | `tmds_clk_TX_RST` | 时钟通道 |

### 3.2 公共 TX 设置（4 路相同）

| 设置项 | 值 |
|---|---|
| Output Differential Type | `lvds` |
| Output Differential, VOD | `large` |
| Output Pre-Emphasis | `medium low` |
| Mode | `serial data output` |
| Enable Serialization | ☑ |
| Enable Half Rate Serialization | ☑ |
| Serialization Width | `10` |
| Serial Clock — Pin Name / Clock Instance | `clk_HDMI_x5` / `pll_HDMI` |
| Parallel Clock — Pin Name / Clock Instance | `clk_HDMI_x1` / `pll_HDMI` |
| Reset Pin Name | 见 3.1（`tmds_*_TX_RST`） |
| Advanced — Static Mode Delay Setting | `0` |

---

## 4. 其他要点

1. 复位：测试顶层临时本地 `asyn_rst_syn(pll_HDMI_locked)` 同步；正式接入 SoC 后由 AXI 外设统一提供。
2. 端口名 = peri 生成的顶层 net 名；改 Interface Designer 里的名字要同步改 RTL 顶层。
3. `HDMI.pt.sdc` 自动生成勿手改（output_delay 的 reference_pin 与硬核位置绑定）。
4. 测试彩条宽度 = H_ACTIVE/8（当前 240 列/段），改分辨率时同步。
5. 极性：RTL 把 4×10bit **取反**后给硬核，勿删（§1 已注）。
6. 本文件归档位置待定。
