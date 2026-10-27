# -*- coding: utf-8 -*-
"""
生成《系统整体框图》用的 5 张 PNG。

来源：RTL/ 下当前代码 + project/GamingPlayUnit/GamingPlayUnit.xml 的编译文件表。
只画"已经存在的模块"和"已经接上的线"，不画计划中的东西。

用法：python gen_block_diagrams.py
输出：同目录 图1_系统顶层.png … 图5_APB0子系统.png
"""

import os
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch, Rectangle

plt.rcParams["font.sans-serif"] = ["Microsoft YaHei"]
plt.rcParams["font.family"] = "sans-serif"
plt.rcParams["axes.unicode_minus"] = False

HERE = os.path.dirname(os.path.abspath(__file__))

# ----------------------------------------------------------------------------
# 配色：一类模块一个色系，读图时靠颜色分辨"它属于哪一层"
# ----------------------------------------------------------------------------
C = {
    "clk":  ("#EFEAF7", "#5B4E9E"),   # 时钟 / 第三方 IP
    "io":   ("#F2F2F2", "#6B6B6B"),   # 板级 / 引脚
    "soc":  ("#E8E6F5", "#5B4E9E"),   # SoC
    "bus":  ("#EDEDED", "#4D4D4D"),   # 总线
    "cmd":  ("#FDE8C8", "#C07A22"),   # 命令域
    "src":  ("#DCEBF7", "#2F6E96"),   # 像素源
    "proc": ("#E3F1DC", "#3F7D34"),   # 像素运算 / 加工面
    "mem":  ("#F7E4F1", "#8E4B7F"),   # 存储
    "disp": ("#FBE3E1", "#B0524A"),   # 显示通道
    "reg":  ("#FFF6CC", "#A08A1E"),   # 寄存器层
    "ext":  ("#FFFFFF", "#8A8A8A"),   # 本模块之外
}

LW_EDGE = 1.2
LW_ARROW = 1.4


# ----------------------------------------------------------------------------
# 绘图小工具
# ----------------------------------------------------------------------------
def canvas(w, h, dpi=150):
    fig = plt.figure(figsize=(w / 10.0, h / 10.0), dpi=dpi)
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set_xlim(0, w)
    ax.set_ylim(0, h)
    ax.set_aspect("equal")
    ax.axis("off")
    return fig, ax


def _block_height(n_title, n_body, tfs, bfs):
    pt = (1.65 * tfs if n_title else 0) + 1.55 * bfs * n_body
    return pt * 10.0 / 72.0


def box(ax, x, y, w, h, title, body=(), kind="bus", tfs=10.5, bfs=8.2,
        lw=LW_EDGE, ls="-", zorder=2, tc=None):
    fc, ec = C[kind]
    ax.add_patch(FancyBboxPatch(
        (x, y), w, h, boxstyle="round,pad=0,rounding_size=1.4",
        linewidth=lw, edgecolor=ec, facecolor=fc, linestyle=ls, zorder=zorder))
    total = _block_height(bool(title), len(body), tfs, bfs)
    cy = y + h / 2.0 + total / 2.0
    if title:
        th = 1.65 * tfs * 10.0 / 72.0
        ax.text(x + w / 2.0, cy - th / 2.0, title, ha="center", va="center",
                fontsize=tfs, color=tc or ec, fontweight="bold", zorder=zorder + 1)
        cy -= th
    for line in body:
        bh = 1.55 * bfs * 10.0 / 72.0
        ax.text(x + w / 2.0, cy - bh / 2.0, line, ha="center", va="center",
                fontsize=bfs, color="#222222", zorder=zorder + 1)
        cy -= bh


def group(ax, x, y, w, h, label, ec="#8C8C8C", fs=9.5, ls=(0, (5, 3)),
          fc="none", zorder=1):
    ax.add_patch(Rectangle((x, y), w, h, linewidth=1.1, edgecolor=ec,
                           facecolor=fc, linestyle=ls, zorder=zorder))
    ax.text(x + 1.4, y + h - 1.4, label, ha="left", va="top", fontsize=fs,
            color=ec, fontweight="bold", zorder=zorder + 6,
            bbox=dict(fc="white", ec="none", alpha=0.9, pad=0.6))


def arrow(ax, p0, p1, color="#333333", lw=LW_ARROW, ls="-", rad=0.0,
          mut=9, zorder=4, both=False):
    style = "<|-|>" if both else "-|>"
    ax.add_patch(FancyArrowPatch(
        p0, p1, arrowstyle=style, mutation_scale=mut, color=color,
        linewidth=lw, linestyle=ls, shrinkA=0, shrinkB=0, zorder=zorder,
        connectionstyle="arc3,rad=%s" % rad))


