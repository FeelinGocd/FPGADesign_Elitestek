# alpha 混合 —— 实现与验收结果单

> 日期：2026-10-06　规范：`docs/spec/硬件_加速器.md` §5.3
> 范围：**只做 PRAM 精灵路径**的 α 混合；整屏 alpha 禁止；DDR 帧源的 alpha 不做。
> 状态：**RTL + 素材脚本 + C API 全部完成，仿真回归全过，上板验证通过（2026-10-06，demo_glass）。**
> **后续（2026-10-11）**：在 α 后面又加了 **图片合成** 与 **颜色合成（色调 / 闪红，12 个 DSP）**，
> 见 `docs/spec/硬件_加速器.md` §5.5 / §5.6 与 `docs/spec/实测数据.md` §5.7；本文件只记录 α 那一次验收，不再更新。

## 1. 一句话结论

精灵的 α 存在**调色板每一项的高 8 位**（一项 = `AARRGGBB`），命令**零改动**，
只把 `MODE` 从 `1` 改成 `2`；硬件把**这条命令要写的那块矩形读回来当背景**，
在合成器里逐像素混合：`out = 背景 + (精灵 − 背景) × α / 255`（12 个 DSP）。

## 2. 改了哪些文件（为什么）

| 文件 | 改动 | 为什么 |
|---|---|---|
| `process/alphaBlending.v` | 新增（98 行，纯组合） | 混合器本体；一拍 4 像素 × 3 通道，`/255` 用精确整数式 |
| `pixel/palette/PaletteRAM.v`·`PaletteLUT.v`·`PaletteDecoder.v` | 调色板一项 24→32 位；拆出 `palette_alpha` 旁路 | α 只能放调色板（BRAM 只剩 24 块，独立 α 平面放不下）；低 24 位照旧 ⇒ 像素流通路一个字节不改 |
| `pixel/palette/PaletteSource.v` | 新出口 `palette_alpha` | 把 α 顺着同一条流水送到合成器 |
| `factory/Factory.v` | 新口 `src1_alpha` + 透传 | α **不并进 96 位像素流**，否则下游位宽全要动 |
| `factory/Compositor.v` | 例化混合器；**模式自己也解**（`src_en_of(flags)`），两路同时用 ⇒ 混合；掩码与 ck 相与、两路 valid 相与、`buf_free` 要两路都 free | 合成器是选路级，逐像素的事只能在这做 |
| `cmd/CmdExec.v` | **不再解"要哪几路源"**：删掉 `src_en_of` / `cmd_src_en_r` / `line_src_en`；只广播 `line_flags` | 谁要不要动是各源自己的事（工程里本来就写着"谁去读 DDR 是源自己的事"）；省掉跨模块的"解码结果"线 |
| `cmd/CmdChannel.v` | 删掉 `line_src_en` / `line_bg_rd` 两个透传口 | 行任务广播里不需要它们了 |
| `factory/Factory.v` | 删掉 `line_src_en` 口 | 同理（它只是转给合成器） |
| `pixel/DDR/DDRSource.v` | 新输入 `line_flags`；内部 `my_line` 判"要不要我"、`bg_rd` 判"读源行还是读目的行" | 背景 = 读回目的自己；`row_addr = bg_rd ? 目的行 : 源行`，相位/读长/拆笔一个字不改 |
| `pixel/palette/PaletteSource.v` | 新内部 `my_line`（本来就是 PRAM 源那一半） | 同上 |
| `factory/DDRPacker.v`·`Factory.v` | 行首 `cnt_byte <= dst_off`（**拍对齐下沉**，前置改动） | 源侧只做“像素对齐” ⇒ 合成器拿到的永远是 4 个整像素，α 才配得上 |
| `tools/img2hex/pic2pram.py` | `palette.hex` 输出 `AARRGGBB`；α = 该索引下所有像素的最大 α；索引 0 强制 0；半透明像素保留原色 | 素材侧唯一要跟着改的地方；索引数据 `assets.hex` **逐字节没变** |
| `CInstruction/CI_gpu2d.c`·`.h` | 新增 `CI_gpu2d_spriteAlpha` / `spriteAlphaRect` | 原来只能发 `MODE=1`，不补 API 上板没法试 |

