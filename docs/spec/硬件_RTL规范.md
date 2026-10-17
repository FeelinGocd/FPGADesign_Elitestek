# 硬件 RTL 规范（命名 / 结构 / 连线）

> **用途**：本工程 RTL 的**写法约定**。改 RTL 之前先看这一份；评审 diff 时也按它挑毛病。
> **基准**：`lineBuffer` 那一支（`lineBuffer.v` / `bufferControler.v` / `bufferUnpacker.v` / `PingPongBuffer/*`）本来就是照这套写的，新代码以它为样板。
> 建立：2026-10-11（GPU_2D 子树按本规范过了一遍之后）。
> v1.1（2026-10-11 晚）：§4 纪律补 5 条（都是这两天踩到的）—— **TB 过 ≠ 能综合**（要跑 `--flow map`）、
> **别用字符串脚本批量改 RTL**、`case` 不许重复项、**乘法器要 mux 共用**、按趟选不能用旧 `pass_r`。

---

## 1. 命名

### 1.1 `line_` = 行任务那一族

**含义**：行任务这条路上"特意引出来"的一族线；前缀之后只写**功能缩写**，不再写模块名。

```
line_start  line_w  line_dst_addr  line_src_addr  line_flags  line_param  line_fx_en
line_image_next      （源② 那条流的"图一到此为止"）
line_colorKey_en     （命令级 ck 开关）
line_solid_en / line_solid_data / line_solid_alpha / line_tint / line_fx_over
                     （颜色合成：上层是纯色 / 纯色值 / 浓度 / 色调模式 / 上下方向）
line_src0_free  line_src1_free  line_imgRow_free  line_buf_free  line_wr_free  line_buffer_free
line_done
line_src_addr_last / line_flags_last / line_param_last   （"看最后一格"的三根）
```

### 1.2 模块 IO 体现"这条线是哪一家的"

不再用 `in_*` / `out_*` / `src_*` 这类**无名前缀**；名字要么写**对端**，要么写**本模块**：

| 模块 | 入口 | 出口 |
|---|---|---|
| `PaletteSource` | `line_*` / `key_color` | `palette_data/mask/alpha/valid/ready`、`line_src1_free` |
| `imageCombine` | `source_data/mask/alpha/valid`、`source_ready` | `imgCombine_data/mask/alpha/valid`、`imgCombine_ready`、`line_imgRow_free` |
| `Compositor` | `src0_*`（源①）、`src1_*`（源②） | `pixel_data/mask/valid`（→ DDRPacker） |
| `DDRPacker` | `pixel_data/mask/valid/ready` | `packer_data/mask/valid/ready` |
| `WriteController` | `packer_data/valid/mask` | `cmd_wr_*`、`wr_data/strb/valid`、`line_done` |
| `Factory` | `src0_*`/`src1_*` 两路 + 行任务 | `cmd_wr_*`/`wr_*`、`line_done`、`line_buffer_free` |

### 1.3 free / 握手一族统一 `line_*_free`

- 两个源**各一份**（避免顶层重名）：`line_src0_free`（源①）、`line_src1_free`（源②）；
- 合成级缓冲：`line_imgRow_free`；合成器汇总：`line_buf_free`；写侧：`line_wr_free`；
- 回给命令通路（CmdChannel 的端口名）：`line_buffer_free`。
- 链路上是"下游表态 → 上游汇总"：`line_buf_free = 各源 free & line_imgRow_free`，再和写侧 `line_wr_free` 相与成 `line_buffer_free`。

### 1.4 时序/组合

- 寄存器一律 `_r` 后缀（`flags_r / src_en_r / color_r / row_done_r / wr_ptr_r`）；组合量不加；
- 函数名 `xxx_of(...)`（`src_en_of` / `src_stride_of` / `needed_bytes_of`）；genvar 用 `gi / gp / gc`。

### 1.5 全工程固定约定