def poly(ax, pts, color="#333333", lw=LW_ARROW, ls="-", head=True, mut=9,
         both=False):
    for i in range(len(pts) - 2):
        ax.plot([pts[i][0], pts[i + 1][0]], [pts[i][1], pts[i + 1][1]],
                color=color, lw=lw, ls=ls, zorder=4, solid_capstyle="butt")
    if head:
        arrow(ax, pts[-2], pts[-1], color=color, lw=lw, ls=ls, mut=mut,
              both=both)
    else:
        ax.plot([pts[-2][0], pts[-1][0]], [pts[-2][1], pts[-1][1]],
                color=color, lw=lw, ls=ls, zorder=4)


def bus(ax, pts, color="#4D4D4D", lw=3.2, mut=11):
    ax.plot([p[0] for p in pts], [p[1] for p in pts], color=color, lw=lw,
            zorder=4, solid_capstyle="butt", solid_joinstyle="miter")
    arrow(ax, pts[-2], pts[-1], color=color, lw=lw, mut=mut)


def lbl(ax, x, y, text, fs=8.0, color="#333333", ha="center", va="center",
        rot=0, bg=True, zorder=7, weight="normal"):
    ax.text(x, y, text, ha=ha, va=va, fontsize=fs, color=color, rotation=rot,
            zorder=zorder, linespacing=1.3, fontweight=weight,
            bbox=dict(fc="white", ec="none", alpha=0.9, pad=0.7) if bg else None)


