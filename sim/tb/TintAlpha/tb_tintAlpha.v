//==============================================================================
//  tb_tintAlpha.v —— GPU2D_MODE_TINT_ALPHA（MODE=4）整链逐字节验证
//------------------------------------------------------------------------------
//  测什么
//      一条 MODE=4 的 3-CI 命令走完整条链（★ 和 CI_gpu2d.c 的位序一致）：
//          CI#1 BLIT {w,h} + 目的地址
//          CI#2 ARGS{base 基址} + {more=1, REPLACE|SPRITE[|CKEN|MX], base 跨距}
//          CI#3 ARGS{ARGB}      + {more=0, TINT_ALPHA|SPRITE|CKEN, tone/strength}
//      → CIUnpack → CmdExec（每行一个任务）
//      → PaletteSource（base 那一趟）→ imageCombine（tint）→ alphaBlending（背景 = DDR 读回）
//      → DDRPacker → WriteController → DDR 模型
//      最后把 DDR 里的字节和【本 TB 自己算的参考值】逐字节比。
//
//  为什么参考值要重算
//      一条 MODE=4 的结果是三段算术：
//          ① base 那一格：素材索引 → 调色板 (A,R,G,B) + ck 掩码
//          ② imageCombine：tint 作用在 base 的每个字节上（tone 1/2/3/4）
//          ③ alphaBlending：out = 背景 + (tinted − 背景) × base_α / 255
//      只比"跑两遍一致"查不出算错，必须逐字节比【绝对颜色】。
//
//  重点钉住的契约（★ 都是这条命令独有、最容易漏的）
//      A) CmdExec 对【纯色层那一格】不做行递推 —— 颜色是颜色，不是地址。
//         所以第 0..h-1 行的 line_src_addr_last 必须【逐行完全等于 argb】。
//         （MODE=3 早就这么要求了；MODE=4 是后加的纯色层，必须同样对待。）
//      B) tone[2] 走 param[23]，而 param[23] 同时被 CIUnpack 塞进 flags[7]（ROT90）。
//         对纯色层这一格，flags[7] 必须【不】被当成旋转位：tone=4 时颜色不许被改。
//      C) 底图 α（0/128/255）必须完整保留，tint 不碰背景。
//      D) 纯色层自己不带掩码：写不写只由 base 的 ck 掩码 + α 决定。
//         白色像素（键色）与 α=0 的像素必须保持背景原值。
//
//  素材（本目录 palette.hex / assets.hex）
//      base 图 4 像素/行、行跨距 4 字，行 0..4：
//          row0 idx 1(a255) 2(a128) 0(a0) 5(白 a128, ck 命中)
//          row1 idx 3(a0)   4(a255) 6(a255) 7(a64)
//          row2 idx 7 6 5 4
//          row3 idx 2 1 0 3
//          row4 idx 4 4 6 6
//      调色板 0..7 见 palette.hex 前 8 行；键色 = 白 24'hFFFFFF。
//
//  跑法：sim/tb/run_tb.ps1 -TbDir TintAlpha -TbTop tb_tintAlpha
//==============================================================================
`timescale 1ns/1ps

module tb_tintAlpha #(parameter ASYNC_MEMORY_TEST = 0);

localparam real CLK_HALF = ASYNC_MEMORY_TEST ? (5.0/3.0) : 5.0;
localparam TRACE    = 1'b0;      // 1 = 打印每条行任务（查 CmdExec 契约时打开）

localparam [31:0] STRIDE = 32'd5760;
localparam [31:0] FBASE  = 32'h0880_0000;

//   素材在 PRAM 里的位置（和本目录 assets.hex 一致）
localparam [31:0] BASE_OFF    = 32'd0;
localparam [31:0] BASE_STRIDE = 32'd4;      // 字/行 = 16 像素/行

//   CI 功能号 / flags 位
localparam [9:0] FID_BLIT = 10'h002;
localparam [9:0] FID_ARGS = 10'h007;

localparam [2:0] MODE_REPLACE    = 3'd1;
localparam [2:0] MODE_TINT_ALPHA = 3'd4;
localparam [7:0] FL_SPRITE = 8'd8, FL_CKEN = 8'd16, FL_MX = 8'd32;

localparam [23:0] KEY_COLOR = 24'hFFFFFF;   // 和 DUT 的 ckey_color 一致

reg          clk, rst_n;
reg          mem_clk = 0;
wire         bus_clk;
always #5 mem_clk = ~mem_clk;
assign bus_clk = ASYNC_MEMORY_TEST ? mem_clk : clk;
integer      err_cnt, n_case;
integer      k, i, j, c;
integer      done_cnt, done_base;

//---- CI 口 ----
reg  [9:0]   ci_function_id;
reg  [31:0]  ci_inputs_0, ci_inputs_1;
reg          ci_cmd_valid;
wire         ci_cmd_ready, ci_rsp_valid;
reg          ci_rsp_ready;
wire [31:0]  ci_outputs_0;

//---- AXI ----
wire [7:0]   m_AXI_AWID;
wire [31:0]  m_AXI_AWADDR;
wire [7:0]   m_AXI_AWLEN;
wire [2:0]   m_AXI_AWSIZE;
wire [1:0]   m_AXI_AWBURST;
wire         m_AXI_AWLOCK, m_AXI_AWVALID;
wire [127:0] m_AXI_WDATA;
wire [15:0]  m_AXI_WSTRB;
wire         m_AXI_WLAST, m_AXI_WVALID;
wire [7:0]   m_AXI_BID;
wire [1:0]   m_AXI_BRESP;
reg          m_AXI_BVALID;
wire         m_AXI_BREADY;
wire [7:0]   m_AXI_ARID;
wire [31:0]  m_AXI_ARADDR;
wire [7:0]   m_AXI_ARLEN;
wire [2:0]   m_AXI_ARSIZE;
wire [1:0]   m_AXI_ARBURST;
wire         m_AXI_ARLOCK, m_AXI_ARVALID;
wire [7:0]   m_AXI_RID;
wire [127:0] m_AXI_RDATA;
wire [1:0]   m_AXI_RRESP;
wire         m_AXI_RLAST;
reg          m_AXI_RVALID;
wire         m_AXI_RREADY;
reg          m_AXI_AWREADY, m_AXI_WREADY, m_AXI_ARREADY;

initial begin clk = 1'b0; forever #(CLK_HALF) clk = ~clk; end

//==============================================================================
// 被测：真 GPU_2D（CIUnpack + CmdExec + 两路源 + 合成 + 写侧全在里面）
//==============================================================================
GPU_2D #(
    .FIFO_AW            (9),
    .LINE_BYTES         (5760),
    .AXI_ADDR_WIDTH     (32),
    .AXI_DATA_WIDTH     (128),
    .AXI_STRB_WIDTH     (16),
    .AXI_ID_WIDTH       (8),
    .AXI_LEN_WIDTH      (8),
    .AXI_ID             (8'h00),
    .ASYNC_MEMORY       (ASYNC_MEMORY_TEST != 0)
) u_dut (
    .clk_ci             (clk),
    .rst_ci_n           (rst_n),
    .clk_acc            (clk),
    .rst_acc_n          (rst_n),
    .clk_mem            (bus_clk),
    .rst_mem_n          (rst_n),
    .ci_cmd_valid       (ci_cmd_valid),
    .ci_cmd_ready       (ci_cmd_ready),
    .ci_function_id     (ci_function_id),
    .ci_inputs_0        (ci_inputs_0),
    .ci_inputs_1        (ci_inputs_1),
    .ci_rsp_valid       (ci_rsp_valid),
    .ci_rsp_ready       (ci_rsp_ready),
    .ci_outputs_0       (ci_outputs_0),

    .clk_HDMI_x1                  (clk),
    .rst_hdmi_n                   (rst_n),
    .enable                       (1'b0),
    .split_pending                (1'b0),
    .framea_base_addr             (64'd0),
    .frameb_base_addr             (64'd0),
    .frame_full_base_addr         (64'd0),
    .done_gpu2d                   (1'b0),
    //   ★ 键色故意给【白】：素材里有白色像素，用来钉"ck 开时白必须不写"
    .ckey_color                   (KEY_COLOR),
    .done_cpu                     (1'b0),
    .acc_frame_done               (),
    .done_clr                     (),
    .fps_acc_val                  (),
    .fps_cpu_val                  (),
    .busy_val                     (),
    .cmd_done_val                 (),
    .frame_done_val               (),
    .wr_beats_val                 (),
    .rd_beats_val                 (),
    .busy_cyc_val                 (),

    .lineBuffer_m_AXI_AWREADY     (1'b0),
    .lineBuffer_m_AXI_WREADY      (1'b0),
    .lineBuffer_m_AXI_BID         (8'd0),
    .lineBuffer_m_AXI_BRESP       (2'd0),
    .lineBuffer_m_AXI_BVALID      (1'b0),
    .lineBuffer_m_AXI_ARREADY     (1'b0),
    .lineBuffer_m_AXI_RID         (8'd0),
    .lineBuffer_m_AXI_RDATA       (128'd0),
    .lineBuffer_m_AXI_RRESP       (2'd0),
    .lineBuffer_m_AXI_RLAST       (1'b0),
    .lineBuffer_m_AXI_RVALID      (1'b0),

    .m_AXI_AWID         (m_AXI_AWID),
    .m_AXI_AWADDR       (m_AXI_AWADDR),
    .m_AXI_AWLEN        (m_AXI_AWLEN),
    .m_AXI_AWSIZE       (m_AXI_AWSIZE),
    .m_AXI_AWBURST      (m_AXI_AWBURST),
    .m_AXI_AWLOCK       (m_AXI_AWLOCK),
    .m_AXI_AWCACHE      (),
    .m_AXI_AWPROT       (),
    .m_AXI_AWQOS        (),
    .m_AXI_AWREGION     (),
    .m_AXI_AWVALID      (m_AXI_AWVALID),
    .m_AXI_AWREADY      (m_AXI_AWREADY),
    .m_AXI_WDATA        (m_AXI_WDATA),
    .m_AXI_WSTRB        (m_AXI_WSTRB),
    .m_AXI_WLAST        (m_AXI_WLAST),
    .m_AXI_WVALID       (m_AXI_WVALID),
    .m_AXI_WREADY       (m_AXI_WREADY),
    .m_AXI_BID          (m_AXI_BID),
    .m_AXI_BRESP        (m_AXI_BRESP),
    .m_AXI_BVALID       (m_AXI_BVALID),
    .m_AXI_BREADY       (m_AXI_BREADY),
    .m_AXI_ARID         (m_AXI_ARID),
    .m_AXI_ARADDR       (m_AXI_ARADDR),
    .m_AXI_ARLEN        (m_AXI_ARLEN),
    .m_AXI_ARSIZE       (m_AXI_ARSIZE),
    .m_AXI_ARBURST      (m_AXI_ARBURST),
    .m_AXI_ARLOCK       (m_AXI_ARLOCK),
    .m_AXI_ARCACHE      (),
    .m_AXI_ARPROT       (),
    .m_AXI_ARQOS        (),
    .m_AXI_ARREGION     (),
    .m_AXI_ARVALID      (m_AXI_ARVALID),
    .m_AXI_ARREADY      (m_AXI_ARREADY),
    .m_AXI_RID          (m_AXI_RID),
    .m_AXI_RDATA        (m_AXI_RDATA),
    .m_AXI_RRESP        (m_AXI_RRESP),
    .m_AXI_RLAST        (m_AXI_RLAST),
    .m_AXI_RVALID       (m_AXI_RVALID),
    .m_AXI_RREADY       (m_AXI_RREADY)
);

//==============================================================================
// DDR 模型：写（AW/W/B）+ 读（AR/R）—— MODE=4 要读回目的当背景
//==============================================================================
reg [127:0] mem [0:1048575];
reg [31:0]  wr_addr_r;
reg [8:0]   wr_left_r;
reg         wr_run_r, b_pend_r;
integer     wb;

reg [31:0]  rd_addr_r;
reg [8:0]   rd_left_r;
reg         rd_run_r;
integer     rd_lat_r;

initial begin
    m_AXI_AWREADY = 1'b0; m_AXI_WREADY = 1'b0; m_AXI_ARREADY = 1'b0;
    m_AXI_BVALID = 1'b0;  m_AXI_RVALID = 1'b0;
    wr_run_r = 1'b0; b_pend_r = 1'b0;
    rd_run_r = 1'b0; rd_lat_r = 0;
    for (i = 0; i < 1048576; i = i + 1) mem[i] = 128'h0;
end

always @(posedge bus_clk or negedge rst_n) begin
    if (!rst_n) begin
        m_AXI_AWREADY <= 1'b0; m_AXI_WREADY <= 1'b0;
        m_AXI_BVALID  <= 1'b0; wr_run_r <= 1'b0; b_pend_r <= 1'b0;
    end
    else begin
        m_AXI_AWREADY <= ~wr_run_r;
        if (m_AXI_AWVALID & m_AXI_AWREADY) begin
            wr_addr_r <= m_AXI_AWADDR;
            wr_left_r <= m_AXI_AWLEN + 9'd1;
            wr_run_r  <= 1'b1;
        end
        m_AXI_WREADY <= wr_run_r;
        if (m_AXI_WVALID & m_AXI_WREADY) begin
            for (wb = 0; wb < 16; wb = wb + 1)
                if (m_AXI_WSTRB[wb]) mem[wr_addr_r[23:4]][wb*8 +: 8] <= m_AXI_WDATA[wb*8 +: 8];
            wr_addr_r <= wr_addr_r + 32'd16;
            wr_left_r <= wr_left_r - 9'd1;
            if (m_AXI_WLAST) begin wr_run_r <= 1'b0; b_pend_r <= 1'b1; end
        end
        if (b_pend_r) m_AXI_BVALID <= 1'b1;
        if (m_AXI_BVALID & m_AXI_BREADY) begin m_AXI_BVALID <= 1'b0; b_pend_r <= 1'b0; end
    end
end

//   读：一次受理一笔，固定 3 拍延迟，每拍 16 字节
always @(posedge bus_clk or negedge rst_n) begin
    if (!rst_n) begin
        m_AXI_ARREADY <= 1'b0; m_AXI_RVALID <= 1'b0;
        rd_run_r <= 1'b0; rd_lat_r <= 0;
    end
    else begin
        m_AXI_ARREADY <= ~rd_run_r;
        if (m_AXI_ARVALID & m_AXI_ARREADY) begin
            rd_addr_r <= m_AXI_ARADDR;
            rd_left_r <= m_AXI_ARLEN + 9'd1;
            rd_run_r  <= 1'b1;
            rd_lat_r  <= 3;
        end
        if (m_AXI_RVALID & m_AXI_RREADY) begin
            rd_addr_r <= rd_addr_r + 32'd16;
            rd_left_r <= rd_left_r - 9'd1;
            if (rd_left_r == 9'd1) begin
                m_AXI_RVALID <= 1'b0; rd_run_r <= 1'b0;
            end
        end
        if (rd_run_r & ~m_AXI_RVALID) begin
            if (rd_lat_r != 0) rd_lat_r <= rd_lat_r - 1;
            else begin
                m_AXI_RVALID <= 1'b1;
            end
        end
    end
end
assign m_AXI_RDATA = mem[rd_addr_r[23:4]];
assign m_AXI_RLAST = m_AXI_RVALID & (rd_left_r == 9'd1);

always @(posedge clk) if (rst_n & u_dut.line_done) done_cnt = done_cnt + 1;

//==============================================================================
// 每行任务的契约检查（★ 不依赖像素数学，直接钉 CmdExec 的广播）
//==============================================================================
reg             e_run;
integer         e_row, e_h, e_w, e_dst, e_tone, e_str, e_mx;
reg  [31:0]     e_argb, e_base_off, e_base_stride;
integer         mon_err;
integer         e_source_row;
reg [31:0]      e_expected_src;

always @(posedge clk) begin
    if (rst_n && e_run && u_dut.line_start) begin
        if (TRACE)
            $display("  [LINE] row%0d flags0=%h src0=%h last_src=%h last_flags=%h last_param=%h",
                     e_row, u_dut.line_flags[7:0], u_dut.line_src_addr[31:0],
                     u_dut.line_src_addr_last, u_dut.line_flags_last, u_dut.line_param_last);
        //   A) 颜色逐行不变（★ MODE=4 的核心契约）
        if (u_dut.line_src_addr_last !== e_argb) begin
            mon_err = mon_err + 1;
            if (mon_err <= 8)
                $display("  [MON-ERR] row%0d line_src_addr_last=%h exp color %h (颜色被当成地址递推了!)",
                         e_row, u_dut.line_src_addr_last, e_argb);
        end
        //   B) 合成器要认得出纯色层 + tone/strength 原样到最后一格
        if (u_dut.line_fx_en !== 1'b1) begin
            mon_err = mon_err + 1;
            if (mon_err <= 8) $display("  [MON-ERR] row%0d line_fx_en=%b exp 1", e_row, u_dut.line_fx_en);
        end
        if (u_dut.line_flags_last[2:0] !== MODE_TINT_ALPHA) begin
            mon_err = mon_err + 1;
            if (mon_err <= 8)
                $display("  [MON-ERR] row%0d flags_last[2:0]=%0d exp 4", e_row, u_dut.line_flags_last[2:0]);
        end
        if (u_dut.line_flags_last[4] !== 1'b1) begin
            mon_err = mon_err + 1;
            if (mon_err <= 8) $display("  [MON-ERR] row%0d flags_last[4]=0 (ck 必须开)", e_row);
        end
        if (u_dut.line_param_last[23:21] !== e_tone[2:0]) begin
            mon_err = mon_err + 1;
            if (mon_err <= 8)
                $display("  [MON-ERR] row%0d tone=%0d exp %0d", e_row, u_dut.line_param_last[23:21], e_tone);
        end
        if (u_dut.line_param_last[19:12] !== e_str[7:0]) begin
            mon_err = mon_err + 1;
            if (mon_err <= 8)
                $display("  [MON-ERR] row%0d strength=%0d exp %0d", e_row, u_dut.line_param_last[19:12], e_str);
        end
        //   C) 底图那一格：源地址每行 += 跨距（MX 时广播行尾字，w=4 时就是行首），目的 += 5760
        e_source_row = e_mx[1] ? (e_h - 1 - e_row) : e_row;
        if (e_mx[2]) begin
            e_expected_src = e_base_off + (e_source_row >> 2);
            e_expected_src[31:30] = e_source_row & 3;
        end else begin
            e_expected_src = e_base_off + e_source_row * e_base_stride
                           + (e_mx[0] ? ((e_w + 3) / 4 - 1) : 0);
        end
        if (u_dut.line_src_addr[31:0] !== e_expected_src) begin
            mon_err = mon_err + 1;
            if (mon_err <= 8)
                $display("  [MON-ERR] row%0d base_src=%h exp %h", e_row, u_dut.line_src_addr[31:0],
                         e_expected_src);
        end
        if (u_dut.line_dst_addr !== (e_dst + e_row * STRIDE)) begin
            mon_err = mon_err + 1;
            if (mon_err <= 8)
                $display("  [MON-ERR] row%0d dst=%h exp %h", e_row, u_dut.line_dst_addr, e_dst + e_row * STRIDE);
        end
        e_row = e_row + 1;
    end
end

//   调试：MX 那一行的第一拍调色板数据（只在 TRACE 时打，最多 6 拍）
integer dbg_pal_cnt;
always @(posedge clk) begin
    if (TRACE && rst_n && u_dut.src1_valid && u_dut.line_flags[5] && dbg_pal_cnt < 6) begin
        $display("  [PAL] data=%h mask=%h alpha=%h", u_dut.src1_data, u_dut.src1_mask, u_dut.src1_alpha);
        dbg_pal_cnt = dbg_pal_cnt + 1;
    end
end

//==============================================================================
// CI 发送（和 C 驱动 CI_gpu2d.c 的位序完全一致）
//==============================================================================
task ci_send;
    input [9:0]  fid;
    input [31:0] in0, in1;
    begin
        @(negedge clk);
        ci_function_id = fid; ci_inputs_0 = in0; ci_inputs_1 = in1;
        ci_cmd_valid = 1'b1;
        while (!ci_cmd_ready) @(negedge clk);
        @(negedge clk);
        ci_cmd_valid = 1'b0;
    end
endtask

//==============================================================================
// 参考模型：素材 + 调色板
//==============================================================================
reg [31:0] pal  [0:255];
reg [31:0] ramw [0:32767];
reg [7:0]  bg_save [0:4095];

function [7:0] asset_idx;               // 输出像素 (x,y) 取素材哪一项（含 MX）
    input [31:0] off, stride;
    input integer mx, w, h, x, y;
    integer ax, ay, swap_axis;
    reg [31:0] wd;
    begin
        ax = mx[0] ? (w - 1 - x) : x;
        ay = mx[1] ? (h - 1 - y) : y;
        if (mx[2]) begin swap_axis = ax; ax = ay; ay = swap_axis; end
        wd = ramw[off + ay * stride + (ax >> 2)];
        asset_idx = wd[(ax % 4) * 8 +: 8];
    end
endfunction

function [7:0] peek_byte;
    input [31:0] a;
    begin
        peek_byte = mem[a[23:4]][{a[3:0], 3'b000} +: 8];
    end
endfunction

//   /255（和 alphaBlending.v 完全一样的精确式）
function integer div255;
    input integer p;
    begin
        div255 = (p + 1 + (p >> 8)) >> 8;
    end
endfunction

function [7:0] blend_ch;
    input [7:0] fg, bg, al;
    integer p, q, d;
    begin
        d = fg - bg;
        if (d < 0) begin
            q = div255((-d) * al);
            blend_ch = bg - q;
        end
        else begin
            q = div255(d * al);
            blend_ch = bg + q;
        end
    end
endfunction

//   imageCombine.v 的逐字节 tint（★ 和 RTL 同一份整数算法）
function [7:0] tint_ch;
    input [7:0] p, cc, k;
    input integer tone;
    integer d, prod, q8;
    begin
        d = (cc >= p) ? (cc - p) : (p - cc);
        case (tone)
            1: prod = d  * k;      // 插值
            2: prod = cc * k;      // 加光
            3: prod = p  * k;      // 乘系数
            4: prod = p  * cc;     // 正片叠底
            default: prod = 0;
        endcase
        q8 = (prod >> 8) & 255;    // RTL: mul_a*mul_b 先落 16 位再取 [15:8]
        case (tone)
            1: tint_ch = (cc >= p) ? (p + q8) : (p - q8);
            2: tint_ch = ((p + q8) > 255) ? 8'hFF : (p + q8);
            3: tint_ch = q8;
            4: tint_ch = q8;
            default: tint_ch = p;  // tone=0：纯色层不生效，base 原样
        endcase
    end
endfunction

//==============================================================================
// 用例任务
//==============================================================================
integer ux, uy, uc, bad_cnt, exp_val, got_val;
integer tmo;

task fill_bg;
    input [31:0] dst;
    input integer w, h;
    integer ii, rr;
    begin
        for (rr = 0; rr < h; rr = rr + 1)
        for (ii = 0; ii < w * 3; ii = ii + 1) begin
            mem[((dst + rr*STRIDE + ii) >> 4) & 32'h000F_FFFF][((dst + rr*STRIDE + ii) & 15)*8 +: 8]
                = 8'h40 + (ii & 8'h3F);
            if (rr == 0) bg_save[ii] = 8'h40 + (ii & 8'h3F);
        end
    end
endtask

task check_sentinel;
    input [31:0] dst;
    input integer w;
    input [8*24-1:0] tag;
    begin
        if (peek_byte(dst - 1) !== 8'h00) begin
            $display("FAIL [%0s]: 块前一个字节被写了 = %02x", tag, peek_byte(dst-1));
            err_cnt = err_cnt + 1;
        end
        if (peek_byte(dst + w*3) !== 8'h00) begin
            $display("FAIL [%0s]: 块后一个字节被写了 = %02x", tag, peek_byte(dst+w*3));
            err_cnt = err_cnt + 1;
        end
    end
endtask

//   跑一条 MODE=4 用例
task run_tint;
    input [31:0] dst;
    input integer w, h;
    input [31:0] base_off, base_stride;
    input [31:0] argb;
    input integer strength, tone, mx;
    input [8*24-1:0] tag;
    reg [7:0]  base_flags, tint_flags;
    reg [23:0] base_param, tint_param;
    integer    ii;
    begin
        n_case = n_case + 1;
        bad_cnt = 0;
        mon_err = 0;

        fill_bg(dst, w, h);
        done_base = done_cnt;

        //   ★ 和 CI_gpu2d_spriteAlphaTint() 一模一样：
        //      base: REPLACE|SPRITE[|CKEN|MX]，param = 跨距
        //      tint: TINT_ALPHA|SPRITE|CKEN，param = {tone[2:0], strength[7:0]}
        base_flags = MODE_REPLACE | FL_SPRITE | (mx[0] ? FL_MX : 8'd0)
                   | (mx[1] ? 8'h40 : 8'd0);
        base_param = base_stride[23:0] | (mx[2] ? 24'h800000 : 24'd0);
        tint_flags = MODE_TINT_ALPHA | FL_SPRITE | FL_CKEN;
        tint_param = ((tone & 7) << 21) | ((strength & 255) << 12);

        e_run = 1'b1; e_row = 0; e_h = h; e_w = w; e_dst = dst;
        e_argb = argb; e_tone = tone; e_str = strength; e_mx = mx;
        e_base_off = base_off; e_base_stride = base_stride;

        //   ★ 命令开始那条 CI 带 {w,h}（不是源地址）；源地址在每张图自己的 ARGS 里。
        ci_send(FID_BLIT, {w[15:0], h[15:0]}, dst);
        ci_send(FID_ARGS, base_off, {1'b1, base_flags[6:0], base_param});
        ci_send(FID_ARGS, argb,     {1'b0, tint_flags[6:0], tint_param});

        //   等 h 个 line_done
        tmo = 0;
        while ((done_cnt - done_base) < h && tmo < 400000) begin @(posedge clk); tmo = tmo + 1; end
        if ((done_cnt - done_base) != h) begin
            $display("FAIL [%0s]: line_done 数 = %0d，期望 %0d（卡住了）", tag, done_cnt - done_base, h);
            err_cnt = err_cnt + 1;
            e_run = 1'b0;
            //   卡住后整条链不会自己恢复：直接给结论，别让后面的用例假装通过
            $display("SUMMARY: tb_tintAlpha FAIL (stuck at %0s)", tag);
            $finish;
        end
        e_run = 1'b0;
        $display("PERF tint=%0s core_cycles=%0d rows=%0d width=%0d async=%0d",
                 tag, tmo, h, w, ASYNC_MEMORY_TEST);

        if (e_row != h) begin
            $display("FAIL [%0s]: 行任务数 = %0d，期望 %0d", tag, e_row, h);
            err_cnt = err_cnt + 1;
        end
        if (mon_err != 0) begin
            $display("FAIL [%0s]: CmdExec 行任务契约错误 %0d 处", tag, mon_err);
            err_cnt = err_cnt + mon_err;
        end

        repeat (50) @(posedge clk);

        //   逐字节比对（★ 绝对颜色，不是"跑两遍一致"）
        for (uy = 0; uy < h; uy = uy + 1)
            for (ux = 0; ux < w; ux = ux + 1)
                for (uc = 0; uc < 3; uc = uc + 1) begin
                    begin : calc
                        reg [31:0] pentry;
                        reg [7:0]  base_b, cc, fg, al, ckm, bgorig;
                        pentry = pal[asset_idx(base_off, base_stride, mx, w, h, ux, uy)];
                        base_b = pentry[uc*8 +: 8];
                        al     = pentry[31:24];
                        cc     = argb[uc*8 +: 8];
                        fg     = tint_ch(base_b, cc, strength[7:0], tone);
                        ckm    = (pentry[23:0] == KEY_COLOR) ? 8'd0 : 8'd1;
                        bgorig = bg_save[ux*3 + uc];
                        //   MODE=4：命令级 ck 取自 tint 那一格（恒 1）⇒ base 的 ck 掩码生效；
                        //   α=0 或 ck 命中 ⇒ 不写，保持背景原值。
                        if ((al == 8'd0) || (ckm == 8'd0)) exp_val = bgorig;
                        else                              exp_val = blend_ch(fg, bgorig, al);
                    end
                    got_val = peek_byte(dst + uy*STRIDE + ux*3 + uc);
                    if (got_val !== exp_val) begin
                        bad_cnt = bad_cnt + 1;
                        if (bad_cnt <= 8)
                            $display("  [ERR] %0s px(%0d,%0d) ch%0d: got %02x exp %02x",
                                     tag, ux, uy, uc, got_val, exp_val);
                    end
                end

        check_sentinel(dst, w, tag);

        if (bad_cnt == 0) $display("  [ok] %0s : tone=%0d str=%0d mx=%0d : %0d 行 x %0d 像素 x 3 字节全对",
                                   tag, tone, strength, mx, h, w);
        else begin
            $display("FAIL [%0s]: %0d 个字节不对", tag, bad_cnt);
            err_cnt = err_cnt + bad_cnt;
        end
    end
endtask

//==============================================================================
// 主流程
//==============================================================================
initial begin
    err_cnt = 0; n_case = 0; done_cnt = 0; done_base = 0;
    e_run = 1'b0; e_row = 0; e_h = 0; e_w = 0; e_dst = 0; e_tone = 0; e_str = 0; e_mx = 0;
    e_argb = 0; e_base_off = 0; e_base_stride = 0; mon_err = 0;
    dbg_pal_cnt = 0;
    ci_cmd_valid = 1'b0; ci_function_id = 10'd0; ci_inputs_0 = 32'd0; ci_inputs_1 = 32'd0;
    ci_rsp_ready = 1'b1;      // ★ 必须收响应：欠响应计数到顶就不再收 CI 了

    $readmemh("palette.hex", pal);
    $readmemh("assets.hex",  ramw);

    rst_n = 1'b0;
    repeat (20) @(posedge clk);
    rst_n = 1'b1;
    repeat (20) @(posedge clk);

    $display("==== tb_tintAlpha: MODE=4 (tint + alpha) 整链逐字节验证 ====");
    $display("     base: PRAM sprite 4px x 5row @ word0 stride4; key color = white");

    //   ---- 用例1: tone=1 插值染色，h=2（第 2 行就能抓到"颜色被递推"）----
    $display("---- 1) tone=1 插值染色（h=2）----");
    run_tint(FBASE + 10*STRIDE, 4, 2, BASE_OFF, BASE_STRIDE,
             32'h00FF8000, 128, 1, 0, "tone1-h2");

    //   ---- 用例2: tone=2 加光（饱和）----
    $display("---- 2) tone=2 加光（h=2）----");
    run_tint(FBASE + 20*STRIDE, 4, 2, BASE_OFF, BASE_STRIDE,
             32'h0000FF80, 160, 2, 0, "tone2-h2");

    //   ---- 用例3: tone=3 乘系数 ----
    $display("---- 3) tone=3 乘系数（h=2）----");
    run_tint(FBASE + 30*STRIDE, 4, 2, BASE_OFF, BASE_STRIDE,
             32'h00FFFFFF, 96, 3, 0, "tone3-h2");

    //   ---- 用例4: tone=4 正片叠底 + h=5 ----
    //      ★ tone=4 让 param[23]=1，CIUnpack 会把它同时放进 flags[7]（ROT90）。
    //        纯色层这一格必须【不】解释 flags[7]；h=5 时 rot 起点偏移 = (h-1)>>2 = 1，
    //        一旦被当成旋转位，第 0 行颜色就会 +1。
    $display("---- 4) tone=4 正片叠底（h=5，含 tone[2]↔ROT90 位冲突）----");
    run_tint(FBASE + 40*STRIDE, 4, 5, BASE_OFF, BASE_STRIDE,
             32'h00C06030, 255, 4, 0, "tone4-h5");

    //   ---- 用例5: 底图 MX（水平镜像，w=4 = 4 的倍数）----
    $display("---- 5) tone=1 + 底图 MX（h=2）----");
    run_tint(FBASE + 60*STRIDE, 4, 2, BASE_OFF, BASE_STRIDE,
             32'h00FF8000, 128, 1, 1, "tone1-mx");

    //   ---- 用例6: 目的不对齐（x=100 → dst_off=12）----
    $display("---- 6) tone=2 + dst_off=12（x=100，h=2）----");
    run_tint(FBASE + 70*STRIDE + 100*3, 4, 2, BASE_OFF, BASE_STRIDE,
             32'h0040C0FF, 64, 2, 0, "tone2-dstoff12");

    //   ---- 用例7: tone=0（文档：tint 关 = 回到老行为，base 原样 + alpha）----
    //      ★ 放最后：如果这一档没实现（合成级等不到第二趟数据），会在上面卡住并直接报错。
    $display("---- 7) tone=0（tint 关，应等价 MODE=2）----");
    run_tint(FBASE + 80*STRIDE, 4, 2, BASE_OFF, BASE_STRIDE,
             32'h00FF8000, 128, 0, 0, "tone0");

    // A full synthetic 32x32 base exercises the production 24x25 crop and
    // the 25x24 orientation, including alpha/CK holes and unused tail beats.
    // Seed both the independent reference and the read-only DUT BRAM.
    for (i = 0; i < 256; i = i + 1) begin
        for (j = 0; j < 4; j = j + 1)
            ramw[256+i][j*8 +: 8] = (i*7+j) % 8;
        u_dut.u_PaletteSource.u_PaletteRAM.u_BlockRAM.BUFFER[256+i] = ramw[256+i];
    end
    run_tint(FBASE + 100*STRIDE + 3, 24, 25, 256+4*8+1, 8,
             32'h00FF2020, 170, 1, 0, "bullet24x25-off3");
    run_tint(FBASE + 140*STRIDE + 12, 25, 24, 256+4*8+1, 8,
             32'h00FF2020, 170, 1, 0, "bullet25x24-off12");
    run_tint(FBASE + 180*STRIDE + 3, 25, 24, 256+4*8+1, 8,
             32'h00FF2020, 170, 1, 4, "bullet-rot90");
    run_tint(FBASE + 220*STRIDE + 12, 25, 24, 256+4*8+1, 8,
             32'h00FF2020, 170, 1, 6, "bullet-rot90-my");
    run_tint(FBASE + 260*STRIDE + 3, 25, 24, 256+4*8+1, 8,
             32'h00FF2020, 170, 1, 5, "bullet-rot90-mx");

    $display("---- 结果: %0d 条用例, err_cnt = %0d ----", n_case, err_cnt);
    if (err_cnt == 0) $display("SUMMARY: tb_tintAlpha ALL-PASS");
    else              $display("SUMMARY: tb_tintAlpha FAIL (%0d)", err_cnt);
    $finish;
end

initial begin
    #120_000_000;
    $display("SUMMARY: tb_tintAlpha FAIL (global timeout)");
    $finish;
end

endmodule