| 项 | 约定 |
|---|---|
| 像素流字节序 | **低位字节 = B**（一拍 12 字节 = 4 个像素，B,G,R 循环） |
| 调色板一项 | 32 位 `AARRGGBB`（α 在最高字节） |
| α | **走旁路**（`*_alpha`，4×8 位），**不并进像素流** |
| 地址单位 | 行任务里的源地址 = **PRAM 字地址**；`param[11:0]` 的跨距按**字** |
| 拍 | AXI 一拍 16 字节；像素流一拍 12 字节（4 像素） |

---

## 2. 文件结构

### 2.1 文件头固定六段

```
//  <模块>.v —— 一句话定位
//  干什么            （职责；不做什么也写）
//  上一级是谁        （谁例化它；顶层写"RTL/GamingPlayUnit_Top.v"）
//  接口契约          （握手/时序上必须守的几条）
//  为什么这么做      （设计取舍、踩过的坑 ★）
//  参考：docs/spec/xxx.md §n
```

### 2.2 模块内分区顺序（不许乱）

```
1) module 端口（按组，注释带 (I)/(O) + 对端）
2) localparam / 参数换算（先把算式变成名字，正文只用名字）
3) wire / reg        —— 按"和谁之间"分组： // ---- A <-> B ----
4) 组合条件（assign）
5) 时序（always）
6) 例化               —— 放最后
```

### 2.3 端口注释：按组标对端

一组端口标一次就够（本工程定的是**按组**，不是逐根）：

```verilog
//==================== 行任务口 → 各源 / 合成器 / 写侧 ====================
input  wire line_done,          //(I) 这一行已经写进 DDR   ← Factory
//==================== AXI4 主口 → Axi_Mux S3（写）====================
```

规则：**出 `→`、入 `←`**，后面写对端的模块名（必要时带端口名）。

---

## 3. 顶层只连线

`GPU_2D.v` / `Factory.v` / `CmdChannel.v` / `lineBuffer.v` 这一级**不摆逻辑**，只有三样东西：

```verilog
// 1) 子模块各一组 wire（名字用子模块自己的端口名）
wire [95:0] palette_data;      // ← 素材源那一侧
wire [95:0] src1_data;         // ← 加工面那一侧

// 2) assign 段：一行一条连线，谁连谁只在这里看
assign src1_data = palette_data;

// 3) 例化放最后，每个子模块只连自己那组线
```

### 3.1 `wire` 与 `assign` 必须分开（★ 为什么要这样）

- `wire` **只声明"这根线归谁"**（名字用对端端口名）⇒ 不用翻例化就知道它属于哪个模块；
- `assign` **只回答"从谁连到谁"** ⇒ 一次 grep 就能把一个模块的全部连线捞出来；
- 改了某侧端口，只动它那一根线 + 一行 assign，不会牵动别人。

✗ 不许：`wire x = expr;`（把"归属"和"来源"糊在一行）
✗ 不许：一根线直接接两个模块、连接关系藏在端口列表里
★ **`assign` 左边永远是被驱动的那一侧**（本条踩过：方向写反 = 双驱动，仿真当场 FAIL）

### 3.2 两侧同名就一根线，不同名就"两侧各一根 + assign"

| 情况 | 写法 |
|---|---|
| 两侧端口同名（`line_start` ↔ `line_start`） | 一根 wire 直连 |
| 两侧端口不同名（`palette_data` ↔ `src1_data`） | 两侧各一根 + `assign src1_data = palette_data;` |
| 汇总/派生（`line_buf_free` ↔ `line_buffer_free`） | 各一根 + `assign`，别让一根线带两种语义 |

---

## 4. 改 RTL 的纪律（都是踩过的）

