//==============================================================================
//  tb_m3palette.v —— M3 整链回环: CI → 命令 → 调色板素材源 → 合成 → 写回 DDR
//------------------------------------------------------------------------------
//  测的是什么（这是 M3 的"主链路"，第一次把【素材那条路】整条串起来跑）
//
//      CI ─▶ CmdChannel ─▶ CmdExec ──行任务──┬─▶ DDRSource      (本 TB 不用)
//                                            └─▶ PaletteSource ──96b─▶ Compositor
//      PaletteSource ─▶ PaletteDecoder ─▶ PaletteLUT ─▶ 一行像素
//                                              │
//                          [DDR 内存模型] ◀──AW/W/B── WriteController ◀── DDRPacker
//
//  为什么必须有这个 TB
//      Decoder / Source 的单元 TB 是【手工喂行任务】的 —— 命令解码、行任务广播、
//      合成器的 src1 选路、line_src1_free 参与的 line_buf_free 节奏，这几处只有整链才验得到。
//
//  判据（不靠肉眼，逐字节比对）
//      1) 目的区逐字节 == 素材期望（索引→调色板→B,G,R 字节序）
//      2) 块外还是哨兵 0xA5（没写出去）
//      3) 两个相位各画一份（dst_off = 12 和 2），两份都要对 —— 相位错就会露馅
//      4) line_done 数 = 行数；全程没有 AXI 读（素材在片上, 一个字节都不读 DDR）
//
//  跑法(ModelSim)
//    vlog -quiet -work work GPU_2D.v CmdChannel.v CmdExec.v DDRSource.v DDRUnpacker.v \
//         PaletteSource.v PaletteDecoder.v PaletteLUT.v PaletteRAM.v \
//         Factory.v Compositor.v DDRPacker.v WriteController.v AXIMastersHandler.v \
//         AXI4_Master_Read.v AXI4_Master_Write.v BlockRAM.v async_fifo.v async_PingPongBuffer.v \
//         lineBuffer/*.v PingPongBuffer/*.v HDMI/**/*.v  (GPU_2D 里例化了显示通道) \
//         tb_m3palette.v
//    vsim -c -quiet -do "run -all; quit -f" work.tb_m3palette
//
//==============================================================================
`timescale 1ns/100ps

module tb_m3palette;

localparam CLK_HALF = 5.0;
localparam [31:0] STRIDE = 32'd5760;

//   素材参数（和当前 assets_desc.h 一致：第 0 张 63x89，跨距 16 字/行 = 64 像素）
localparam integer AW = 63;             // 素材声明宽度（真实像素数）
localparam integer AH = 89;             // 素材整高
localparam [23:0] STRIDE_W = 24'd16;    // ★ 字, 不是像素（16 字 = 64 像素，行尾有 1 个 padding）
//   ★ 镜像(MX)必须在【4 像素对齐的跨度】里做：本 TB 的 MX 用例取 w=60（4 的倍数），
//     避开行尾那个 padding 像素（它是索引 0 = 白，镜像后会跑到左边）。

//   两个目的位置: x=100 → dst_off=12 ; x=406 → dst_off=2（相位不同）
localparam [31:0] DST_A = 32'h0880_0000 + 300*5760 + 100*3;
localparam [31:0] DST_B = 32'h0880_0000 + 300*5760 + 406*3;
//   镜像用例：换三块不重叠的地方（AH=121 行一块）
localparam [31:0] DST_C = 32'h0880_0000 + 600*5760 + 100*3;
localparam [31:0] DST_D = 32'h0880_0000 + 900*5760 + 100*3;
localparam [31:0] DST_E = 32'h0880_0000 + 1200*5760 + 100*3;

localparam [9:0]  FID_BLIT = 10'h002;
localparam [9:0]  FID_ARGS = 10'h007;
localparam [6:0]  FLAGS_SPRITE = 7'h09;          // REPLACE(1) | SPRITE(8)

reg          clk, rst_n;
integer      err_cnt, n_chk, line_done_cnt, rd_bursts, wr_bursts;
integer      i, j, c;
integer      w_draw = AW;       // 本次绘制宽度（MX 用例会改成 4 的倍数）
reg  [7:0]   got_b, exp_b;

//---- CI 口 ----
reg  [9:0]   ci_function_id;
reg  [31:0]  ci_inputs_0, ci_inputs_1;
reg          ci_cmd_valid;
wire         ci_cmd_ready, ci_rsp_valid;
reg          ci_rsp_ready;
wire [31:0]  ci_outputs_0;

//---- AXI（只写；读侧不接东西, 素材源根本不读 DDR）----
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
// 被测：真的 GPU_2D（整条链都在里面）
//==============================================================================
GPU_2D #(
    .FIFO_AW            (9),
    .LINE_BYTES         (5760),
    .AXI_ADDR_WIDTH     (32),
    .AXI_DATA_WIDTH     (128),
    .AXI_STRB_WIDTH     (16),
    .AXI_ID_WIDTH       (8),
    .AXI_LEN_WIDTH      (8),
    .AXI_ID             (8'h00)
) u_dut (
    .clk_ci             (clk),
    .rst_ci_n           (rst_n),
    .clk_acc            (clk),
    .rst_acc_n          (rst_n),
    .ci_cmd_valid       (ci_cmd_valid),
    .ci_cmd_ready       (ci_cmd_ready),
    .ci_function_id     (ci_function_id),
    .ci_inputs_0        (ci_inputs_0),
    .ci_inputs_1        (ci_inputs_1),
    .ci_rsp_valid       (ci_rsp_valid),
    .ci_rsp_ready       (ci_rsp_ready),
    .ci_outputs_0       (ci_outputs_0),
    // ---- 显示通道 + 两个 APB 寄存器格子(都在 GPU_2D 里)：本 TB 只验加速器那条链 ----
    .clk_HDMI_x1                  (clk),
    .rst_hdmi_n                   (rst_n),

    // 寄存器侧（★ 总线层 APB0_gpu2d 在 APB0_Interconnect 里，本 TB 不例化它）:
    //   显示关掉 / 全屏 / 无交帧 —— 本 TB 只验加速器那条链
    .enable                       (1'b0),
    .split_pending                (1'b0),
    .framea_base_addr             (64'd0),
    .frameb_base_addr             (64'd0),
    .frame_full_base_addr         (64'd0),
    .done_gpu2d                   (1'b0),
    .ckey_color                   (24'hFFFFFF),   // 颜色键控的键色（本 TB 不开 ck）
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
// DDR 内存模型（只做写：AW/W/B；读侧什么都不回 —— 素材源不读 DDR）
//==============================================================================
reg [127:0] mem [0:1048575];
reg [31:0]  wr_addr_r;
reg [8:0]   wr_left_r;
reg         wr_run_r, b_pend_r;
integer     wb;
integer     ar_cnt;

initial begin
    m_AXI_AWREADY = 1'b0; m_AXI_WREADY = 1'b0; m_AXI_ARREADY = 1'b0;
    m_AXI_BVALID = 1'b0;  m_AXI_RVALID = 1'b0;
    wr_run_r = 1'b0; b_pend_r = 1'b0; ar_cnt = 0;
    for (i = 0; i < 1048576; i = i + 1) mem[i] = 128'h0;
end

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        m_AXI_AWREADY <= 1'b0; m_AXI_WREADY <= 1'b0;
        m_AXI_BVALID  <= 1'b0; wr_run_r <= 1'b0; b_pend_r <= 1'b0;
    end
    else begin
        m_AXI_AWREADY <= 1'b1;
        if (m_AXI_AWVALID & m_AXI_AWREADY) begin
            wr_addr_r <= m_AXI_AWADDR;
            wr_left_r <= m_AXI_AWLEN + 9'd1;
            wr_run_r  <= 1'b1;
            wr_bursts = wr_bursts + 1;
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

//   读通道：永远不接受读（素材源不该发 AR；发了就是 bug，这里记下来）
always @(posedge clk) begin
    m_AXI_ARREADY = 1'b1;
    m_AXI_RVALID  = 1'b0;
    if (m_AXI_ARVALID) ar_cnt = ar_cnt + 1;
end

//==============================================================================
// CI 发送（和 tb_m2loopback 同一套）
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
// 期望数据：从 assets.hex + palette.hex 算出来
//==============================================================================
//   ★ 调色板一项现在是 AARRGGBB（α 在高字节，见 docs/spec/硬件_加速器.md §5.3）——
//     参考数组必须按【32 位】读：还是 24 位的话，$readmemh 读 8 位十六进制会灌成 X。
//     本 TB 只比 RGB ⇒ 用时一律取【低 24 位】。
reg [31:0] pal  [0:255];
reg [31:0] ramw [0:16383];

function [7:0] asset_idx;
    input integer x, y;
    reg [31:0] w;
    begin
        w = ramw[y*STRIDE_W + (x >> 2)];
        asset_idx = w >> ((x % 4) * 8);
    end
endfunction

//   第 y 行、第 x 个像素的第 c 个字节（c=0/1/2 → B/G/R, 全工程字节序）
function [7:0] exp_byte;
    input integer x, y, c;
    reg [23:0] col;
    begin
        col = pal[asset_idx(x, y)][23:0];      // 低 24 位 = RGB（高位是 α）
        exp_byte = (c == 0) ? col[7:0] : (c == 1) ? col[15:8] : col[23:16];
    end
endfunction

//   ★ 镜像开关：由 draw_sprite_and_check 按用例设置
//     MX(flags[5]) → 要画的第 i 列来自素材第 AW−1−i 列
//     MY(flags[6]) → 要画的第 j 行来自素材第 AH−1−j 行
reg mx_on = 1'b0, my_on = 1'b0;

function [7:0] exp_byte_m;
    input integer x, y, c;
    begin
        //   ★ 镜像是在【要画的那一段】(宽 w_draw) 内部翻，不是整张素材翻了再裁
        exp_byte_m = exp_byte(mx_on ? ((w_draw-1)-x) : x, my_on ? (AH-1-y) : y, c);
    end
endfunction

//   读内存模型的某个字节
function [7:0] peek_byte;
    input [31:0] a;
    peek_byte = mem[a[23:4]][{a[3:0], 3'b000} +: 8];
endfunction

//==============================================================================
// ★ 探针：盯着【源吐出来的 12 字节流】, 逐字节和期望比对
//   目的: 分清"源码流就是错的(Schedule/窗口问题)" 还是"源对了、下游写错了"。
//   期望流 S = [dst_off 个填充][P0 P1 P2 ...]（P = 该行的像素字节）,
//   源第 w 个字 = S[12w .. 12w+12)。
//==============================================================================
integer pr_line, pr_word, pr_byte, pr_err, pr_dst_off, pr_cmd_dst;
reg [31:0] pr_addr_r;
reg     pr_on;
reg [7:0] pr_got, pr_exp;

task probe_stream;
    input integer dst;
    begin
        pr_cmd_dst  = dst;
        pr_dst_off  = dst % 16;
        pr_line     = 0;
        pr_word     = 0;
        pr_byte     = 0;
        pr_err      = 0;
        pr_on       = 1'b1;
    end
endtask

integer pr_badw, pr_firstbad, pr_words;
always @(posedge clk) begin
    //   源开始一行 ⇒ 行号+1、字/字节计数归零
    if (u_dut.u_PaletteSource.line_start && u_dut.u_PaletteSource.my_line_0) begin
        if (pr_line > 0)
            $display("  LINE %0d 小结: 读了 %0d 个字, 最后地址 %0d, 错字 %0d, 第一个错字 %0d",
                     pr_line-1, pr_words, u_dut.u_PaletteSource.index_address_r, pr_badw, pr_firstbad);
        pr_line = pr_line + 1;
        pr_word = 0;
        pr_badw = 0; pr_firstbad = -1; pr_words = 0;
    end
    if (pr_on && u_dut.u_PaletteSource.palette_valid && u_dut.u_PaletteSource.palette_ready)
        pr_words = pr_words + 1;
    if (pr_on && u_dut.u_PaletteSource.palette_valid && u_dut.u_PaletteSource.palette_ready) begin
        for (c = 0; c < 12; c = c + 1) begin
            pr_got = u_dut.u_PaletteSource.palette_data[c*8 +: 8];       // 低位=最老的字节
            pr_exp = ((pr_line-1) < AH) ? exp_byte_m(((pr_word*12 + c) - pr_dst_off) / 3,
                                                   (pr_line-1),
                                                   ((pr_word*12 + c) - pr_dst_off) % 3) : 8'h00;
            //   ★ 只检查本行像素覆盖的范围: 超出 3*AW 的那几个字节是尾巴, 源允许吐垃圾
            //     （由 DDRPacker 在行边界丢掉）, 拿它当错就是假报警。
            if (((pr_word*12 + c) >= pr_dst_off) && (((pr_word*12 + c) - pr_dst_off) < (3*w_draw))) begin
                if (pr_got !== pr_exp) begin
                    pr_err = pr_err + 1;
                    pr_badw = pr_badw + 1;
                    if (pr_firstbad < 0) begin
                        pr_firstbad = pr_word;
                        $display("  ★ 行%0d 第一个错字 = %0d", pr_line-1, pr_word);
                    end
                    if (pr_badw <= 3)
                        $display("     字%0d 字节%0d: got %02x exp %02x", pr_word, c, pr_got, pr_exp);
                    if (pr_err <= 6)
                        $display("  [SRC-ERR] 行%0d 源字%0d 字节%0d: got %02x exp %02x",
                                 pr_line-1, pr_word, c, pr_got, pr_exp);
                end
            end
        end
        pr_word = pr_word + 1;
    end
end

//==============================================================================
// 行完成计数
//==============================================================================
always @(posedge clk)
    if (u_dut.u_Factory.line_done) line_done_cnt = line_done_cnt + 1;

//==============================================================================
// 主序列
//==============================================================================
integer t0, timeout;
task draw_sprite_and_check;
    input [31:0] dst;
    input [6:0]  flip;              // flags[5]=MX 水平, flags[6]=MY 垂直；0 = 不镜像
    input [8*16-1:0] tag;
    begin
        mx_on = flip[5];            // ★ 期望值跟着镜像走
        my_on = flip[6];
        line_done_cnt = 0;
        probe_stream(dst);                      // ★ 打开源出口探针
        //   命令: 第 1 条给宽高 + 目的 ; 第 2 条（第 1 张图）给源地址(PRAM 字 0) + flags/跨距
        ci_send(FID_BLIT, {w_draw[15:0], AH[15:0]}, dst);
        ci_send(FID_ARGS, 32'd0,
                {1'b0, (FLAGS_SPRITE | flip), STRIDE_W});   // ★ 镜像位就加在 flags 上

        //   等到恰好 AH 行写完
        timeout = 0;
        while ((line_done_cnt < AH) && (timeout < 200000)) begin
            @(negedge clk); timeout = timeout + 1;
        end
        if (line_done_cnt != AH) begin
            $display("  [ERR] %0s: line_done=%0d 期望 %0d", tag, line_done_cnt, AH);
            err_cnt = err_cnt + 1;
        end

        //   逐字节比对: 块内要等于素材, 块外要还是 0
        for (j = 0; j < AH; j = j + 1) begin
            for (i = 0; i < w_draw; i = i + 1) begin
                for (c = 0; c < 3; c = c + 1) begin
                    got_b = peek_byte(dst + j*STRIDE + i*3 + c);
                    exp_b = exp_byte_m(i, j, c);
                    n_chk = n_chk + 1;
                    if (got_b !== exp_b) begin
                        err_cnt = err_cnt + 1;
                        if (err_cnt <= 8)
                            $display("  [ERR] %0s px(%0d,%0d) byte%0d: got %02x exp %02x",
                                     tag, i, j, c, got_b, exp_b);
                    end
                end
            end
            //   块外(右边一个像素)应该还是 0
            if (peek_byte(dst + j*STRIDE + w_draw*3) !== 8'h00) begin
                err_cnt = err_cnt + 1;
                if (err_cnt <= 8) $display("  [ERR] %0s 行 %0d 块外被写了", tag, j);
            end
        end
        pr_on = 1'b0;
        //   最后一行的小结（它后面没有 line_start 了, 单独打）
        $display("  LINE %0d 小结: 读了 %0d 个字, 最后地址 %0d, 错字 %0d, 第一个错字 %0d",
                 pr_line-1, pr_words, u_dut.u_PaletteSource.index_address_r, pr_badw, pr_firstbad);
        $display("  %0s 画完并比对 (line_done=%0d) / ★ 源出口流错误 %0d", tag, line_done_cnt, pr_err);
    end
endtask

initial begin
    ci_function_id = 10'd0; ci_inputs_0 = 32'd0; ci_inputs_1 = 32'd0;
    ci_cmd_valid = 1'b0;    ci_rsp_ready = 1'b1;
    err_cnt = 0; n_chk = 0; line_done_cnt = 0; wr_bursts = 0; rd_bursts = 0;
    rst_n = 1'b0;

    $readmemh("hex/assets.hex", ramw);
    $readmemh("hex/palette.hex", pal);

    repeat (10) @(negedge clk);
    rst_n = 1'b1;
    repeat (10) @(negedge clk);

    $display("---- M3 整链: 素材源(PRAM 索引 -> 调色板) 画两份, 相位不同 ----");
    w_draw = AW;
    draw_sprite_and_check(DST_A, 7'h00, "dst_off=12");
    draw_sprite_and_check(DST_B, 7'h00, "dst_off=2 ");

    $display("---- 镜像: MX / MY / MX+MY（源地址约定不变，只加 flags[5]/[6]）----");
    w_draw = 60;                        // ★ 4 的倍数：MX 的跨度要求（避开行尾 padding）
    draw_sprite_and_check(DST_C, 7'h20, "MX  水平镜像");
    w_draw = AW;
    draw_sprite_and_check(DST_D, 7'h40, "MY  垂直镜像");
    w_draw = 60;
    draw_sprite_and_check(DST_E, 7'h60, "MX+MY 180 度 ");
    w_draw = AW;

    $display("");
    $display("  检查字节数 %0d / 写突发 %0d / AXI 读请求 %0d (必须是 0)", n_chk, wr_bursts, ar_cnt);
    $display("");
    if (err_cnt == 0 && ar_cnt == 0)
        $display("SUMMARY: err_cnt=0 ar=0 RESULT=ALL-PASS");
    else
        $display("SUMMARY: err_cnt=%0d ar=%0d RESULT=FAIL", err_cnt, ar_cnt);
    $finish;
end

endmodule