# ============================================================================
# 图 1 —— 系统顶层
# ============================================================================
def fig1():
    fig, ax = canvas(200, 112)
    ax.text(100, 109, "图 1  系统顶层框图（GamingPlayUnit_Top）",
            ha="center", va="center", fontsize=15, fontweight="bold", color="#222")

    box(ax, 2, 82, 34, 24, "时钟生成（3 个 PLL）", (
        "pll_inst1  → core_clk 100 MHz",
        "DDR3_PLL   → sdram_clk 400 MHz",
        "pll_HDMI   → clk_HDMI_x1 74.25 MHz",
        "           → clk_HDMI_x5 371.25 MHz",
    ), kind="clk", tfs=10)

    box(ax, 2, 54, 34, 24, "板级接口", (
        "key_pin[3:0]（按下 = 0）",
        "UART0 / SPI0 / GPIO0",
        "JTAG（硬核 TAP）",
    ), kind="io", tfs=10)

    box(ax, 48, 58, 56, 50, "SoC_RISCV32（Sapphire SoC · VexRiscv · IP）", (
        "① io_ddrA      ：AXI4 主口 128b，ID 全 0",
        "② io_apbSlave_0：APB3 从口（用户外设窗）",
        "③ cpu0_customInstruction：CI 8 根",
        "④ userInterruptA：PLIC 中断号 16",
        "⑤ SPI0 / UART0 / GPIO0 / JTAG",
        "⑥ 4 KB D-cache（写后须刷 / 无效）",
    ), kind="soc", tfs=10.5, bfs=8.2)

    box(ax, 116, 58, 34, 50, "Axi_Mux", (
        "4 从 1 主 · 5 通道独立仲裁",
        "S0 ← SoC io_ddrA",
        "S1 ← 显示取数",
        "S2 ← 加速器读",
        "S3 ← 加速器写",
        "M  → ddr3_top s_axi",
        "仲裁：轮询（Cfg_Arb_Type=0）",
    ), kind="bus", tfs=11)

    box(ax, 162, 62, 36, 42, "ddr3_top", (
        "Elitestek DDR3 控制器 IP（加密）",
        "AXI4 从口 128b / 28b 地址",
        "本地总线 + 存储控制器",
        "PHY：写校准 / 读定级 / MPR",
        "400 MHz DDR3",
    ), kind="clk", tfs=11)

    box(ax, 162, 38, 36, 18, "DDR3 颗粒（板载）", (
        "ck / addr / ba / cs",
        "dq / dqs / dm / odt",
    ), kind="io", tfs=10)

    box(ax, 48, 30, 56, 20, "APB0_Interconnect（APB 子系统）", (
        "APB0_Decoder：槽 0 → key，槽 2 → GPU_2D",
        "槽内 APB0_gpu2d：0x2000 观测 / 0x2040 显示",
    ), kind="bus", tfs=10)

    box(ax, 2, 28, 34, 14, "key", ("同步 + 消抖 + 按下沿", ), kind="cmd", tfs=10)

    box(ax, 48, 4, 56, 20, "GPU_2D（加速器 + 显示通道 · 功能层）", (
        "命令域 → 像素源 → 加工面 → AXI 主口",
        "显示通道 lineBuffer：取数 → 跨域 → 拆包 → TMDS",
        "寄存器 ←→ 功能：GPU2D_APBHandler（唯一翻译层）",
    ), kind="proc", tfs=10.5)

    box(ax, 112, 30, 34, 20, "复位门控（顶层生成）", (
        "sys_rst      = pll_locked",
        "                & user_pll_locked",
        "axi_rst_n    = sys_rst & cal_done",
        "rst_hdmi_n   = axi_rst_n",
        "                & pll_HDMI_locked",
    ), kind="reg", tfs=9.2, bfs=7.2)

    box(ax, 112, 4, 34, 18, "TMDS → HDMI 连接器", (
        "4 对 LVDS 硬核 TX",
        "clk / d0 / d1 / d2",
    ), kind="io", tfs=10)

    # ---- 连线 ----
    arrow(ax, (36, 100), (48, 100))
    lbl(ax, 42, 102, "user_clk 等", fs=7.6)

    poly(ax, [(36, 90), (40, 90), (40, 16), (48, 16)])
    lbl(ax, 39, 52, "clk_HDMI_x1 74.25 MHz", fs=7.6, rot=90)

    arrow(ax, (36, 68), (48, 68))
    lbl(ax, 42, 71, "按键 / 串口 /\nSPI / GPIO / JTAG", fs=6.4)

    arrow(ax, (104, 98), (116, 98))
    lbl(ax, 110, 100, "io_ddrA（S0）", fs=7.6)

    arrow(ax, (150, 98), (162, 98))
    lbl(ax, 156, 100, "M", fs=8.4)

    arrow(ax, (174, 62), (174, 58))
    arrow(ax, (186, 58), (186, 62))
    lbl(ax, 180, 60, "ddr_*", fs=7.6)

    arrow(ax, (76, 58), (76, 50))
    lbl(ax, 77, 55.4, "io_apbSlave_0（APB3）", fs=7.2, ha="left")

    arrow(ax, (48, 31), (36, 31))
    lbl(ax, 42, 33.6, "key_state / key_edge", fs=6.2)

    arrow(ax, (88, 30), (88, 24))
    arrow(ax, (82, 24), (82, 30))

    arrow(ax, (100, 50), (100, 58), ls=(0, (4, 2)), color="#B0524A")
    lbl(ax, 99, 52.4, "IRQ_frame", fs=7.0, ha="right", color="#B0524A")

    poly(ax, [(48, 64), (44, 64), (44, 8), (48, 8)])
    lbl(ax, 43, 44, "CI 8 根", fs=7.2, rot=90)

    bus(ax, [(104, 18), (110, 18), (110, 54), (130, 54), (130, 58)])
    lbl(ax, 119, 56.2, "3× AXI4 主口 → S1 显示 / S2 读 / S3 写", fs=7.4, bg=False)

    arrow(ax, (104, 12), (112, 12))
    lbl(ax, 108, 14, "tmds_*", fs=7.4)

    fig.savefig(os.path.join(HERE, "图1_系统顶层.png"), dpi=150, facecolor="white")
    plt.close(fig)