### ★ 和初稿不同的地方（原因）

初稿写“`CmdExec`：`MODE_ALPHA` ⇒ `line_src_addr = 目的行地址`”。实现时发现**做不到**：
`line_src_addr` 是**广播**字段，源②(调色板精灵) 拿它当**精灵在 PRAM 里的字基址** ——
改成目的地址会连精灵地址一起改坏（PRAM 会去读 `目的地址 & 0x7FFF` 那个字）。

最终做法比"再加一根线"更彻底：**模式解码下沉到各源**——
`CmdExec` 只广播 `flags`，`DDRSource` / `PaletteSource` / `Compositor` 各自看 `flags[2:0]` 判；
`line_src_en`(2 位) 和临时出现过的 `line_bg_rd`(1 位) 都删掉了。详见 `硬件_加速器.md` §1.3。

## 3. 回归结果（10 个 TB）

| TB | 验什么 | 结果 |
|---|---|---|
| `tb_alphaBlending` | 混合器对拍软件整数模型 252 组 × 12 字节 | ALL-PASS |
| `tb_alphaCompositor`（新） | 合成器级 8 组：α=0/255/逐像素、ck 相与、valid 相与、取数节奏、`buf_free`、00/01/10 老行为回归 | ALL-PASS |
| `tb_cmd_ddrsource` | 命令 + DDR 源整链；**新增第二条 alpha 命令，直接查 AXI 读地址 = 目的行** | ALL-PASS（AR = `08800100`） |
| `tb_CmdExec` | 地址/模式/镜像；**新增 alpha 多行（模式=2、目的逐行 +5760、精灵地址不受影响）+ 反向用例** | ALL-PASS（28 个行任务） |
| `tb_m3palette` | 真 GPU_2D 整链逐字节（PRAM 精灵源） | ALL-PASS |
| `Factory_ck_tb` | 合成器 + 打包 + ck 掩码；**新增 `chk_fill_abs`：`dst_off` 0~15 全扫、逐字节比常数色的绝对颜色** | ALL PASS（**29 用例**）|
| `tb_PaletteDecStream` | 调色板解码流水 | ALL-PASS |
| `tb_DDRUnpacker` | 丢头/补头 | err_cnt = 0 |
| `tb_CmdChannel` | CI → 命令 → 行任务整链（顺手修好了它自己过期的 `line_color` 端口名） | ALL-PASS |
| `tb_m2loopback` | 端到端回路：加速器写进 DDR → CPU 读回来逐字节比（含 `dst_off=8` 的 FILL 段 + BLIT 段） | **从长期 FAIL 变 PASS** ✅ |

## 3.1 ★ 上板发现的真 bug（已修）：常数色被多转了一次

**症状（上板实测）**：左边方块颜色随位置在变；擦除后黑色背景上留**彩色残影**。

**根因**：两处都在补偿“目的地址在 16 字节拍内的偏移”，撞成了双份：

| 谁 | 干了什么 |
|---|---|
| `DDRPacker` | 行首 `cnt_byte <= dst_off`（拍对齐下沉，2026-10 加的）→ 流的第 0 字节落在目的块的第 0 字节 |
| `Compositor`（旧） | 又按 `dst_off mod 3` 把常数色**轮转**了一下（老约定留下的）→ 转多了 |

结果：`dst_off mod 3 == 1` 时块内颜色变成 R,B,G（本该 B,G,R）⇒ 方块颜色错；
擦背景那次 FILL 也错 ⇒ 写回去的不是背景色 ⇒ 彩色残影。

**为什么以前没抓到**：老的检查只比“跑两遍是否一致 + 掩码对不对”，**从没比过绝对颜色**；
而且只在 `dst_off mod 3 != 0` 时错（`dst_off=12` 那种正好对）。

