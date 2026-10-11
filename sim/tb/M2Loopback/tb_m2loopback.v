//==============================================================================
//  tb_m2loopback.v —— M2 回环：DDR 读回来 → 加速器合成 → 写回 DDR
//------------------------------------------------------------------------------
//  测的是什么（这是 M2 的"主链路"，一次把整条链全串起来跑）
//
//      CI ─▶ CmdChannel ─▶ CmdExec ──行任务(广播)──┬─▶ DDRSource ──96b─┐
//                                                  │                    ▼
//                                                  │              Compositor ──96b─▶ DDRPacker ──128b─▶ WriteController
//                                                  └──────────────────────────────────────────────────────────┘
//            DDRSource ──AR/R──▶ [DDR 内存模型] ◀──AW/W/B── WriteController
//                                  ▲                            │
//                                  └──────── 数据真的搬过来了吗 ─┘
//
//  判据（不是"没报错"，是逐字节比对）
//    1) 目的区【数据区】逐字节 == 源区对应字节        —— 数据搬对了
//    2) 目的区【头 4 字节】还是哨兵 0xA5              —— 首拍 WSTRB 遮头遮住了
//    3) 目的区【尾 4 字节起】还是哨兵 0xA5            —— 末拍 WSTRB 遮尾遮住了
//    4) 3 行全部 line_done、命令收工、AR/W 笔数符合几何
//  这三条一起才算"回环通了"：只对上数据而遮头遮尾错，说明字节相位错了。
//
//  为什么用真的 GPU_2D 而不是拼一个数据通路
//      CI → 命令 → 行任务这一段 M1 已经在板上验过；这里直接用它，
//      验的就是"接上真 DDR 之后整条链还对不对"。
//
//  DDR 内存模型
//      16 字节一个字的字节数组，索引取地址 [23:4]（20 位 = 1M 个字 = 16MB 地址空间，够用）
//      ★ 索引位宽必须盖住源和目的基址的差别：0x0900_0000 与 0x0880_0000 的区别在 bit 23，
//        用 [15:4] 或 [19:4] 都是 0 → 两个区域混叠成一个字（踩过：读回来的是哨兵值）
//      读：AR 收下 addr/len，按拍连吐，RLAST 落对；
//      写：AW 收地址，W 按 WSTRB 逐字节写（★ 遮头遮尾全靠它），B 回一拍。
//
//  编译运行(ModelSim)
//    vlog AXI4_Master_Write.v AXI4_Master_Read.v BlockRAM.v async_fifo.v async_PingPongBuffer.v \
//         CIUnpack.v CmdBuffer.v CmdChannel.v CmdExec.v DDRUnpacker.v DDRSource.v \
//         Compositor.v DDRPacker.v WriteController.v AXIMastersHandler.v GPU_2D.v
//    vlog tb_m2loopback.v
//    vsim -c tb_m2loopback -do "run -all; quit -f"
//==============================================================================
`timescale 1ns/100ps

module tb_m2loopback;

localparam CLK_HALF = 5.0;

// ---- 几何：全屏跨距，源和目的都故意让它不整齐 ----
//   板上 mode 8 的真实参数（用来复现上板卡住）：C(600,300) -> D(1200,700)，600x300
//     src = 0x0800_0000 + 300*5760 + 600*3  = 0x081A_64C8  (低 4 位 = 8)
//     dst = 0x0880_0000 + 700*5760 + 1200*3 = 0x08BD_95D0  (低 4 位 = 0)
localparam  [31:0]              SRC       = 32'h081A_64C8;
localparam  [31:0]              DST       = 32'h08BD_95D0;
localparam  [31:0]              DST_OFF   = 32'h08BD_95D0;
localparam  [31:0]              DST_OFF_B = 32'd0;              // 目的块在拍内的起始偏移(= DST_OFF - DST)
localparam  [15:0]              WIDTH     = 16'd600;
localparam  [15:0]              HEIGHT    = 16'd300;
localparam  [31:0]              STRIDE    = 32'd5760;           // 1920 × 3

localparam  [31:0]              DST2       = 32'h0880_8000;     // FILL 段的目的拍网格基准（另一块地方）
//   ★ FILL 段故意挑一个【非 3 的倍数】的拍内偏移(8): 常数色没有源可以拿来对齐,
//     所以它是"拍对齐归 DDRPacker"这条改动最容易被漏掉的那条路 —— 留着当回归哨兵。
//     (2026-10 实测: Compositor 里老的相位轮转没删时, 这一段正好会报错。见 硬件_加速器.md §4.4)
localparam  [31:0]              DST2_OFF   = 32'h0880_8008;     // 低 4 位 = 8（非 3 的倍数）
localparam  [23:0]              FILL_RGB   = 24'h12_34_56;      // R=0x12 G=0x34 B=0x56
localparam  [31:0]              FILL_OFF_B = 32'd8;             // 块在拍内的起始字节偏移(= DST2_OFF 的低 4 位)

//   ---- 第三段：MODE_ALPHA（PRAM 精灵 + 读回目的自己）----
//     ★ 参数故意取 demo_glass 里第一块玻璃格的样子：w=32、目的低 4 位=0
localparam  [31:0]              DST3_OFF  = 32'h0884_0000;                  // 低 4 位 = 0
localparam  [15:0]              AL_W      = 16'd32;
localparam  [15:0]              AL_H      = 16'd16;
localparam  [6:0]               AL_FLAGS  = {1'b0,1'b0,1'b0,1'b1,3'd2};     // SPRITE(0x08) | MODE_ALPHA(2) = 0x0A
localparam  [23:0]              AL_STRIDE = 24'd8;                          // 源行跨距(字)

//   ---- 第四段：图片合成（两张 PRAM 素材图合成一张）----
//     ★ 只回答一个问题：这条命令【能不能跑完】。合成要两趟读 PRAM（图一 → 图二），
//       中间 imageCombine 还要缓存图一那一行 —— 任何一处握手不对就永远等不到 line_done
//       ⇒ 板上表现就是"黑屏 + 串口停住"。
localparam  [31:0]              DST4_OFF     = 32'h0888_0000;       // 低 4 位 = 0
localparam  [15:0]              CMB_W        = 16'd32;
localparam  [15:0]              CMB_H        = 16'd8;
localparam  [31:0]              CMB_A        = 32'd2886;            // assets_desc.h: "num0" (32x48, 跨距 8 字)
localparam  [31:0]              CMB_B        = 32'd3270;            // assets_desc.h: "num1"
localparam  [23:0]              CMB_STR      = 24'd8;               // 源行跨距(字)
localparam  [6:0]               CMB_FLAGS    = 7'h09;               // MODE_REPLACE(1) | SPRITE(0x08)
localparam  [6:0]               CMB_FLAGS_CK = 7'h19;               // 上面那格 + CKEN(flags[4]) —— 命令级 ck 开关

//   ---- 第五段：合成 + ck（复现 demo 的 ④），逐像素比对 ----
//     ★ 这一段回答"下游对不对"：源里的 ck 掩码 → imageCombine 选图层 → 打包 → WSTRB 落地。
//       期望（和 demo_combine 的自检同一条契约）：
//         图二这点 != 白  ⇒ 写图二
//         图二这点 == 白  ⇒ 图二不写；图一 != 白 就露图一，图一也白就露底色
localparam  [31:0]              DST5_OFF = 32'h088C_0000;       // 低 4 位 = 0
localparam  [15:0]              CK_W     = 16'd32;
localparam  [15:0]              CK_H     = 16'd16;
localparam  [23:0]              CK_BG    = 24'h20_30_40;        // 底色（和 demo 一样）
localparam  [31:0]              DST6_OFF = 32'h088E_0000;       // 第六段：单图 BLIT（只验源的行地址）
localparam  [31:0]              DST7_OFF = 32'h0890_0000;       // 第七段：合成但【不开 ck】（期望处处 = 图二）

localparam  [9:0]               FID_BLIT = 10'h002;     // funct3=2
localparam  [9:0]               FID_FILL = 10'h001;     // funct3=1
localparam  [9:0]               FID_ARGS = 10'h007;     // funct3=7 参数 CI（命令结束词）

reg                     clk, rst_n;
integer                 i, j, line, px, c, k;
integer                 al_ls0, al_ld0;                                     // alpha 段开始前的行任务/落盘计数快照
integer                 cmb_ls0, cmb_ld0;                                   // 合成段开始前的快照
integer                 ck_ls0, ck_ld0;                                     // 合成+ck 段开始前的快照
integer                 cmb_done_lines;                                     // 合成段【当场】落盘的行数（后面几段还会往上加，判定要用快照）
integer                 err_bytes, n_expose_img1, n_bgcolor, n_img2_pix;    // 合成+ck 段的逐像素统计
integer                 row_y, col_x;
reg [23:0]              pix_img1, pix_img2, pix_exp;

integer                 err_cnt, rd_bursts, wr_bursts, line_start_cnt, line_done_cnt, fill_err;
integer                 blit_ls, blit_ld;                                                           // BLIT 段结束时的快照（FILL 段还会往上加）
reg  [7:0]              exp_b, got_b;

//==============================================================================
// CI 侧
//==============================================================================
reg  [9:0]                  ci_function_id;
reg  [31:0]                 ci_inputs_0, ci_inputs_1;
reg                         ci_cmd_valid;
wire                        ci_cmd_ready;
wire                        ci_rsp_valid;
reg                         ci_rsp_ready;
wire [31:0]                 ci_outputs_0;

//==============================================================================
// AXI 主口（GPU_2D）→ DDR 内存模型
//==============================================================================
wire [7:0]                  m_AXI_AWID;
wire [31:0]                 m_AXI_AWADDR;
wire [7:0]                  m_AXI_AWLEN;
wire [2:0]                  m_AXI_AWSIZE;
wire [1:0]                  m_AXI_AWBURST;
wire                        m_AXI_AWLOCK;
wire                        m_AXI_AWVALID;
wire [127:0]                m_AXI_WDATA;
wire [15:0]                 m_AXI_WSTRB;
wire                        m_AXI_WLAST, m_AXI_WVALID;
wire [7:0]                  m_AXI_BID;
wire [1:0]                  m_AXI_BRESP;
wire [7:0]                  m_AXI_ARID;
wire [31:0]                 m_AXI_ARADDR;
wire [7:0]                  m_AXI_ARLEN;
wire [2:0]                  m_AXI_ARSIZE;
wire [1:0]                  m_AXI_ARBURST;
wire                        m_AXI_ARLOCK;
wire                        m_AXI_ARVALID;
wire [7:0]                  m_AXI_RID;
wire [127:0]                m_AXI_RDATA;
wire [1:0]                  m_AXI_RRESP;
wire                        m_AXI_RLAST, m_AXI_RVALID;

wire                        m_AXI_AWREADY;
wire                        m_AXI_WREADY;
reg                         m_AXI_BVALID;
wire                        m_AXI_BREADY;
wire                        m_AXI_ARREADY;
reg  [127:0]                m_AXI_RDATA_r;
reg  [1:0]                  m_AXI_RRESP_r;
reg                         m_AXI_RLAST_r, m_AXI_RVALID_r;
wire                        m_AXI_RREADY;

assign m_AXI_RDATA  = m_AXI_RDATA_r;
assign m_AXI_RRESP  = m_AXI_RRESP_r;
assign m_AXI_RLAST  = m_AXI_RLAST_r;
assign m_AXI_RVALID = m_AXI_RVALID_r;
assign m_AXI_BID    = 8'h00;
assign m_AXI_BRESP  = 2'b00;

initial begin clk = 1'b0; forever #(CLK_HALF) clk = ~clk; end

//==============================================================================
// 被测：真的 GPU_2D
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
    .clk_HDMI_x1                        (clk),
    .rst_hdmi_n                         (rst_n),

    // 寄存器侧（★ 总线层 APB0_gpu2d 在 APB0_Interconnect 里，本 TB 不例化它）:
    //   显示关掉 / 全屏 / 无交帧 —— 本 TB 只验加速器那条链
    .enable                       (1'b0),
    .split_pending                (1'b0),
    .framea_base_addr             (64'd0),
    .frameb_base_addr             (64'd0),
    .frame_full_base_addr         (64'd0),
    .ckey_color                   (24'hFFFFFF),   // ★ ck 的键色（原来悬空 = z，开 ck 的用例必须给）
    .done_gpu2d                   (1'b0),   /* ★ 端口早就改名了: done_acc -> done_gpu2d */
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

    .lineBuffer_m_AXI_AWREADY           (1'b0),
    .lineBuffer_m_AXI_WREADY            (1'b0),
    .lineBuffer_m_AXI_BID               (8'd0),
    .lineBuffer_m_AXI_BRESP             (2'd0),
    .lineBuffer_m_AXI_BVALID            (1'b0),
    .lineBuffer_m_AXI_ARREADY           (1'b0),
    .lineBuffer_m_AXI_RID               (8'd0),
    .lineBuffer_m_AXI_RDATA             (128'd0),
    .lineBuffer_m_AXI_RRESP             (2'd0),
    .lineBuffer_m_AXI_RLAST             (1'b0),
    .lineBuffer_m_AXI_RVALID            (1'b0),

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
// DDR 内存模型：16 字节一个字，按地址 [23:4] 索引
//==============================================================================
reg [127:0] mem [0:1048575]; // 索引取地址 [23:4]（2^20 个字 = 16MB 地址空间）

//   ---- 素材/调色板：和硬件加载的是同一份 hex，用来算"这个像素应该是什色" ----
reg [31:0] amem [0:32767];  // 素材 PRAM（一个字 4 个索引）
reg [31:0] pmem [0:255];    // 调色板（AARRGGBB）

//   素材第 (x,y) 个像素的颜色：字地址 = base + y*stride + (x/4)，字内第 (x%4) 个字节 = 索引
function [23:0] asset_pix;
    input [31:0]            base;
    input [23:0]            str;
    input                   integer x;
    input                   integer y;
    reg [31:0]              w;
    reg [7:0]               ix;
    begin
        w  = amem[base + y*str + (x>>2)];
        ix = w[8*(x&3) +: 8];
        asset_pix = pmem[ix][23:0];
    end
endfunction

//   一字节：低位 = 地址低 4 位算出的字节偏移（WSTRB[i] ↔ 地址偏移 i）
task poke_byte;
    input [31:0]            a;
    input [7:0]             v;
    begin
        mem[a[23:4]][{a[3:0], 3'b000} +: 8] = v;
    end
endtask

function [7:0] peek_byte;
    input [31:0] a;
    begin
        peek_byte = mem[a[23:4]][{a[3:0], 3'b000} +: 8];
    end
endfunction

//   源区图案：值 == 字节偏移的低 8 位 + 0x20（位置相关，错一个字节就能看出来）
function [7:0] pat;
    input       integer ln;
    input       integer x;
    input       integer ch;
    begin
        pat = (ln*5760 + x*3 + ch + 32) & 32'hFF;
    end
endfunction

//------------------------------------------------------------------------------
// 读通道（AR / R）
//------------------------------------------------------------------------------
reg  [8:0]                  rd_left_r;
reg  [31:0]                 rd_addr_r;
reg                         rd_run_r;

assign m_AXI_ARREADY = ~rd_run_r;
assign m_AXI_RID     = 8'h00;

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        rd_run_r <= 1'b0; rd_left_r <= 9'd0; rd_addr_r <= 32'd0;
        m_AXI_RVALID_r <= 1'b0; m_AXI_RLAST_r <= 1'b0; m_AXI_RDATA_r <= 128'd0;
    end
    else if (!rd_run_r) begin
        if (m_AXI_ARVALID & m_AXI_ARREADY) begin
            rd_run_r       <= 1'b1;
            rd_left_r      <= {1'b0, m_AXI_ARLEN} + 9'd1;
            rd_addr_r      <= m_AXI_ARADDR;
            m_AXI_RDATA_r  <= mem[m_AXI_ARADDR[23:4]];
            m_AXI_RVALID_r <= 1'b1;
            m_AXI_RLAST_r  <= (m_AXI_ARLEN == 8'd0);
            rd_bursts      = rd_bursts + 1;
            $display("    [T=%0t] AR addr=%08h len=%0d", $time, m_AXI_ARADDR, m_AXI_ARLEN);
        end
    end
    else if (m_AXI_RVALID_r & m_AXI_RREADY) begin
        if (rd_left_r == 9'd1) begin
            rd_run_r <= 1'b0; m_AXI_RVALID_r <= 1'b0; m_AXI_RLAST_r <= 1'b0;
        end
        else begin
            rd_left_r      <= rd_left_r - 9'd1;
            rd_addr_r      <= rd_addr_r + 32'd16;
            m_AXI_RDATA_r  <= mem[rd_addr_r[23:4] + 20'd1];
            m_AXI_RLAST_r  <= (rd_left_r == 9'd2);
        end
    end
end

//------------------------------------------------------------------------------
// 写通道（AW / W / B）
//------------------------------------------------------------------------------
reg  [8:0]                  wr_left_r;
reg  [31:0]                 wr_addr_r;
reg                         wr_run_r;
reg                         b_pend_r;
integer                     wb;

assign m_AXI_AWREADY = ~wr_run_r & ~b_pend_r;
assign m_AXI_WREADY  = wr_run_r;
assign m_AXI_BREADY  = 1'b1;                    // 永远收 B

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        wr_run_r  <= 1'b0; b_pend_r <= 1'b0;
        wr_left_r <= 9'd0; wr_addr_r <= 32'd0;
        m_AXI_BVALID <= 1'b0;
    end
    else begin
        if (m_AXI_AWVALID & m_AXI_AWREADY) begin
            wr_run_r  <= 1'b1;
            wr_addr_r <= m_AXI_AWADDR;
            wr_left_r <= {1'b0, m_AXI_AWLEN} + 9'd1;
            wr_bursts = wr_bursts + 1;
            $display("    [T=%0t] AW addr=%08h len=%0d", $time, m_AXI_AWADDR, m_AXI_AWLEN);
        end

        if (m_AXI_WVALID & m_AXI_WREADY) begin
            for (wb = 0; wb < 16; wb = wb + 1)
                if (m_AXI_WSTRB[wb])
                    mem[wr_addr_r[23:4]][wb*8 +: 8] <= m_AXI_WDATA[wb*8 +: 8];
            wr_addr_r <= wr_addr_r + 32'd16;
            wr_left_r <= wr_left_r - 9'd1;
            if (m_AXI_WLAST) begin
                wr_run_r <= 1'b0;
                b_pend_r <= 1'b1;
            end
        end

        if (b_pend_r) m_AXI_BVALID <= 1'b1;
        if (m_AXI_BVALID & m_AXI_BREADY) begin
            m_AXI_BVALID <= 1'b0;
            b_pend_r     <= 1'b0;
        end
    end
end

//==============================================================================
// 探针：行任务的收发（命令行数对不对，一眼看出来）
//==============================================================================
integer             r_beats, w_beats;
integer             c_unp, c_pix, c_pkin, c_pkout, c_wbeat, c_rready, c_wbus, c_rbeat, c_pkin_src;
integer             c_wrlow, c_unpw, c_rdw;
reg                 p_ls, p_ld;
reg [1:0]           p_ss, p_ws;
always @(posedge clk) begin
    if (m_AXI_RVALID_r & m_AXI_RREADY) r_beats = r_beats + 1;
    if (u_dut.rd_ready)                                                          c_rready = c_rready + 1;
    if (!u_dut.u_DDRSource.bank_wr_ready)                                        c_wrlow = c_wrlow + 1;
    if (u_dut.u_DDRSource.unpack_out_valid & ~u_dut.u_DDRSource.unpack_out_ready) c_unpw = c_unpw + 1;
    if (u_dut.u_DDRSource.rd_valid & ~u_dut.u_DDRSource.rd_ready)                 c_rdw   = c_rdw + 1;
    if (u_dut.u_DDRSource.unpack_out_valid & u_dut.u_DDRSource.unpack_out_ready) begin
        c_unp = c_unp + 1;
        if (c_unp <= 5)
            $display("    [T=%0t] unpack 出第 %0d 字: %02h %02h %02h %02h %02h %02h %02h %02h %02h %02h %02h %02h",
                     $time, c_unp-1,
                     u_dut.u_DDRSource.unpack_out_data[7:0],   u_dut.u_DDRSource.unpack_out_data[15:8],
                     u_dut.u_DDRSource.unpack_out_data[23:16], u_dut.u_DDRSource.unpack_out_data[31:24],
                     u_dut.u_DDRSource.unpack_out_data[39:32], u_dut.u_DDRSource.unpack_out_data[47:40],
                     u_dut.u_DDRSource.unpack_out_data[55:48], u_dut.u_DDRSource.unpack_out_data[63:56],
                     u_dut.u_DDRSource.unpack_out_data[71:64], u_dut.u_DDRSource.unpack_out_data[79:72],
                     u_dut.u_DDRSource.unpack_out_data[87:80], u_dut.u_DDRSource.unpack_out_data[95:88]);
    end
    if (u_dut.u_DDRSource.rd_valid & u_dut.u_DDRSource.rd_ready) begin
        c_rbeat = c_rbeat + 1;
        if (c_rbeat <= 3)
            $display("    [T=%0t] 读第 %0d 拍收进: %02h %02h %02h ... %02h %02h %02h",
                     $time, c_rbeat-1,
                     u_dut.u_DDRSource.rd_data[7:0],   u_dut.u_DDRSource.rd_data[15:8],  u_dut.u_DDRSource.rd_data[23:16],
                     u_dut.u_DDRSource.rd_data[103:96],u_dut.u_DDRSource.rd_data[111:104],u_dut.u_DDRSource.rd_data[119:112]);
    end
    if (u_dut.u_DDRSource.pixel_rd_valid    & u_dut.u_DDRSource.pixel_rd_en)         c_pix    = c_pix + 1;

    if (u_dut.u_Factory.u_DDRPacker.pixel_valid & u_dut.u_Factory.u_DDRPacker.pixel_ready) begin
        c_pkin = c_pkin + 1;
        if (u_dut.u_Factory.u_Compositor.src_used) c_pkin_src = c_pkin_src + 1;
        if (u_dut.u_Factory.u_Compositor.src_used && (c_pkin_src <= 6))
            $display("    [T=%0t] packer 收第 %0d 字 (cnt=%0d): %02h %02h %02h %02h %02h %02h %02h %02h %02h %02h %02h %02h  src_used=%b",
                     $time, c_pkin_src-1, u_dut.u_Factory.u_DDRPacker.cnt_byte,
                     u_dut.u_Factory.u_DDRPacker.pixel_data[7:0],   u_dut.u_Factory.u_DDRPacker.pixel_data[15:8],
                     u_dut.u_Factory.u_DDRPacker.pixel_data[23:16], u_dut.u_Factory.u_DDRPacker.pixel_data[31:24],
                     u_dut.u_Factory.u_DDRPacker.pixel_data[39:32], u_dut.u_Factory.u_DDRPacker.pixel_data[47:40],
                     u_dut.u_Factory.u_DDRPacker.pixel_data[55:48], u_dut.u_Factory.u_DDRPacker.pixel_data[63:56],
                     u_dut.u_Factory.u_DDRPacker.pixel_data[71:64], u_dut.u_Factory.u_DDRPacker.pixel_data[79:72],
                     u_dut.u_Factory.u_DDRPacker.pixel_data[87:80], u_dut.u_Factory.u_DDRPacker.pixel_data[95:88],
                     u_dut.u_Factory.u_Compositor.src_used);
    end
    if (u_dut.u_Factory.u_DDRPacker.packer_valid       & u_dut.u_Factory.u_DDRPacker.packer_ready)         c_pkout  = c_pkout + 1;
    if (u_dut.wr_valid                    & u_dut.wr_ready)                      c_wbeat  = c_wbeat + 1;
    if (m_AXI_WVALID & m_AXI_WREADY) begin
        c_wbus = c_wbus + 1;
        if (c_wbus <= 3)
            $display("    [T=%0t] W 总线第 %0d 拍: addr=%08h strb=%04h data=%032h", $time, c_wbus, wr_addr_r, m_AXI_WSTRB, m_AXI_WDATA);
    end
    if (m_AXI_AWVALID & m_AXI_AWREADY)
        $display("    [T=%0t] AW 握手: addr=%08h", $time, m_AXI_AWADDR);
    if (m_AXI_WVALID & m_AXI_WREADY) begin
        w_beats = w_beats + 1;
        if ((w_beats == 1) || (w_beats == 38))
            $display("    [T=%0t] W 第 %0d 拍", $time, w_beats);
    end
    if (u_dut.u_DDRSource.state !== p_ss) begin
        p_ss = u_dut.u_DDRSource.state;
        $display("    [T=%0t]   src.state -> %0d  (r=%0d w=%0d)", $time, u_dut.u_DDRSource.state, r_beats, w_beats);
    end
    if (u_dut.u_Factory.u_WriteController.state !== p_ws) begin
        p_ws = u_dut.u_Factory.u_WriteController.state;
        $display("    [T=%0t]   wr.state  -> %0d  (r=%0d w=%0d)", $time, u_dut.u_Factory.u_WriteController.state, r_beats, w_beats);
    end
end


always @(posedge clk) begin
    if (u_dut.line_start & ~p_ls) begin line_start_cnt = line_start_cnt + 1;
        $display("    [T=%0t] line_start #%0d flags=%b dst=%08h src=%08h",
                 $time, line_start_cnt, u_dut.line_flags, u_dut.line_dst_addr, u_dut.line_src_addr);
    end
    if (u_dut.line_done & ~p_ld) begin line_done_cnt = line_done_cnt + 1;
        $display("    [T=%0t] line_done  #%0d   (r=%0d rd_rdy=%0d unpack=%0d pix=%0d pk_in=%0d pk_out=%0d wbeat=%0d)",
                 $time, line_done_cnt, r_beats, c_rready, c_unp, c_pix, c_pkin, c_pkout, c_wbeat);
        $display("        STALL pp_wrlow=%0d unpack_blocked=%0d rd_wait=%0d",
                 c_wrlow, c_unpw, c_rdw, $time);
    end
    p_ls <= u_dut.line_start;
    p_ld <= u_dut.line_done;
end

//==============================================================================
// CI 发送：一条 CI 一个 valid/ready 握手
//==============================================================================
task ci_send;
    input [9:0]             fid;
    input [31:0]            in0;
    input [31:0]            in1;
    begin
        @(posedge clk); #1;
        ci_function_id = fid;
        ci_inputs_0    = in0;
        ci_inputs_1    = in1;
        ci_cmd_valid   = 1'b1;
        @(posedge clk); #1;
        while (!ci_cmd_ready) begin @(posedge clk); #1; end
        ci_cmd_valid = 1'b0;
        @(posedge clk); #1;
    end
endtask

//==============================================================================
// 主流程
//==============================================================================
initial begin
    err_cnt = 0; fill_err = 0; rd_bursts = 0; wr_bursts = 0; line_start_cnt = 0; line_done_cnt = 0;
    p_ls = 0; p_ld = 0; r_beats = 0; w_beats = 0; p_ss = 2'd0; p_ws = 2'd0;
    c_unp = 0; c_pix = 0; c_pkin = 0; c_pkout = 0; c_wbeat = 0; c_rready = 0; c_wbus = 0; c_rbeat = 0; c_pkin_src = 0; c_wrlow = 0; c_unpw = 0; c_rdw = 0;
    ci_cmd_valid = 1'b0; ci_rsp_ready = 1'b1;
    ci_function_id = 10'd0; ci_inputs_0 = 32'd0; ci_inputs_1 = 32'd0;
    rst_n = 1'b0;

    for (i = 0; i < 1048576; i = i + 1) mem[i] = 128'd0;
    $readmemh("assets.hex",  amem);      // 和 GPU_2D 里 PaletteRAM 加载的是同一份
    $readmemh("palette.hex", pmem);

    // ---- 源区：写图案 ----
    for (line = 0; line < HEIGHT; line = line + 1)
        for (px = 0; px < WIDTH; px = px + 1)
            for (c = 0; c < 3; c = c + 1)
                poke_byte(SRC + line*STRIDE + px*3 + c, pat(line, px, c));

    // ---- 目的区：整片铺哨兵 0xA5（好验"遮头遮尾没被写坏"）----
    for (line = 0; line < HEIGHT; line = line + 1)
        for (i = 0; i < 3*WIDTH + 64; i = i + 1)
            poke_byte(DST + line*STRIDE + i, 8'hA5);

    repeat (10) @(posedge clk);
    rst_n = 1'b1;
    repeat (20) @(posedge clk);

    // ---- 一条 BLIT：源 = 帧缓冲里的 200×3 块，目的 = 另一帧里低 4 位错开的同尺寸块 ----
    $display("==== 内存模型自检 ====");
    $display("  源首字 mem[%05h] = %032h", SRC[23:4], mem[SRC[23:4]]);
    $display("  目的首字 mem[%05h] = %032h", DST[23:4], mem[DST[23:4]]);
    $display("  源头 6 字节 = %02h %02h %02h %02h %02h %02h",
             peek_byte(SRC), peek_byte(SRC+1), peek_byte(SRC+2),
             peek_byte(SRC+3), peek_byte(SRC+4), peek_byte(SRC+5));
    $display("==== 发命令: BLIT src=%08h dst=%08h w=%0d h=%0d ====", SRC, DST_OFF, WIDTH, HEIGHT);
    ci_send(FID_BLIT, {WIDTH, HEIGHT}, DST_OFF);                      // 命令 CI：宽高 + 目的
    ci_send(FID_ARGS, SRC, {1'b0, 7'd1, 24'd0});                      // 第 1 张图：地址 + {more,flags=REPLACE,参数}

    repeat (400000) @(posedge clk);

    // ---- 逐字节比对 ----
    blit_ls = line_start_cnt;                   // 快照：BLIT 段的行任务/落盘数
    blit_ld = line_done_cnt;

    $display("==== 校验 ==== (W 总线 %0d 拍, 目的字 mem[%0h]=%032h)",
             c_wbus, DST[23:4], mem[DST[23:4]]);
    for (line = 0; line < HEIGHT; line = line + 1) begin
        // 头 4 字节：首拍 WSTRB 必须遮住（目的块从 DST+4 开始）
        for (i = 0; i < DST_OFF_B; i = i + 1)
            if (peek_byte(DST + line*STRIDE + i) !== 8'hA5) begin
                err_cnt = err_cnt + 1;
                $display("  !! 第 %0d 行 头字节 [%0d] = %02h，应为 A5（遮头坏了）",
                         line, i, peek_byte(DST + line*STRIDE + i));
            end
        // 数据区：逐字节 == 源区
        for (px = 0; px < WIDTH; px = px + 1)
            for (c = 0; c < 3; c = c + 1) begin
                exp_b = pat(line, px, c);
                got_b = peek_byte(DST + line*STRIDE + DST_OFF_B + px*3 + c);
                if (got_b !== exp_b) begin
                    err_cnt = err_cnt + 1;
                    if (err_cnt <= 8)
                        $display("  !! 第 %0d 行 px=%0d c=%0d: 得 %02h 应 %02h（偏移 %0d）",
                                 line, px, c, got_b, exp_b, px*3 + c);
                end
            end
        // 尾：604 起必须是哨兵（末拍 WSTRB 遮尾）
        for (i = DST_OFF_B + 3*WIDTH; i < DST_OFF_B + 3*WIDTH + 36; i = i + 1)
            if (peek_byte(DST + line*STRIDE + i) !== 8'hA5) begin
                err_cnt = err_cnt + 1;
                if (err_cnt <= 8)
                    $display("  !! 第 %0d 行 尾字节 [%0d] = %02h，应为 A5（遮尾坏了）",
                             line, i, peek_byte(DST + line*STRIDE + i));
            end
    end

    //--------------------------------------------------------------------------
    // 第二段: FILL(纯色)—— 验"老测试(mode 7)走的那条路"在新架构下还对
    //   FILL 不走源（模式=0）-> 合成器内部生成常数色
    //   帧缓存里一个像素的字节序是 B,G,R, 所以颜色 0xRRGGBB -> 内存 56 34 12 这样铺
    //--------------------------------------------------------------------------
    $display("==== 第二段: FILL 到 0x%08h ====", DST2_OFF);
    // 目的区铺哨兵
    for (line = 0; line < HEIGHT; line = line + 1)
        for (i = 0; i < 3*WIDTH + 64; i = i + 1)
            poke_byte(DST2 + line*STRIDE + i, 8'hA5);

    ci_send(FID_FILL, {WIDTH, HEIGHT}, DST2_OFF);                      // 命令 CI：宽高 + 目的
    ci_send(FID_ARGS, 32'd0, {1'b0, 7'd0, FILL_RGB});                  // 第 1 张图（纯色图）：地址 0 + 颜色

    repeat (400000) @(posedge clk);

    fill_err = 0;
    for (line = 0; line < HEIGHT; line = line + 1) begin
        // 头 4 字节与尾: 仍是哨兵
        for (i = 0; i < FILL_OFF_B; i = i + 1)
            if (peek_byte(DST2 + line*STRIDE + i) !== 8'hA5) fill_err = fill_err + 1;
        for (i = FILL_OFF_B + 3*WIDTH; i < FILL_OFF_B + 3*WIDTH + 36; i = i + 1)
            if (peek_byte(DST2 + line*STRIDE + i) !== 8'hA5) fill_err = fill_err + 1;
        // 数据区: 按 B,G,R 循环铺色
        for (px = 0; px < WIDTH; px = px + 1)
            for (c = 0; c < 3; c = c + 1) begin
                exp_b = (c == 0) ? FILL_RGB[7:0]      // B
                      : (c == 1) ? FILL_RGB[15:8]     // G
                      :            FILL_RGB[23:16];   // R
                if (peek_byte(DST2 + line*STRIDE + FILL_OFF_B + px*3 + c) !== exp_b) begin
                    fill_err = fill_err + 1;
                    if (fill_err <= 4)
                        $display("  !! FILL 第 %0d 行 px=%0d c=%0d: 得 %02h 应 %02h",
                                 line, px, c,
                                 peek_byte(DST2 + line*STRIDE + FILL_OFF_B + px*3 + c), exp_b);
                end
            end
    end
    $display("==== FILL 段: err=%0d (0 才算 FILL 通路正常) ====", fill_err);

    //--------------------------------------------------------------------------
    // 第三段: MODE_ALPHA —— PRAM 精灵 + 读回【目的自己】
    //   这一段只为回答一个问题：这条命令【能不能跑完】。
    //   alpha 是第一次让两路源【同时】跑（源① DDR 读回目的 + 源② 片上 PRAM），
    //   而合成器要求两路都有货才认一拍 ⇒ 两路在这一行各出几拍必须对得上，
    //   否则先空的那一路会让另一路的尾巴永远没人取 ⇒ 源永远不 free ⇒ 下一行发不出去。
    //--------------------------------------------------------------------------
    $display("==== 第三段: MODE_ALPHA 到 0x%08h (w=%0d h=%0d flags=%02h) ====",
             DST3_OFF, AL_W, AL_H, AL_FLAGS);
    al_ls0 = line_start_cnt;
    al_ld0 = line_done_cnt;
    ci_send(FID_BLIT, {AL_W, AL_H}, DST3_OFF);                      // 命令 CI：宽高 + 目的
    ci_send(FID_ARGS, 32'd0, {1'b0, AL_FLAGS, AL_STRIDE});          // 第 1 张图：地址 = PRAM 第 0 字 + 跨距

    repeat (400000) @(posedge clk);

    $display("==== ALPHA 段: line_start %0d / line_done %0d (期望 %0d / %0d) ====",
             line_start_cnt - al_ls0, line_done_cnt - al_ld0, AL_H, AL_H);

    if ((line_done_cnt - al_ld0) != AL_H) begin
        err_cnt = err_cnt + 1;
        $display("  !! ALPHA 段没跑完: 只落盘 %0d 行(链路挂住)", line_done_cnt - al_ld0);
    end

    //--------------------------------------------------------------------------
    // 第四段: 图片合成 —— 一条命令 3 条 CI (命令开始 + 图一 + 图二)
    //--------------------------------------------------------------------------
    $display("==== 第四段: 图片合成(两图) 到 0x%08h (w=%0d h=%0d) ====", DST4_OFF, CMB_W, CMB_H);
    cmb_ls0 = line_start_cnt;
    cmb_ld0 = line_done_cnt;

    ci_send(FID_BLIT, {CMB_W, CMB_H}, DST4_OFF);                    // 命令开始: 宽高 + 目的
    ci_send(FID_ARGS, CMB_A, {1'b1, CMB_FLAGS, CMB_STR});           // 图一: more=1
    ci_send(FID_ARGS, CMB_B, {1'b0, CMB_FLAGS, CMB_STR});           // 图二: more=0(结束词)

    repeat (400000) @(posedge clk);

    $display("==== 合成段: line_start %0d / line_done %0d (期望 %0d / %0d) ====",
             line_start_cnt - cmb_ls0, line_done_cnt - cmb_ld0, CMB_H, CMB_H);
    cmb_done_lines = line_done_cnt - cmb_ld0;      // ★ 当场快照：后面的段落还会加计数
    if (cmb_done_lines != CMB_H) begin
        err_cnt = err_cnt + 1;
        $display("  !! 合成段没跑完: 只落盘 %0d 行(链路挂住了)", cmb_done_lines);
    end

    //--------------------------------------------------------------------------
    // 第五段: 合成 + ck —— 复现 demo 的 ④，回答"下游对不对"
    //   期望（和 demo_combine 的自检同一条契约）：
    //     图二这点 != 白 ⇒ 写图二；图二 == 白 ⇒ 图二不写，露图一；图一也白 ⇒ 露底色
    //--------------------------------------------------------------------------
    $display("==== 第五段: 合成+ck 到 0x%08h (w=%0d h=%0d) ====", DST5_OFF, CK_W, CK_H);
    //   目的区先铺底色（等价于 demo 里那条 FILL；FILL 通路第二段已经单独验过）
    for (line = 0; line < CK_H; line = line + 1)
        for (i = 0; i < 3*CK_W; i = i + 1)
            poke_byte(DST5_OFF + line*STRIDE + i,
                      ((i % 3) == 0) ? CK_BG[7:0] :
                      ((i % 3) == 1) ? CK_BG[15:8] : CK_BG[23:16]);

    ck_ls0 = line_start_cnt;
    ck_ld0 = line_done_cnt;
    ci_send(FID_BLIT, {CK_W, CK_H}, DST5_OFF);                      // 命令开始
    ci_send(FID_ARGS, CMB_A, {1'b1, CMB_FLAGS,    CMB_STR});        // 图一：不开 ck
    ci_send(FID_ARGS, CMB_B, {1'b0, CMB_FLAGS_CK, CMB_STR});        // 图二：开 ck（命令级开关看这一格）

    repeat (400000) @(posedge clk);

    $display("==== 合成+ck 段: line_start %0d / line_done %0d (期望 %0d / %0d) ====",
             line_start_cnt - ck_ls0, line_done_cnt - ck_ld0, CK_H, CK_H);
    if ((line_done_cnt - ck_ld0) != CK_H) begin
        err_cnt = err_cnt + 1;
        $display("  !! 合成+ck 段没跑完: 只落盘 %0d 行", line_done_cnt - ck_ld0);
    end

    err_bytes = 0; n_expose_img1 = 0; n_bgcolor = 0; n_img2_pix = 0;
    for (row_y = 0; row_y < CK_H; row_y = row_y + 1)
        for (col_x = 0; col_x < CK_W; col_x = col_x + 1) begin
            pix_img1 = asset_pix(CMB_A, CMB_STR, col_x, row_y);
            pix_img2 = asset_pix(CMB_B, CMB_STR, col_x, row_y);
            if (pix_img2 !== 24'hFFFFFF)      begin pix_exp = pix_img2;      n_img2_pix    = n_img2_pix + 1; end
            else if (pix_img1 !== 24'hFFFFFF) begin pix_exp = pix_img1;      n_expose_img1 = n_expose_img1 + 1; end
            else                        begin pix_exp = CK_BG;   n_bgcolor    = n_bgcolor + 1; end
            for (c = 0; c < 3; c = c + 1) begin
                exp_b = ((c == 0) ? pix_exp[7:0] : (c == 1) ? pix_exp[15:8] : pix_exp[23:16]);
                got_b = peek_byte(DST5_OFF + row_y*STRIDE + col_x*3 + c);
                if (got_b !== exp_b) begin
                    err_bytes = err_bytes + 1;
                    if (err_bytes <= 8)
                        $display("  !! 合成+ck (%0d,%0d) c=%0d: 得 %02h 应 %02h (图一 %06h 图二 %06h)",
                                 col_x, row_y, c, got_b, exp_b, pix_img1, pix_img2);
                end
            end
        end
    $display("==== 合成+ck 段: 错字节 %0d, 露图一 %0d 像素, 两张都白(底色) %0d, 图二赢 %0d ====",
             err_bytes, n_expose_img1, n_bgcolor, n_img2_pix);

    //   ---- 把前 4 行的"实测/期望"倒出来，便于逐像素找规律 ----
    begin : dump_blk
        integer                 fd;
        reg [23:0]              got_rgb;
        fd = $fopen("ck_dump.txt", "w");
        for (row_y = 0; row_y < CK_H; row_y = row_y + 1) begin
            for (col_x = 0; col_x < CK_W; col_x = col_x + 1) begin
                got_rgb = {peek_byte(DST5_OFF + row_y*STRIDE + col_x*3 + 2),
                           peek_byte(DST5_OFF + row_y*STRIDE + col_x*3 + 1),
                           peek_byte(DST5_OFF + row_y*STRIDE + col_x*3 + 0)};
                $fwrite(fd, "%06h ", got_rgb);
            end
            $fwrite(fd, "\n");
        end
        $fclose(fd);
    end
    if (err_bytes != 0) err_cnt = err_cnt + 1;

    //--------------------------------------------------------------------------
    // 第六段: 单图 BLIT（图一，不开 ck）—— 把"合成"整个摘掉，单看源那一行对不对
    //   期望：每个字节 == 图一素材对应像素（透明像素在调色板里就是 ffffff，照写）
    //--------------------------------------------------------------------------
    $display("==== 第六段: 单图 BLIT 到 0x%08h ====", DST6_OFF);
    for (line = 0; line < CK_H; line = line + 1)
        for (i = 0; i < 3*CK_W; i = i + 1)
            poke_byte(DST6_OFF + line*STRIDE + i, 8'h00);

    ci_send(FID_BLIT, {CK_W, CK_H}, DST6_OFF);
    ci_send(FID_ARGS, CMB_A, {1'b0, CMB_FLAGS, CMB_STR});
    repeat (400000) @(posedge clk);

    err_bytes = 0;
    for (row_y = 0; row_y < CK_H; row_y = row_y + 1)
        for (col_x = 0; col_x < CK_W; col_x = col_x + 1) begin
            pix_img1 = asset_pix(CMB_A, CMB_STR, col_x, row_y);
            for (c = 0; c < 3; c = c + 1) begin
                exp_b = ((c == 0) ? pix_img1[7:0] : (c == 1) ? pix_img1[15:8] : pix_img1[23:16]);
                got_b = peek_byte(DST6_OFF + row_y*STRIDE + col_x*3 + c);
                if (got_b !== exp_b) err_bytes = err_bytes + 1;
            end
        end
    begin : dump6_blk
        integer                 fd_single;
        reg [23:0]              got_rgb_single;
        fd_single = $fopen("single_dump.txt", "w");
        for (row_y = 0; row_y < CK_H; row_y = row_y + 1) begin
            for (col_x = 0; col_x < CK_W; col_x = col_x + 1) begin
                got_rgb_single = {peek_byte(DST6_OFF + row_y*STRIDE + col_x*3 + 2),
                        peek_byte(DST6_OFF + row_y*STRIDE + col_x*3 + 1),
                        peek_byte(DST6_OFF + row_y*STRIDE + col_x*3 + 0)};
                $fwrite(fd_single, "%06h ", got_rgb_single);
            end
            $fwrite(fd_single, "\n");
        end
        $fclose(fd_single);
    end
    $display("==== 单图 BLIT 段: 错字节 %0d (0 = 源那一行本身是对的) ====", err_bytes);
    if (err_bytes != 0) err_cnt = err_cnt + 1;

    //--------------------------------------------------------------------------
    // 第七段: 合成但不开 ck —— 期望处处 = 图二（把 ck 摘掉，单看"合成"本身对不对）
    //--------------------------------------------------------------------------
    $display("==== 第七段: 合成(不开 ck) 到 0x%08h ====", DST7_OFF);
    for (line = 0; line < CK_H; line = line + 1)
        for (i = 0; i < 3*CK_W; i = i + 1)
            poke_byte(DST7_OFF + line*STRIDE + i, 8'h00);

    ci_send(FID_BLIT, {CK_W, CK_H}, DST7_OFF);
    ci_send(FID_ARGS, CMB_A, {1'b1, CMB_FLAGS, CMB_STR});
    ci_send(FID_ARGS, CMB_B, {1'b0, CMB_FLAGS, CMB_STR});
    repeat (400000) @(posedge clk);

    err_bytes = 0;
    for (row_y = 0; row_y < CK_H; row_y = row_y + 1)
        for (col_x = 0; col_x < CK_W; col_x = col_x + 1) begin
            pix_img2 = asset_pix(CMB_B, CMB_STR, col_x, row_y);
            for (c = 0; c < 3; c = c + 1) begin
                exp_b = ((c == 0) ? pix_img2[7:0] : (c == 1) ? pix_img2[15:8] : pix_img2[23:16]);
                got_b = peek_byte(DST7_OFF + row_y*STRIDE + col_x*3 + c);
                if (got_b !== exp_b) err_bytes = err_bytes + 1;
            end
        end
    begin : dump7_blk
        integer                 fd_nock;
        reg [23:0]              got_rgb_nock;
        fd_nock = $fopen("noc_dump.txt", "w");
        for (row_y = 0; row_y < CK_H; row_y = row_y + 1) begin
            for (col_x = 0; col_x < CK_W; col_x = col_x + 1) begin
                got_rgb_nock = {peek_byte(DST7_OFF + row_y*STRIDE + col_x*3 + 2),
                        peek_byte(DST7_OFF + row_y*STRIDE + col_x*3 + 1),
                        peek_byte(DST7_OFF + row_y*STRIDE + col_x*3 + 0)};
                $fwrite(fd_nock, "%06h ", got_rgb_nock);
            end
            $fwrite(fd_nock, "\n");
        end
        $fclose(fd_nock);
    end
    $display("==== 合成(不开 ck) 段: 错字节 %0d (0 = 合成本身是对的) ====", err_bytes);
    if (err_bytes != 0) err_cnt = err_cnt + 1;

    $display("==== 结束: line_start %0d 次 / line_done %0d 次, AR %0d 笔, AW %0d 笔, R 收 %0d 拍, W 收 %0d 拍, err=%0d ====",
             line_start_cnt, line_done_cnt, rd_bursts, wr_bursts, r_beats, w_beats, err_cnt);
    if ((err_cnt == 0) && (fill_err == 0) && (blit_ls == HEIGHT) && (blit_ld == HEIGHT)
        && (cmb_done_lines == CMB_H))
        $display("==== PASS: DDR 读回来 → 合成 → 写回 DDR，逐字节一致、遮头遮尾正确 ====");
    else
        $display("==== FAIL ====");
    $finish;
end

initial begin
    #30_000_000;
    $display("!! 全局超时 —— 链路挂住了（line_start %0d / line_done %0d / AR %0d / AW %0d）",
             line_start_cnt, line_done_cnt, rd_bursts, wr_bursts);
    $finish;
end

endmodule