# ============================================================================
# 图 2 —— GPU_2D 内部
# ============================================================================
def fig2():
    fig, ax = canvas(252, 152)
    ax.text(126, 149,
            "图 2  GPU_2D 内部框图（命令域 / 像素源 / 加工面 / AXI 主口 / 显示通道）",
            ha="center", va="center", fontsize=15, fontweight="bold", color="#222")

    # ------------- 命令域 -------------
    group(ax, 10, 100, 80, 44, "CmdChannel —— 命令域（clk_ci = clk_acc = 100 MHz）",
          ec="#C07A22")
    group(ax, 13, 104, 42, 34, "CmdBuffer", ec="#C07A22", fs=8.6)
    box(ax, 15, 107, 17, 24, "CIUnpack", ("多条 CI → 一条命令", "FILL / BLIT / ARGS / FENCE"),
        kind="cmd", tfs=8.8, bfs=7.4)
    box(ax, 35, 107, 18, 24, "async_fifo", ("512 深 / 216 位", "内含 BlockRAM", "跨 clk_ci → clk_acc"),
        kind="mem", tfs=8.8, bfs=7.4)
    arrow(ax, (32, 119), (35, 119))
    box(ax, 60, 104, 27, 30, "CmdExec", ("一条命令 → h 个行任务", "地址递推 / 镜像",
                                         "模式 → 源使能（只此一处）", "发 / 完成 双判据"),
        kind="cmd", tfs=9.4, bfs=7.4)
    arrow(ax, (55, 119), (60, 119))
    lbl(ax, 57.5, 121, "命令字", fs=7.0)
    arrow(ax, (2, 126), (13, 126))
    lbl(ax, 2, 131, "CI 8 根\n← SoC", fs=7.4, ha="left")

    # ------------- 行任务广播 -------------
    bus(ax, [(62, 100), (62, 92), (4, 92), (4, 29), (13, 29)])
    lbl(ax, 63, 96.4, "行任务广播", fs=7.6, ha="left")
    lbl(ax, 33, 94.6, "line_start / dst / w / fx_en / src_addr[n] / flags[n] / param[n]", fs=6.6)
    arrow(ax, (30, 92), (30, 88))
    arrow(ax, (56, 92), (56, 88))

    # ------------- 源 -------------
    group(ax, 10, 54, 40, 34, "DDRSource —— 源①（DDR 读）", ec="#2F6E96")
    box(ax, 13, 57, 16, 20, "DDRUnpacker", ("128b → 4×RGB888", "丢 src_off"), kind="src", tfs=8.6, bfs=7.2)
    box(ax, 31, 57, 16, 20, "async_PingPong", ("一行双 bank", "+ BlockRAM"), kind="mem", tfs=8.2, bfs=7.2)
    arrow(ax, (29, 67), (31, 67))
    lbl(ax, 12, 55.2, "rd_cmd / rd_data ←→ AXIMastersHandler", fs=6.8, ha="left")

    group(ax, 54, 54, 56, 34, "PaletteSource —— 源②（片上 PRAM 素材）", ec="#2F6E96")
    box(ax, 57, 57, 18, 20, "PaletteRAM", ("素材索引 ROM", "编译期烧 hex"), kind="mem", tfs=8.6, bfs=7.2)
    box(ax, 78, 57, 29, 20, "PaletteDecoder", ("colorKey：源侧判键色", "PaletteLUT：4 份 RAM 查表", "输出 RGB + 掩码 + α"),
        kind="src", tfs=8.6, bfs=7.0)
    arrow(ax, (75, 67), (78, 67))

    # ------------- 加工面 -------------
    group(ax, 10, 12, 100, 38, "Factory —— 加工面（合成 → 打包 → 写）", ec="#3F7D34")
    box(ax, 13, 16, 40, 26, "Compositor", ("选路：源① / 源② / FILL 纯色", "imageCombine：图一 + 图二 合成",
                                           "alphaBlending：逐像素 α", "出口 96b 像素 + 字节掩码"),
        kind="proc", tfs=9.4, bfs=7.2)
    box(ax, 57, 16, 21, 26, "DDRPacker", ("96b → 128b", "跨拍拼接", "行首补 dst_off", "行尾丢尾巴"),
        kind="proc", tfs=9.0, bfs=7.2)
    box(ax, 82, 16, 25, 26, "WriteController", ("宽×3 算字节", "拆 4KB / 256 拍", "WSTRB 遮头尾", "line_done"),
        kind="proc", tfs=8.8, bfs=7.2)
    arrow(ax, (53, 29), (57, 29))
    arrow(ax, (78, 29), (82, 29))
    lbl(ax, 55, 31, "96b", fs=7.0)
    lbl(ax, 80, 31, "128b", fs=7.0)

    arrow(ax, (32, 54), (32, 42))
    lbl(ax, 34, 52.4, "src0_* 96b", fs=7.0, ha="left")
    poly(ax, [(66, 54), (66, 46), (50, 46), (50, 42)])
    lbl(ax, 62, 47.4, "src1_* 96b + mask + α", fs=7.0)

    # ------------- AXI 主口 -------------
    group(ax, 118, 12, 44, 24, "AXIMastersHandler", ec="#4D4D4D")
    box(ax, 121, 24, 38, 9, "AXI4_Master_Write", (), kind="bus", tfs=8.4)
    box(ax, 121, 13, 38, 9, "AXI4_Master_Read", (), kind="bus", tfs=8.4)
    arrow(ax, (107, 29), (121, 29))
    lbl(ax, 108, 31.4, "cmd_wr_* / wr_*", fs=7.0, ha="left")

    # 读命令 / 读数据
    poly(ax, [(121, 17), (116, 17), (116, 52), (47, 52), (47, 57)],
         color="#2F6E96", ls=(0, (4, 2)))
    lbl(ax, 104, 53.4, "rd_cmd / rd_data", fs=7.2, color="#2F6E96")

    # ------------- 两个反馈闭环 -------------
    poly(ax, [(110, 24), (113, 24), (113, 97), (80, 97), (80, 104)],
         color="#B0524A", ls=(0, (4, 2)))
    lbl(ax, 95, 98.6, "line_done → CmdExec（完成）", fs=7.2, color="#B0524A")

    poly(ax, [(110, 40), (116, 40), (116, 93), (70, 93), (70, 104)],
         color="#2F6E96", ls=(0, (4, 2)))
    lbl(ax, 92, 90.6, "line_buffer_free → CmdExec（节奏）", fs=7.2, color="#2F6E96")

    # ------------- 显示通道 -------------
    box(ax, 118, 106, 92, 26, "lineBuffer —— 显示通道（详见 图 3）", (
        "① bufferControler：按 {段0, 段1} 起 DDR 读（AXI4_Master → S1）",
        "② lineAligner：行首非 16B 对齐时丢 skip 字节（241 → 240 拍）",
        "③ async_PingPongBuffer：双 bank 跨 100 MHz ←→ 74.25 MHz",
        "④ bufferUnpacker：128b → RGB888 像素流",
        "⑤ HDMI_Top：pixel_timing + dvi_encoder_Top（3× tmds_encoder）",
    ), kind="disp", tfs=10.5, bfs=7.4)

    # ------------- 寄存器层 -------------
    box(ax, 118, 80, 92, 12, "GPU2D_APBHandler —— 寄存器 ←→ 功能（唯一翻译层）", (
        "→ lineBuffer：enable / mode / addr{段0, 段1}      ← lineBuffer：frame_start",
        "→ 寄存器格子：done_clr / fps_*_val                ← 加速器：busy / cmd_done / 拍数",
    ), kind="reg", tfs=9.0, bfs=6.8)
    arrow(ax, (188, 92), (188, 106), both=True)

    box(ax, 166, 62, 44, 12, "APB0_gpu2d（总线层 · 本模块之外）", (
        "在 APB0_Interconnect 里例化",
        "0x2000 观测 / 0x2040 显示",
    ), kind="ext", tfs=7.8, bfs=6.8, ls=(0, (4, 2)))
    arrow(ax, (188, 74), (188, 80))
    arrow(ax, (210, 68), (216, 68))
    lbl(ax, 217, 68, "← SoC APB3（槽 2）", fs=7.4, ha="left")

    # 三个闭环说明（填在右下空白处）
    ax.text(120, 58, "三个闭环（本图的重点）", fontsize=8.4, fontweight="bold",
            color="#444444", ha="left", va="top")
    for i, line in enumerate((
        "① 节奏：line_buffer_free =（各源 free）&（写侧 wr_free）→ 能不能发下一行",
        "② 完成：line_done = 这一行连 B 响应都回来了 → 命令什么时候算做完",
        "③ 翻页：frame_start → APBHandler 翻 acc_idx / cpu_idx + 帧率；",
        "        行模式两份锁存在这一拍同时切换（复位域不同，故意分开）",
    )):
        ax.text(120, 54.5 - i * 3.4, line, fontsize=7.2, color="#444444",
                ha="left", va="top")

    # 显示通道出口
    arrow(ax, (210, 121), (216, 121))
    lbl(ax, 217, 121, "→ Axi_Mux S1（显示取数）", fs=7.6, ha="left")
    arrow(ax, (210, 110), (216, 110))
    lbl(ax, 217, 110, "tmds_* → HDMI 引脚", fs=7.6, ha="left")

    # AXI 出口
    arrow(ax, (162, 28), (168, 28))
    lbl(ax, 169, 28, "AW / W / B → Axi_Mux S3（写）", fs=7.6, ha="left")
    arrow(ax, (162, 16), (168, 16))
    lbl(ax, 169, 16, "AR / R → Axi_Mux S2（读）", fs=7.6, ha="left")

    lbl(ax, 126, 2.5,
        "复位：命令侧 axi_rst_n（DDR 校准完成才放行）／ 显示侧 rst_hdmi_n（= axi_rst_n & HDMI PLL 锁定）",
        fs=7.4, color="#666666", bg=False)

    fig.savefig(os.path.join(HERE, "图2_GPU2D内部.png"), dpi=150, facecolor="white")
    plt.close(fig)