**修法**：`Compositor` 不看目的地址相位，`fill_data = {4{color_r}}` 原样铺；
`phase_r` / `mod3()` 删除，`line_dst_addr` 端口和 `Factory` 里那根线一起去掉。

**新防线**：`Factory_ck_tb.chk_fill_abs`（16 档偏移全扫）＋ `tb_m2loopback` 的逐字节回路检查
（它本来就是为这个相位问题写的，现在终于过了）。

## 3.2 ★ 上板卡死（已修）：两路源的取数门控

**症状**：demo 一开就卡住、屏幕黑（显示被 `exclusive_begin` 关了，之后再也回不来），串口停在
"先关显示, 画完再开"那一行。

**复现**：`tb_m2loopback` 手发一条 `MODE=2` 命令（不涉及 C、不涉及 CI 驱动）⇒
`line_start 1 / line_done 0` —— 第一行就出不去，和板上一样。

**根因**（探针实测）：

| 探针 | 值 | 含义 |
|---|---|---|
| 源①valid / 源②valid | 10 拍 / 8 拍 | 两路源都出了数据 |
| packer out_valid | 0（alpha 那一行）| 合成器一拍配对数据都没送到打包器 |
| 两路 `src_free` | 几乎恒 1 | 数据被"取走了"，但**没人要** |
| 写侧 `wr_free` | 恒 0 | 一个字节都没拿到，永远等 |

`Compositor` 的取数门控原本是 `src_rd_en = src_en & pixel_ready`（只看"下游想不想收"），
而打包器真正收下的条件是 `pixel_valid & pixel_ready`。**PRAM 源 2 拍就到，DDR 背景还在等 AXI
读回来** ⇒ 中间那几拍被逐个【取走并丢弃】⇒ 打包器什么都没收到 ⇒ 写侧永远等 ⇒
`line_buffer_free` 不拉高 ⇒ `CmdExec` 发不出下一行 ⇒ 整条链卡死。
（单路源时 `pixel_ready` 恰好等价于"确实收下"，所以两条老路一直没暴露。）

**修法**（§4.1 的规则）：

```verilog
assign src0_rd_en = src_en_r[0] & ((pixel_ready & pixel_valid) | row_done_r);
assign src1_rd_en = src_en_r[1] & ((pixel_ready & pixel_valid) | row_done_r);
//   row_done_r：line_done（写侧完成）之后，两路各自把写侧不要的尾巴丢掉，好让 src_free 拉高
```

**验证**：`tb_m2loopback` 第三段 修前 `1/0` → 修后 **`16/16`**，BLIT 段/FILL 段无回归；
`tb_alphaCompositor` 新增第 9 例；全套 10 个 TB 全过。

**注意**：加了玻璃之后调色板会重新训练（共用调色板），`sim/tb/*/palette.hex` 这些**运行目录副本要跟着更新**，
否则 TB 的 DUT 和参考会读到两份不同的调色板，报一堆假错（`tb_m3palette` 现在读同目录的副本）。
## 4. 上板前要注意的三件事

1. **必须重新综合**：`palette.hex` 和素材都是 BRAM 初值（生成 bit 阶段烧进去的），
   调色板从 24 位变 32 位、内容也变了。
2. **固件要重编**：新增了 `CI_gpu2d_spriteAlpha()`；`assets_desc.h` 也重新生成过（内容未变）。
3. **素材要按 α 档位画**：一个索引 = 一种 (颜色, α)，用固定档位（0/128/255），
   别用连续渐变的透明度，否则 256 项装不下。生成完 `tools/img2hex/output/report.txt` 里会打出 α 档位统计。

## 5. 怎么发一条 alpha 命令

```c
CI_gpu2d_spriteAlphaRect(asset_word_base, stride_word, fb_back,
                         x, y, w, h, 0 /* extra: CKEY_EN | FLIP_X | FLIP_Y */);
```

命令形状和普通精灵一模一样，只多了“把目的那块读回来当背景”这一步。
**别拿它刷整屏**：读回量 = 矩形那一段，整屏读回会把 DDR 读带宽吃满。
