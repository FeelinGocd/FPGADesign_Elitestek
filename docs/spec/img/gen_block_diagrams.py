# -*- coding: utf-8 -*-
"""
生成《系统整体框图》用的 5 张 PNG（2026-10-30 版：300 MHz acc / 100 MHz mem 双时钟域 + CDC）。

来源：RTL/ 当前代码 + project/GamingPlayUnit/GamingPlayUnit.xml 的编译文件表。
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

C = {
    "clk":  ("#EFEAF7", "#5B4E9E"),
    "io":   ("#F2F2F2", "#6B6B6B"),
    "soc":  ("#E8E6F5", "#5B4E9E"),
    "bus":  ("#EDEDED", "#4D4D4D"),
    "cmd":  ("#FDE8C8", "#C07A22"),
    "src":  ("#DCEBF7", "#2F6E96"),
    "proc": ("#E3F1DC", "#3F7D34"),
    "mem":  ("#F7E4F1", "#8E4B7F"),
    "disp": ("#FBE3E1", "#B0524A"),
    "reg":  ("#FFF6CC", "#A08A1E"),
    "cdc":  ("#E9F3EE", "#2E7D6B"),
    "ext":  ("#FFFFFF", "#8A8A8A"),
}

LW_EDGE = 1.2
LW_ARROW = 1.4


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
          fc="none", zorder=1, lx=1.4, ly=1.4):
    ax.add_patch(Rectangle((x, y), w, h, linewidth=1.1, edgecolor=ec,
                           facecolor=fc, linestyle=ls, zorder=zorder))
    ax.text(x + lx, y + h - ly, label, ha="left", va="top", fontsize=fs,
            color=ec, fontweight="bold", zorder=zorder + 6,
            bbox=dict(fc="white", ec="none", alpha=0.9, pad=0.6))


def band(ax, x, y, w, h, label, fc, ec, fs=9.0):
    ax.add_patch(Rectangle((x, y), w, h, facecolor=fc, edgecolor=ec,
                           ls=(0, (6, 4)), lw=1.0, zorder=0))
    ax.text(x + 2, y + h - 2, label, ha="left", va="top", fontsize=fs,
            color=ec, fontweight="bold", zorder=0.5,
            bbox=dict(fc="white", ec="none", alpha=0.85, pad=0.5))


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
        arrow(ax, pts[-2], pts[-1], color=color, lw=lw, ls=ls, mut=mut, both=both)
    else:
        ax.plot([pts[-2][0], pts[-1][0]], [pts[-2][1], pts[-1][1]],
                color=color, lw=lw, ls=ls, zorder=4)


def bus(ax, pts, color="#4D4D4D", lw=3.2, mut=11):
    ax.plot([p[0] for p in pts], [p[1] for p in pts], color=color, lw=lw,
            zorder=4, solid_capstyle="butt", solid_joinstyle="miter")
    arrow(ax, pts[-2], pts[-1], color=color, lw=lw, mut=mut)


def lbl(ax, x, y, text, fs=8.0, color="#333333", ha="center", va="center",
        rot=0, bg=True, zorder=7):
    ax.text(x, y, text, ha=ha, va=va, fontsize=fs, color=color, rotation=rot,
            zorder=zorder, linespacing=1.3,
            bbox=dict(fc="white", ec="none", alpha=0.9, pad=0.7) if bg else None)


# ============================================================================
# 图 1 —— 系统顶层（300 MHz acc + 100 MHz mem + 74.25 MHz pixel）
# ============================================================================
def fig1():
    fig, ax = canvas(200, 114)
    ax.text(100, 111, "图 1  系统顶层框图（GamingPlayUnit_Top）",
            ha="center", va="center", fontsize=15, fontweight="bold", color="#222")

    box(ax, 2, 78, 36, 30, "时钟生成（3 个 PLL）", (
        "pll_inst1  → core_clk 300 MHz",
        "           → core_clk_feedback 100 MHz",
        "DDR3_PLL   → sdram_clk 400 MHz",
        "pll_HDMI   → clk_HDMI_x1 74.25 MHz",
        "           → clk_HDMI_x5 371.25 MHz",
    ), kind="clk", tfs=9.6, bfs=7.4)

    box(ax, 2, 52, 34, 22, "板级接口", (
        "key_pin[3:0]（按下 = 0）",
        "UART0 / SPI0 / GPIO0",
        "JTAG（硬核 TAP）",
    ), kind="io", tfs=10)

    box(ax, 48, 58, 56, 50, "SoC_RISCV32（Sapphire SoC · VexRiscv · IP）", (
        "① io_systemClk 300 MHz（CPU 域）",
        "② io_memoryClk 100 MHz（DDR 域）",
        "③ io_ddrA：AXI4 主口 128b，ID 全 0",
        "④ io_apbSlave_0：APB3（300 MHz）",
        "⑤ cpu0_customInstruction：CI 8 根",
        "⑥ 4 KB D-cache / PLIC",
    ), kind="soc", tfs=10.2, bfs=7.8)

    box(ax, 116, 58, 34, 50, "Axi_Mux", (
        "Sys_Clk = mem_clk 100 MHz",
        "4 从 1 主 · 5 通道独立仲裁",
        "S0 ← SoC io_ddrA",
        "S1 ← 显示取数",
        "S2 ← 加速器读",
        "S3 ← 加速器写",
        "轮询（Cfg_Arb_Type=0）",
    ), kind="bus", tfs=10.5, bfs=7.4)

    box(ax, 162, 62, 36, 42, "ddr3_top", (
        "Elitestek DDR3 控制器 IP（加密）",
        "axi_clk / core_clk = mem_clk 100 MHz",
        "AXI4 从口 128b / 28b 地址",
        "PHY：写校准 / 读定级 / MPR",
        "SDRAM 400 MHz",
    ), kind="clk", tfs=10.5, bfs=7.2)

    box(ax, 162, 38, 36, 18, "DDR3 颗粒（板载）", (
        "ck / addr / ba / cs",
        "dq / dqs / dm / odt",
    ), kind="io", tfs=9.8)

    box(ax, 48, 30, 56, 20, "APB0_Interconnect（APB 子系统 · 300 MHz）", (
        "APB0_Decoder：槽 0 → key，槽 2 → GPU_2D",
        "槽内 APB0_gpu2d：0x2000 观测 / 0x2040 显示",
    ), kind="bus", tfs=9.4, bfs=7.6)

    box(ax, 2, 26, 34, 14, "key", ("同步 + 消抖 + 按下沿", ), kind="cmd", tfs=9.6)

    box(ax, 48, 4, 56, 20, "GPU_2D（加速器 + 显示通道 · 功能层）", (
        "clk_ci / clk_acc 300 MHz ｜ clk_mem 100 MHz ｜ 像素 74.25 MHz",
        "命令域 → 像素源 → 加工面 → MemoryCDC → AXI 主口",
        "显示：DisplayCDC（帧边界握手）→ lineBuffer → TMDS",
    ), kind="proc", tfs=9.8, bfs=7.2)

    box(ax, 112, 30, 34, 20, "复位门控（顶层生成）", (
        "sys_rst = pll_locked",
        "          & user_pll_locked",
        "→ ResetSync(300) → axi_rst_n",
        "→ ResetSync(100) → mem_rst_n",
        "rst_hdmi_n = axi_rst_n & PLL",
    ), kind="reg", tfs=8.8, bfs=6.8)

    box(ax, 112, 4, 34, 18, "TMDS → HDMI 连接器", (
        "4 对 LVDS 硬核 TX",
        "clk / d0 / d1 / d2",
    ), kind="io", tfs=9.6)

    # ---- 连线 ----
    arrow(ax, (38, 102), (48, 102))
    lbl(ax, 43, 104, "300 MHz", fs=7.2)
    arrow(ax, (38, 96), (48, 96))
    lbl(ax, 43, 94.4, "100 MHz", fs=7.2)

    poly(ax, [(34, 86), (40, 86), (40, 14), (48, 14)])
    lbl(ax, 39, 50, "300 / 100 / 74.25 MHz", fs=7.0, rot=90)

    arrow(ax, (36, 64), (48, 64))
    lbl(ax, 42, 66, "按键 / 串口 / SPI", fs=6.8)

    arrow(ax, (104, 98), (116, 98))
    lbl(ax, 110, 100, "io_ddrA（S0）", fs=7.2)

    arrow(ax, (150, 98), (162, 98))
    lbl(ax, 156, 100, "M", fs=8.2)

    arrow(ax, (174, 62), (174, 58))
    arrow(ax, (186, 58), (186, 62))
    lbl(ax, 180, 60, "ddr_*", fs=7.4)

    arrow(ax, (76, 58), (76, 50))
    lbl(ax, 77, 54, "io_apbSlave_0（APB3）", fs=7.2, ha="left")

    arrow(ax, (48, 33), (36, 33))
    lbl(ax, 42, 35, "key_state / key_edge", fs=6.4)

    arrow(ax, (88, 30), (88, 24))
    arrow(ax, (82, 24), (82, 30))

    arrow(ax, (100, 50), (100, 58), ls=(0, (4, 2)), color="#B0524A")
    lbl(ax, 99, 52.6, "IRQ_frame", fs=7.0, ha="right", color="#B0524A")

    poly(ax, [(48, 67), (44, 67), (44, 8), (48, 8)])
    lbl(ax, 43, 44, "CI 8 根", fs=7.0, rot=90)

    bus(ax, [(104, 18), (110, 18), (110, 54), (130, 54), (130, 58)])
    lbl(ax, 119, 56.2, "3× AXI4 主口 → S1 显示 / S2 读 / S3 写", fs=7.2, bg=False)

    arrow(ax, (104, 11), (112, 11))
    lbl(ax, 108, 13, "tmds_*", fs=7.2)

    fig.savefig(os.path.join(HERE, "图1_系统顶层.png"), dpi=150, facecolor="white")
    plt.close(fig)


# ============================================================================
# 图 2 —— GPU_2D：acc 300 MHz / mem 100 MHz 双域 + 三个跨域桥
# ============================================================================
def fig2():
    fig, ax = canvas(262, 122)
    ax.text(131, 119,
            "图 2  GPU_2D 内部框图（acc 300 MHz ／ mem 100 MHz 双时钟域）",
            ha="center", va="center", fontsize=14.5, fontweight="bold", color="#222")

    band(ax, 2, 4, 140, 110, "acc 域 300 MHz（clk_ci = clk_acc）", "#F7F9FB", "#5A6B7B", fs=8.6)
    band(ax, 144, 4, 50, 110, "跨域桥", "#F3F8F6", "#2E7D6B", fs=8.6)
    band(ax, 196, 4, 62, 110, "mem 域 100 MHz（clk_mem）", "#FDF7F6", "#A5716A", fs=8.6)

    # ---------- 命令域 ----------
    group(ax, 8, 92, 78, 20, "CmdChannel（命令域）", ec="#C07A22", fs=8.2)
    group(ax, 10, 94, 36, 16, "CmdBuffer", ec="#C07A22", fs=7.6, ly=1.0, lx=1.0)
    box(ax, 11, 95, 16, 12, "CIUnpack", ("CI → 一条命令",), kind="cmd", tfs=7.6, bfs=6.6)
    box(ax, 29, 95, 16, 12, "async_fifo", ("512×216b",), kind="mem", tfs=7.6, bfs=6.6)
    arrow(ax, (27, 101), (29, 101))
    box(ax, 49, 95, 34, 14, "CmdExec", ("一条命令 → h 行任务", "地址递推 / 两级流水"), kind="cmd", tfs=8.4, bfs=6.8)
    arrow(ax, (46, 101), (49, 101))
    arrow(ax, (3, 101), (10, 101))
    lbl(ax, 3, 104, "CI", fs=6.6, ha="left")

    # ---------- 寄存器层 ----------
    box(ax, 86, 96, 54, 14, "GPU2D_APBHandler", ("寄存器 ←→ 功能（唯一翻译层）", "翻页 / 帧率 / 观测"),
        kind="reg", tfs=8.2, bfs=6.6)
    lbl(ax, 86, 92.6, "↑ 寄存器来自 APB0_gpu2d（槽 2）", fs=6.2, ha="left", color="#8A7414")

    # ---------- 行任务广播 ----------
    bus(ax, [(64, 92), (64, 88.5), (6, 88.5)])
    lbl(ax, 65, 86.8, "行任务广播", fs=6.4, ha="left")
    arrow(ax, (28, 88.5), (28, 84))
    arrow(ax, (76, 88.5), (76, 84))
    poly(ax, [(6, 88.5), (6, 32), (11, 32)])

    # ---------- 两路源 ----------
    group(ax, 8, 62, 42, 22, "DDRSource —— 源①", ec="#2F6E96", fs=8.0)
    box(ax, 11, 64, 17, 16, "DDRUnpacker", ("128b → 4 像素",), kind="src", tfs=7.4, bfs=6.4)
    box(ax, 30, 64, 17, 16, "PingPong", ("一行双 bank",), kind="mem", tfs=7.4, bfs=6.4)
    arrow(ax, (28, 72), (30, 72))

    group(ax, 54, 62, 50, 22, "PaletteSource —— 源②", ec="#2F6E96", fs=8.0)
    box(ax, 56, 64, 16, 16, "PaletteRAM", ("素材 ROM",), kind="mem", tfs=7.4, bfs=6.4)
    box(ax, 74, 64, 28, 16, "PaletteDecoder", ("colorKey + LUT",), kind="src", tfs=7.4, bfs=6.4)
    arrow(ax, (72, 72), (74, 72))

    # ---------- 加工面 ----------
    group(ax, 8, 8, 130, 42, "Factory —— 加工面（合成 → 弹性寄存 → 打包 → 写）", ec="#3F7D34", fs=8.2)
    box(ax, 11, 16, 40, 28, "Compositor", ("选路 / imageCombine", "alpha 4 级流水", "blend 槽 + 反压"),
        kind="proc", tfs=8.2, bfs=6.8)
    box(ax, 54, 16, 18, 28, "PixelPipeline", ("96b", "弹性一级"), kind="proc", tfs=7.4, bfs=6.4)
    box(ax, 75, 16, 20, 28, "DDRPacker", ("96b→128b", "行首补 off", "行尾丢尾"), kind="proc", tfs=7.6, bfs=6.4)
    box(ax, 98, 16, 22, 28, "WriteController", ("拆 4KB/256 拍", "WSTRB", "line_done"), kind="proc", tfs=7.2, bfs=6.4)
    arrow(ax, (51, 30), (54, 30))
    arrow(ax, (72, 30), (75, 30))
    arrow(ax, (95, 30), (98, 30))

    arrow(ax, (28, 62), (28, 44))
    lbl(ax, 29, 52, "src0", fs=6.4, ha="left")
    poly(ax, [(76, 62), (76, 58), (45, 58), (45, 44)])
    lbl(ax, 60, 59.2, "src1（96b+mask+α）", fs=6.2)

    # ---------- MemoryCDC / AXI 主口 ----------
    box(ax, 146, 16, 44, 34, "GPU2D_MemoryCDC", (
        "5× CdcStream：",
        "写命令 / 写数据 / B",
        "读命令 / 读数据(带 last)",
        "有序 FIFO，逐笔握手",
    ), kind="cdc", tfs=7.8, bfs=6.2)
    arrow(ax, (120, 33), (146, 33), both=True)
    lbl(ax, 122, 30.4, "cmd_wr / wr_* / rd_cmd", fs=6.0, ha="left")

    box(ax, 198, 18, 58, 30, "AXIMastersHandler（mem 100M）", (
        "AXI4_Master_Write → S3",
        "AXI4_Master_Read  → S2",
    ), kind="bus", tfs=8.2, bfs=6.8)
    arrow(ax, (190, 33), (198, 33), both=True)

    arrow(ax, (256, 44), (260, 44))
    lbl(ax, 257, 47, "S3", fs=6.8)
    arrow(ax, (256, 24), (260, 24))
    lbl(ax, 257, 27, "S2", fs=6.8)

    # ---------- DisplayCDC / 显示通道 ----------
    box(ax, 146, 74, 44, 32, "GPU2D_DisplayCDC", (
        "帧边界 request / ack",
        "mode / addr 随帧",
        "提交后才放首行预取",
    ), kind="cdc", tfs=7.8, bfs=6.2)

    box(ax, 198, 72, 58, 34, "lineBuffer（mem + 像素）", (
        "bufferControler → lineAligner",
        "→ PingPong 100→74.25",
        "→ bufferUnpacker → HDMI_Top",
    ), kind="disp", tfs=8.2, bfs=6.4)

    arrow(ax, (140, 104), (146, 104))
    lbl(ax, 143, 106.2, "配置", fs=6.2, ha="center")
    arrow(ax, (146, 96), (140, 96))
    lbl(ax, 143, 93.6, "frame_start", fs=6.2, ha="center")

    arrow(ax, (190, 100), (198, 100))
    lbl(ax, 194, 102.2, "配置 + commit", fs=5.8)
    arrow(ax, (198, 86), (190, 86))
    lbl(ax, 194, 83.6, "frame_start", fs=5.8)

    arrow(ax, (256, 98), (260, 98))
    lbl(ax, 257, 101, "S1", fs=6.8)
    arrow(ax, (256, 80), (260, 80))
    lbl(ax, 257, 83, "tmds", fs=6.8)

    # ---------- 反馈闭环 ----------
    poly(ax, [(138, 30), (139, 30), (139, 90), (83, 90), (83, 95)],
         color="#B0524A", ls=(0, (4, 2)))
    lbl(ax, 110, 91.4, "line_done + line_buffer_free → CmdExec", fs=6.0, color="#B0524A")

    lbl(ax, 131, 1.4,
        "RESET：acc 域 axi_rst_n（ResetSync@300M）／ mem 域 mem_rst_n（ResetSync@100M）／ 显示侧 rst_hdmi_n",
        fs=6.2, color="#666666", bg=False)

    fig.savefig(os.path.join(HERE, "图2_GPU2D内部.png"), dpi=150, facecolor="white")
    plt.close(fig)


# ============================================================================
# 图 3 —— 显示通道（写域 100 MHz + 像素域 74.25 MHz + 帧提交握手）
# ============================================================================
def fig3():
    fig, ax = canvas(214, 88)
    ax.text(107, 85, "图 3  显示通道 lineBuffer 内部框图",
            ha="center", va="center", fontsize=14.5, fontweight="bold", color="#222")

    ax.add_patch(Rectangle((2, 46), 132, 32, facecolor="#F7F9FB",
                           edgecolor="#B9C6D2", ls=(0, (6, 4)), lw=1.0, zorder=0))
    ax.add_patch(Rectangle((138, 4), 70, 74, facecolor="#FDF7F6",
                           edgecolor="#D9B4AE", ls=(0, (6, 4)), lw=1.0, zorder=0))
    ax.text(4, 76.4, "写域  clk = mem_clk 100 MHz（AXI / 寄存器侧）", ha="left", va="top",
            fontsize=8.6, color="#5A6B7B", fontweight="bold", zorder=1)
    ax.text(140, 76.4, "读域  clk_HDMI_x1 = 74.25 MHz（像素侧）", ha="left", va="top",
            fontsize=8.6, color="#A5716A", fontweight="bold", zorder=1)

    box(ax, 5, 52, 27, 18, "AXI4_Master", ("读 DDR → S1", "单笔不跨 4KB"), kind="bus", tfs=8.8, bfs=6.8)
    box(ax, 38, 50, 44, 20, "bufferControler", ("行预取：按 {段0, 段1} 基址",
                                                "241 拍 = 240 + 1 拍余量"), kind="disp", tfs=9.2, bfs=6.9)
    box(ax, 88, 52, 40, 18, "lineAligner", ("32B 窗口 + 桶形移位", "丢行首 skip 字节"), kind="disp", tfs=9.2, bfs=6.9)

    group(ax, 140, 46, 66, 26, "async_PingPongBuffer（双 bank）", ec="#B0524A", fs=8.0)
    box(ax, 142, 48, 30, 22, "写侧", ("async_fifo", "BlockRAM", "wr_block_done"), kind="mem", tfs=8.2, bfs=6.4)
    box(ax, 174, 48, 30, 22, "读侧", ("rd_switch_tgl", "块切换只在读侧", "两边 bank 互补"), kind="mem", tfs=8.2, bfs=6.4)
    arrow(ax, (172, 59), (174, 59))

    box(ax, 140, 24, 66, 16, "bufferUnpacker", (
        "16 字节/拍 → RGB888（24b / 拍）",
        "按余量跨拍拼像素（5,5,6… 循环）"), kind="disp", tfs=9.0, bfs=6.7)
    arrow(ax, (189, 46), (189, 40))
    lbl(ax, 190, 43, "读口 240 拍/行", fs=6.6, ha="left")

    box(ax, 140, 2, 66, 18, "HDMI_Top", (
        "pixel_timing：1280×720@60 时序 / data_req / frame_start",
        "dvi_encoder_Top：3× tmds_encoder（8b → 10b）",
        "输出 tmds_blue / green / red / clk → LVDS 硬核 TX"), kind="disp", tfs=8.8, bfs=6.4)
    arrow(ax, (173, 24), (173, 20))
    arrow(ax, (206, 11), (212, 11))

    arrow(ax, (32, 61), (38, 61), both=True)
    lbl(ax, 35, 63.2, "命令 →\n数据 ←", fs=5.8)
    arrow(ax, (82, 60), (88, 60))
    arrow(ax, (128, 60), (140, 60))
    lbl(ax, 134, 62.2, "跨域", fs=6.4, color="#B0524A")

    arrow(ax, (60, 70), (60, 74.5))
    lbl(ax, 61, 76.0, "← DisplayCDC：enable / mode / addr{段0, 段1}", fs=6.6, ha="left")

    box(ax, 30, 4, 96, 18, "帧边界握手（GPU2D_DisplayCDC 的另一半在 acc 域）", (
        "① 本域出 frame_start → 请求翻页；② acc 域翻页后锁存 mode/addr 并应答",
        "③ 本域收到 frame_commit 才放行下一帧首行预取（EXTERNAL_FRAME_COMMIT=1）",
        "④ 像素域复位：ResetSync 同步释放（异步断言）",
    ), kind="cdc", tfs=8.0, bfs=6.2)
    arrow(ax, (140, 10), (126, 10), color="#2E7D6B", ls=(0, (4, 2)))
    lbl(ax, 133, 12, "frame_commit", fs=6.2, color="#2E7D6B")

    fig.savefig(os.path.join(HERE, "图3_显示通道.png"), dpi=150, facecolor="white")
    plt.close(fig)


# ============================================================================
# 图 4 —— (a) Axi_Mux（100 MHz）  (b) 三个时钟域与复位同步
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
             "图 4  总线与时钟／复位（(a) Axi_Mux 内部 ／ (b) 三个时钟域与复位同步）",
             ha="center", va="center", fontsize=14.5, fontweight="bold")

    # ---------------- (a) Axi_Mux ----------------
    ax1.text(50, 92, "(a) Axi_Mux：4 从 1 主，5 条通道各自独立仲裁（100 MHz）", ha="center",
             fontsize=11.0, fontweight="bold", color="#333")

    slaves = [("S0  SoC io_ddrA（CPU 唯一 DDR 出口）", 70, "soc"),
              ("S1  显示取数（lineBuffer 只读）", 56, "disp"),
              ("S2  加速器读（BLIT 读源）", 42, "src"),
              ("S3  加速器写（帧缓存）", 28, "src")]
    for name, y, kind in slaves:
        box(ax1, 3, y, 30, 9, name, (), kind=kind, tfs=7.4)
        ax1.plot([33, 36], [y + 4.5, y + 4.5], color="#333", lw=1.4, zorder=4)

    ax1.plot([36, 36], [32.5, 74.5], color="#333", lw=2.2, zorder=4)

    chans = [("AW 写地址通道", 70), ("W  写数据通道", 60), ("B  写响应通道", 48),
             ("AR 读地址通道", 38), ("R  读数据通道", 28)]
    for name, y in chans:
        box(ax1, 42, y, 26, 7.5, name, (), kind="bus", tfs=7.6)
        ax1.plot([36, 42], [y + 3.75, y + 3.75], color="#333", lw=1.4, zorder=4)

    box(ax1, 74, 26, 20, 51, "M → ddr3_top", (
        "s_axi 128b",
        "28b 地址",
        "ID 全 0",
        "单笔不跨 4KB",
    ), kind="clk", tfs=8.2, bfs=6.8)
    for _, y in chans:
        arrow(ax1, (68, y + 3.75), (74, y + 3.75), mut=8)

    ax1.text(50, 84,
             "Sys_Clk = mem_clk 100 MHz · Cfg_Arb_Type = 0 · Cfg_Cnl_Mask = 4'b1111",
             fontsize=7.4, ha="center", color="#555")

    box(ax1, 3, 3, 93, 17, "Axi_Mux 内部子模块（官方 IP，按通道分工）", (
        "Axi_Mux_Addr_Ctrl ×2：AW / AR 的通道切换状态机",
        "Axi_Mux_Wr_Ctrl（W/B）+ Axi_Mux_Rd_Ctrl（R）：通道锚定与切换",
        "Info_Fifo_Axi_Mux ×3 + Axi_Mux_Info ×3：把通道号沿 AW→W→B、AR→R 传下去",
    ), kind="ext", tfs=8.0, bfs=6.6)

    # ---------------- (b) 时钟 / 复位 ----------------
    ax2.text(50, 92, "(b) 三个时钟域与复位同步", ha="center",
             fontsize=11.5, fontweight="bold", color="#333")

    box(ax2, 2, 76, 18, 9, "25 MHz 晶振", (), kind="io", tfs=8.2)
    box(ax2, 26, 66, 70, 20, "3 个 PLL（Efinity PLL）", (
        "pll_inst1 → core_clk 300 MHz（CPU / GPU acc）",
        "          → core_clk_feedback 100 MHz（DDR / AXI / 显示）",
        "DDR3_PLL  → sdram_clk 400 MHz、cal 时钟",
        "pll_HDMI  → clk_HDMI_x1 74.25 MHz、x5 371.25 MHz",
    ), kind="clk", tfs=8.8, bfs=6.8)
    arrow(ax2, (20, 80), (26, 80), mut=7)

    box(ax2, 3, 50, 94, 11, "sys_rst", (
        "= pll_locked & user_pll_locked（异步断言）　→ APB0 寄存器格子（300 MHz 域）",
    ), kind="reg", tfs=8.6, bfs=6.8)
    box(ax2, 3, 34, 94, 11, "ResetSync @user_clk → axi_rst_n", (
        "= sys_rst & cal_done 的同步释放（300 MHz）　→ 命令域 / 加速器 / 显示配置侧",
    ), kind="reg", tfs=8.6, bfs=6.8)
    box(ax2, 3, 18, 94, 11, "ResetSync @mem_clk → mem_rst_n", (
        "同一异步源，100 MHz 本地释放　→ Axi_Mux / DDR3 / AXIMastersHandler / lineBuffer 写侧",
    ), kind="reg", tfs=8.6, bfs=6.8)
    box(ax2, 3, 3, 94, 11, "rst_hdmi_n → ResetSync @mem_clk → mem_display_rst_n", (
        "= axi_rst_n & pll_HDMI_locked（GPU 内再同步一次）→ 只给显示通道",
    ), kind="reg", tfs=8.2, bfs=6.4)

    arrow(ax2, (50, 50), (50, 45), mut=7)
    arrow(ax2, (50, 34), (50, 29), mut=7)
    arrow(ax2, (50, 18), (50, 14), mut=7)

    fig.savefig(os.path.join(HERE, "图4_总线与时钟复位.png"), dpi=150, facecolor="white")
    plt.close(fig)


# ============================================================================
# 图 5 —— APB0 子系统
# ============================================================================
def fig5():
    fig, ax = canvas(162, 66)
    ax.text(81, 63, "图 5  APB0 子系统（地址译码 / 外设总线层 / 功能层）",
            ha="center", va="center", fontsize=13.5, fontweight="bold", color="#222")

    box(ax, 2, 30, 24, 13, "SoC io_apbSlave_0", ("APB3 · 0xf810_0000 · 300 MHz",), kind="soc", tfs=7.8, bfs=6.2)

    group(ax, 30, 14, 76, 44, "APB0_Interconnect", ec="#C07A22")
    box(ax, 33, 40, 28, 14, "APB0_Decoder", ("PADDR[15:12] → 槽选", "未命中 → PSLVERROR"), kind="bus", tfs=8.4, bfs=6.6)

    box(ax, 66, 44, 36, 11, "槽 0   0x0000", ("KEY_DATA / IRQ_EN / KEY_ID",), kind="cmd", tfs=7.8, bfs=6.2)
    box(ax, 66, 30, 36, 11, "槽 1   0x1000（空）", ("访问它 → PSLVERROR = 1",), kind="io", tfs=7.8, bfs=6.2)
    box(ax, 66, 16, 36, 11, "槽 2   0x2000  GPU_2D", ("0x2000 观测 / 0x2040 显示",), kind="cmd", tfs=7.8, bfs=6.2)

    arrow(ax, (26, 36), (33, 44), mut=7)
    for y in (49.5, 35.5, 21.5):
        arrow(ax, (61, 47), (66, y), mut=7)

    box(ax, 110, 44, 24, 11, "APB0_key", (), kind="cmd", tfs=7.8)
    box(ax, 138, 44, 22, 11, "key", ("消抖 + 边沿",), kind="cmd", tfs=7.8, bfs=6.4)
    arrow(ax, (102, 49.5), (110, 49.5), mut=7)
    arrow(ax, (134, 49.5), (138, 49.5), mut=7)

    box(ax, 110, 16, 24, 11, "APB0_gpu2d", (), kind="cmd", tfs=7.8)
    box(ax, 138, 14, 22, 15, "GPU_2D 功能侧", ("GPU2D_APBHandler",), kind="proc", tfs=7.2, bfs=6.0)
    arrow(ax, (102, 21.5), (110, 21.5), mut=7)
    arrow(ax, (134, 21.5), (138, 21.5), mut=7)

    poly(ax, [(84, 16), (84, 9), (14, 9), (14, 30)], color="#C07A22", ls=(0, (4, 2)))
    lbl(ax, 47, 10.6, "回程三选一：PRDATA / PREADY / PSLVERROR", fs=6.8, color="#C07A22")

    ax.text(2, 4,
            "槽 2 内用 PADDR[6] 分子块：观测与显示寄存器同一个基址 0xF8102000，软件不用记两个外设。",
            fontsize=6.6, ha="left", va="center", color="#555")

    fig.savefig(os.path.join(HERE, "图5_APB0子系统.png"), dpi=150, facecolor="white")
    plt.close(fig)


if __name__ == "__main__":
    fig1()
    fig2()
    fig3()
    fig4()
    fig5()
    print("done:", HERE)