# ============================================================================
# 图 3 —— 显示通道 lineBuffer
# ============================================================================
def fig3():
    fig, ax = canvas(214, 84)
    ax.text(107, 81, "图 3  显示通道 lineBuffer 内部框图",
            ha="center", va="center", fontsize=14.5, fontweight="bold", color="#222")

    # 两个时钟域
    ax.add_patch(Rectangle((2, 46), 132, 32, facecolor="#F7F9FB",
                           edgecolor="#B9C6D2", ls=(0, (6, 4)), lw=1.0, zorder=0))
    ax.add_patch(Rectangle((138, 4), 70, 74, facecolor="#FDF7F6",
                           edgecolor="#D9B4AE", ls=(0, (6, 4)), lw=1.0, zorder=0))
    ax.text(4, 76.4, "写域  clk = 100 MHz（AXI / 寄存器侧）", ha="left", va="top",
            fontsize=9.0, color="#5A6B7B", fontweight="bold", zorder=1)
    ax.text(140, 76.4, "读域  clk_HDMI_x1 = 74.25 MHz（像素侧）", ha="left", va="top",
            fontsize=9.0, color="#A5716A", fontweight="bold", zorder=1)

    box(ax, 5, 52, 27, 18, "AXI4_Master", ("读 DDR → S1", "单笔不跨 4KB"), kind="bus", tfs=9.0, bfs=7.0)
    box(ax, 38, 50, 44, 20, "bufferControler", ("行预取：按 {段0, 段1} 基址",
                                                "拆 4KB 笔：241 拍 / 2×120 拍"), kind="disp", tfs=9.6, bfs=7.2)
    box(ax, 88, 52, 40, 18, "lineAligner", ("32B 窗口 + 桶形移位", "丢行首 skip 字节"), kind="disp", tfs=9.6, bfs=7.2)

    group(ax, 140, 46, 66, 26, "async_PingPongBuffer（双 bank）", ec="#B0524A", fs=8.2)
    box(ax, 142, 48, 30, 22, "写侧", ("async_fifo", "BlockRAM", "wr_block_done"), kind="mem", tfs=8.6, bfs=6.8)
    box(ax, 174, 48, 30, 22, "读侧", ("rd_switch_tgl", "块切换只在读侧", "两边 bank 互补"), kind="mem", tfs=8.6, bfs=6.8)
    arrow(ax, (172, 59), (174, 59))

    box(ax, 140, 24, 66, 16, "bufferUnpacker", (
        "16 字节/拍 → RGB888（24b / 拍）",
        "按余量跨拍拼像素（5,5,6… 循环）"), kind="disp", tfs=9.6, bfs=7.0)
    arrow(ax, (189, 46), (189, 40))
    lbl(ax, 190, 43, "读口 240 拍/行", fs=7.0, ha="left")

    box(ax, 140, 2, 66, 18, "HDMI_Top", (
        "pixel_timing：1280×720@60 时序 / data_req / frame_start",
        "dvi_encoder_Top：3× tmds_encoder（8b → 10b）",
        "输出 tmds_blue / green / red / clk → LVDS 硬核 TX"), kind="disp", tfs=9.4, bfs=6.8)
    arrow(ax, (173, 24), (173, 20))
    arrow(ax, (206, 11), (212, 11))

    # 主数据通路
    arrow(ax, (32, 61), (38, 61), both=True)
    lbl(ax, 35, 63.2, "命令 →\n数据 ←", fs=6.0)
    arrow(ax, (82, 60), (88, 60))
    arrow(ax, (128, 60), (140, 60))
    lbl(ax, 134, 62.2, "跨域", fs=6.8, color="#B0524A")

    # 配置
    arrow(ax, (60, 70), (60, 74.5))
    lbl(ax, 61, 76.0, "← GPU2D_APBHandler：enable / mode / addr{段0, 段1}", fs=7.0, ha="left")

    # 辅助说明
    box(ax, 30, 4, 96, 18, "两个跨域辅助（都不是数据通路）", (
        "① asyn_rst_syn：rst_hdmi_n → 像素域「异步断言 / 同步释放」",
        "② frame_start 跨域桥：读域 toggle → 写域 2FF + 边沿比较，出单拍",
        "   （frame_start 同时送 bufferControler 与 GPU2D_APBHandler）",
    ), kind="clk", tfs=8.6, bfs=6.8)
    arrow(ax, (140, 11), (126, 11), color="#B0524A", ls=(0, (4, 2)))
    lbl(ax, 133, 13, "frame_start", fs=6.6, color="#B0524A")

    fig.savefig(os.path.join(HERE, "图3_显示通道.png"), dpi=150, facecolor="white")
    plt.close(fig)