1. **模式表三处一起改**：`DDRSource`（要 bit0）、`PaletteSource`（要 bit1）、`Compositor`（要两位）各有一份 `flags` 解码 —— 权威表在 `硬件_加速器.md` §1.3。
2. **广播侧必须看"锁存后的上下文"**，不能看 FWFT 出来的端口值（命令队列预读，端口那拍已经换成下一条了）。
3. **按趟选的信号要组合选，而且判据不能只看 `pass_r`**：`pass_r` 是**收下行任务那一拍**才更新的，
   而 `line_start` 那一拍 Decoder 就把它锁进 `mx_r` 了 —— 那一拍 `pass_r` 还是**上一条命令**留下的值，
   ⇒ 第一行选到错的格（症状：MX 命令第一行不做字内反序，顶行 4 个像素错位）。
   正确写法：`in_pass2 = pass_r & (state != S_IDLE); dec_flags = in_pass2 ? 第2格 : 第1格;`（`PaletteSource`）。
4. **`out_valid` 绝不看 `out_ready`**：下游的取数条件里含 `valid` ⇒ 组合环，整条链一个拍都不动（上板表现：黑屏 + 串口停住）。
5. **一行只算一次的东西不要两处各算一遍**（拍数由写侧算；行边界由源自己数；相位整形只在 `DDRPacker`）。
6. **改完必须跑**：该模块的单元 TB + `tb_m2loopback`（整链 + 逐像素金标准）；改接口还要跑 `Factory_ck_tb`。
7. **★★ TB 过 ≠ 能综合**：ModelSim 很松 —— 重复的端口、断掉的续行、重复的语句、重复的 `case` 项
   **它一个都不报**，Verific 会当场报错。所以**综合前必须跑一次 `efx_run.bat <工程>.xml --flow map`**：
   那才是真正的语法/可综合门禁（本工程曾连踩 4 次：重复端口 / 断行 / 重复语句 / 端口连两次）。
8. **★★ 不要用字符串脚本批量改 RTL**：脚本插行会插出重复端口、断掉的续行、重复的赋值语句 ——
   一律先 `read` 看清上下文再定点 `edit`；改完用重复端口/重复连接的自检脚本扫一遍。
9. **`case` 里不许出现重复的 case 项**：仿真取第一个命中，综合工具不保证 ⇒ "仿真过、板上不对"。
10. **一拍 N 个字节 × M 种模式的运算，乘法器要 mux 着共用**：一个式子一个乘法器就是 `N×M` 个
    （颜色合成四档式子一开始各写一个 ⇒ 12 字节 ×4 = **48 个 DSP**，整机 68/160）。
    把"被乘数 / 乘数"按模式 mux 好再乘一次 ⇒ **12 个**（整机 DSP 68 → 32；现在含旋转那一版是 **34**）。
    ★ 顺带一条 Verilog 坑：**移位不把上下文宽度传给左操作数** —— `(a*b)>>8` 会先把乘积截成 8 位，
      必须先落到 `wire [15:0]` 再取高位。
11. **文档同步**：接口位定义只写 `硬件_加速器.md` §1（唯一定稿）；C 侧用法写 `软件_驱动与API.md`；数字进 `实测数据.md`（带日期）；过时内容不留正文。

---

## 5. 现状（谁已经过完一遍）

> 文件清单与"哪些是自研 / 哪些是拿来的"见 §6 —— 改之前先确认自己动的是哪一类。

| 范围 | 状态 |
|---|---|
| `GPU_2D.v` + `cmd/*` + `pixel/*` + `process/*` + `factory/*` + `interface/*` | ✅ 命名/文件头/端口组标注/顶层连线已按本规范过完（19 个模块） |
| `lineBuffer` 那一支 | ✅ 本来就是基准，未改动 |
| 端口注释 | 🚧 少数"时钟/复位""参数"组不需要对端，未标；个别括号描述待并进箭头写法 |
| TB | ✅ 2026-10-17：**18 项里 17 项 PASS**（含端到端 `tb_m2loopback` 的 **11 段 / 936 行任务**、err=0）；只剩 `tb_lineBuffer` 的期望模型还停在 1080p（`实测数据.md` §6）。`Combine/tb_combine` 是废件，已从 `run_regress.ps1` 移除 |
| 综合 | ✅ `--flow map` PASS（只剩一条 SoC 自带的 `rsp_error has no driver` 老警告） |

---

## 6. 文件清单与分类（自研 / 拿来的）