# ============================================================================
# 图 4 —— (a) Axi_Mux 内部   (b) 时钟树与三级复位
# ============================================================================
def fig4():
    fig = plt.figure(figsize=(20, 9.8), dpi=150)
    ax1 = fig.add_axes([0.005, 0.02, 0.49, 0.88])
    ax2 = fig.add_axes([0.505, 0.02, 0.49, 0.88])
    for ax in (ax1, ax2):
        ax.set_xlim(0, 100)
        ax.set_ylim(0, 96)
        ax.set_aspect("equal")
        ax.axis("off")

    fig.text(0.5, 0.955,
             "图 4  总线与时钟 / 复位（(a) Axi_Mux 内部 ／ (b) 时钟树与三级复位门控）",
             ha="center", va="center", fontsize=14.5, fontweight="bold")

    # ---------------- (a) Axi_Mux ----------------
    ax1.text(50, 92, "(a) Axi_Mux：4 从 1 主，5 条通道各自独立仲裁", ha="center",
             fontsize=11.5, fontweight="bold", color="#333")

    slaves = [("S0  SoC io_ddrA（CPU 唯一 DDR 出口）", 70, "soc"),
              ("S1  显示取数（lineBuffer 只读）", 56, "disp"),
              ("S2  加速器读（BLIT 读源）", 42, "src"),
              ("S3  加速器写（帧缓存）", 28, "src")]
    for name, y, kind in slaves:
        box(ax1, 3, y, 30, 9, name, (), kind=kind, tfs=7.6)
        ax1.plot([33, 36], [y + 4.5, y + 4.5], color="#333", lw=1.4, zorder=4)

    ax1.plot([36, 36], [32.5, 74.5], color="#333", lw=2.2, zorder=4)

    chans = [("AW 写地址通道", 70), ("W  写数据通道", 60), ("B  写响应通道", 48),
             ("AR 读地址通道", 38), ("R  读数据通道", 28)]
    for name, y in chans:
        box(ax1, 42, y, 26, 7.5, name, (), kind="bus", tfs=7.8)
        ax1.plot([36, 42], [y + 3.75, y + 3.75], color="#333", lw=1.4, zorder=4)

    box(ax1, 74, 26, 20, 51, "M → ddr3_top", (
        "s_axi 128b",
        "28b 地址",
        "ID 全 0",
        "单笔不跨 4KB",
    ), kind="clk", tfs=8.6, bfs=7.0)
    for _, y in chans:
        arrow(ax1, (68, y + 3.75), (74, y + 3.75), mut=8)

    ax1.text(50, 84,
             "Cfg_Arb_Type = 0（轮询共享）· Cfg_Cnl_Mask 必须 4'b1111",
             fontsize=7.6, ha="center", color="#555")

    box(ax1, 3, 3, 93, 17, "Axi_Mux 内部子模块（官方 IP，按通道分工）", (
        "Axi_Mux_Addr_Ctrl ×2：AW / AR 的通道切换状态机",
        "Axi_Mux_Wr_Ctrl（W/B）+ Axi_Mux_Rd_Ctrl（R）：通道锚定与切换",
        "Info_Fifo_Axi_Mux ×3 + Axi_Mux_Info ×3：把通道号沿 AW→W→B、AR→R 传下去",
    ), kind="ext", tfs=8.2, bfs=6.8)

    # ---------------- (b) 时钟 / 复位 ----------------
    ax2.text(50, 92, "(b) 时钟树与三级复位门控", ha="center",
             fontsize=11.5, fontweight="bold", color="#333")

    box(ax2, 2, 76, 18, 9, "25 MHz 晶振", (), kind="io", tfs=8.4)
    box(ax2, 26, 64, 70, 22, "3 个 PLL（Efinity PLL）", (
        "pll_inst1 → core_clk 100 MHz = user_clk（ASYN_AXI_CLK = 0）",
        "DDR3_PLL  → sdram_clk 400 MHz、rx/tx_cal_clk、clk_100m",
        "pll_HDMI  → clk_HDMI_x1 74.25 MHz、clk_HDMI_x5 371.25 MHz",
    ), kind="clk", tfs=9.2, bfs=7.0)
    arrow(ax2, (20, 80), (26, 80), mut=7)

    box(ax2, 3, 48, 94, 11, "sys_rst", (
        "= pll_locked & user_pll_locked　→ APB0 寄存器格子、Axi_Mux",
    ), kind="reg", tfs=8.8, bfs=7.0)
    box(ax2, 3, 30, 94, 11, "axi_rst_n", (
        "= sys_rst & cal_done_d2（DDR3 校准完成）　→ rst_ci_n / rst_acc_n：命令侧 + 加速器",
    ), kind="reg", tfs=8.8, bfs=7.0)
    box(ax2, 3, 12, 94, 11, "rst_hdmi_n", (
        "= axi_rst_n & pll_HDMI_locked　→ 只给显示通道（再经 asyn_rst_syn 进像素域）",
    ), kind="reg", tfs=8.8, bfs=7.0)

    arrow(ax2, (50, 48), (50, 41), mut=7)
    arrow(ax2, (50, 30), (50, 23), mut=7)
    lbl(ax2, 52, 44.5, "DDR 校准完成才放行", fs=6.8, ha="left", color="#666")
    lbl(ax2, 52, 26.5, "HDMI PLL 锁定才放行", fs=6.8, ha="left", color="#666")

    ax2.text(3, 6,
             "注：三级复位的释放顺序不能合并 —— 寄存器格子必须早于 axi_rst_n 放行（CPU 要先写基址），"
             "rst_hdmi_n 必须晚于 axi_rst_n（否则显示侧读到未校准的 DDR）。",
             fontsize=6.8, ha="left", va="center", color="#555")

    fig.savefig(os.path.join(HERE, "图4_总线与时钟复位.png"), dpi=150, facecolor="white")
    plt.close(fig)


# ============================================================================
# 图 5 —— APB0 子系统
# ============================================================================
def fig5():
    fig, ax = canvas(162, 66)
    ax.text(81, 63, "图 5  APB0 子系统（地址译码 / 外设总线层 / 功能层）",
            ha="center", va="center", fontsize=13.5, fontweight="bold", color="#222")

    box(ax, 2, 30, 24, 13, "SoC io_apbSlave_0", ("APB3 · 0xf810_0000",), kind="soc", tfs=8.2, bfs=6.6)

    group(ax, 30, 14, 76, 44, "APB0_Interconnect", ec="#C07A22")
    box(ax, 33, 40, 28, 14, "APB0_Decoder", ("PADDR[15:12] → 槽选", "未命中 → PSLVERROR"), kind="bus", tfs=8.6, bfs=6.8)

    box(ax, 66, 44, 36, 11, "槽 0   0x0000", ("KEY_DATA / IRQ_EN / KEY_ID",), kind="cmd", tfs=8.0, bfs=6.4)
    box(ax, 66, 30, 36, 11, "槽 1   0x1000（空）", ("访问它 → PSLVERROR = 1",), kind="io", tfs=8.0, bfs=6.4)
    box(ax, 66, 16, 36, 11, "槽 2   0x2000  GPU_2D", ("0x2000 观测 / 0x2040 显示",), kind="cmd", tfs=8.0, bfs=6.4)

    arrow(ax, (26, 36), (33, 44), mut=7)
    for y in (49.5, 35.5, 21.5):
        arrow(ax, (61, 47), (66, y), mut=7)

    box(ax, 110, 44, 24, 11, "APB0_key", (), kind="cmd", tfs=8.0)
    box(ax, 138, 44, 22, 11, "key", ("消抖 + 边沿",), kind="cmd", tfs=8.0, bfs=6.6)
    arrow(ax, (102, 49.5), (110, 49.5), mut=7)
    arrow(ax, (134, 49.5), (138, 49.5), mut=7)

    box(ax, 110, 16, 24, 11, "APB0_gpu2d", (), kind="cmd", tfs=8.0)
    box(ax, 138, 14, 22, 15, "GPU_2D 功能侧", ("GPU2D_APBHandler",), kind="proc", tfs=7.4, bfs=6.2)
    arrow(ax, (102, 21.5), (110, 21.5), mut=7)
    arrow(ax, (134, 21.5), (138, 21.5), mut=7)

    # 回程
    poly(ax, [(84, 16), (84, 9), (14, 9), (14, 30)], color="#C07A22", ls=(0, (4, 2)))
    lbl(ax, 47, 10.6, "回程三选一：PRDATA / PREADY / PSLVERROR", fs=7.0, color="#C07A22")

    ax.text(2, 4,
            "槽 2 内用 PADDR[6] 分子块：观测与显示寄存器同一个基址 0xF8102000，软件不用记两个外设。",
            fontsize=6.8, ha="left", va="center", color="#555")

    fig.savefig(os.path.join(HERE, "图5_APB0子系统.png"), dpi=150, facecolor="white")
    plt.close(fig)


if __name__ == "__main__":
    fig1()
    fig2()
    fig3()
    fig4()
    fig5()
    print("done:", HERE)