改之前先确认动的是哪一类：**自研模块按本规范写**；**拿来的模块保持原样**（只做参数配置）。

### 6.1 自研（本规范适用）

| 路径 | 模块 | 干什么 |
|---|---|---|
| `GamingPlayUnit_Top.v` | `GamingPlayUnit_Top` | 顶层：只连线 |
| `Peripherial/GPU_2D/GPU_2D.v` | `GPU_2D` | 加速器顶层：四个域拼装 + 行模式锁存 |
| `Peripherial/GPU_2D/cmd/*` | `CIUnpack` / `CmdBuffer` / `CmdChannel` / `CmdExec` | 命令域：CI 收下 → 拼命令 → FIFO → 逐行推进 |
| `Peripherial/GPU_2D/pixel/DDR/*` | `DDRSource` / `DDRUnpacker` | 源①：DDR 读 + 拆像素 |
| `Peripherial/GPU_2D/pixel/palette/*` | `PaletteRAM` / `PaletteLUT` / `PaletteDecoder` / `PaletteSource` | 源②：片上素材 + 调色板 + 镜像 / 旋转 |
| `Peripherial/GPU_2D/process/*` | `colorKey` / `alphaBlending` / `imageCombine` | 像素运算：ck / α / 合成 + 色调 |
| `Peripherial/GPU_2D/factory/*` | `Factory` / `Compositor` / `DDRPacker` / `WriteController` | 选路 → 打包 → 写 |
| `Peripherial/GPU_2D/interface/*` | `AXIMastersHandler` / `GPU2D_APBHandler` | AXI 主口 + APB 寄存器 |
| `Peripherial/GPU_2D/lineBuffer/*` | `lineBuffer` / `bufferControler` / `bufferUnpacker` | 显示通道（**本规范的基准样板**） |
| `Peripherial/GPU_2D/lineBuffer/PingPongBuffer/*` | `async_PingPongBuffer` / `async_fifo` / `BlockRAM` | 跨域双 bank |
| `Peripherial/APB0/*` | `APB0_Decoder` / `APB0_Interconnect` / `APB0_GPU_2D` / `APB0_key` | APB 子系统 |
| `Peripherial/AXI4/Master/*` | `AXI4_Master` / `AXI4_Master_Read` / `AXI4_Master_Write` | AXI4 主口 |

### 6.2 拿来的（保持原样）

| 路径 | 来源 | 备注 |
|---|---|---|
| `Peripherial/GPU_2D/lineBuffer/HDMI/*` | 官方 HDMI 例程 | 只在 `pixel_timing.v` 用 `define` 选分辨率；`TMDS/*` 一个字节没动（**像素通路靠综合，不许碰**） |
| `Peripherial/AXI4/Axi_Mux.v` | 官方多主汇聚 IP | ★ **登记为规范例外**：全文是 `wire x = y;` 一行一根线的写法，按本规范重排等于重写，风险大于收益 |
| `ddr3_controller/**` | 官方 DDR3 控制器 | 只做参数配置，不进去改 |
| `template/APBX_InterfaceTemp.v` | 官方 APB 从设备模板 | **不参与综合**，当参考样例留着 |

> ★ 判据：**综合报告里出现、并且被 `GamingPlayUnit.xml` 列进设计文件**的才守本规范；
> 拿来的只守两条 —— ① 不改内部；② 只在顶层做参数配置。

---

## 7. 版本

| 版本 | 日期 | 变更 |
|---|---|---|
| v1.2 | 2026-10-17 | 新增 §6 文件清单与分类（自研 / 拿来的 / 规范例外 `Axi_Mux`）；§5 状态更新到 17/18 TB 全绿 |
| v1.1 | 2026-10-11 | §4 纪律补 5 条（TB 过 ≠ 能综合、别用脚本批量改 RTL、`case` 不许重复项、乘法器要 mux 共用、按趟选不能用旧 `pass_r`）|
| v1.0 | 2026-10-11 | 建立：命名 / 文件结构 / 顶层只连线 / 改 RTL 的纪律 |
