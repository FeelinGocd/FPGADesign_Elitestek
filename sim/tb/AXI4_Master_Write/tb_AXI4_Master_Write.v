////////////////////////////////////////////////////////////////////////////////
// tb_AXI4_Master_Write.v —— AXI4 写通路验证（假 DDR 从机 + 可读写内存模型）
//
//  为什么要单独验
//    写通路只在 DDR checker 时代上板跑过，而 checker 的用法是：
//        wr_strb = 全 1（整拍写）、一口气连续写 256 拍、从机 ready 常高
//    加速器的用法不一样，用到 checker 从没覆盖过的三件事：
//        (1) WSTRB 部分字节写（非对齐行的首尾遮罩）
//        (2) 写被反压（WREADY 拉低）时 payload 必须保持不变
//        (3) 小突发 + 跳跨距再来一笔（逐行写）
//
//  验什么
//    1) 单笔对齐写: 数据逐字节落对位置, cmd_done 恰好一拍
//    2) WSTRB 部分字节: 只写使能的那些字节, 其余字节一动不动
//    3) 逐行两笔: 地址跳跃, 每笔独立握手
//    4) 反压: WREADY 周期性拉低时 wr_data/wr_strb/wr_last 稳定
//    5) WLAST 只出现在最后一拍; cmd_done 每笔恰好一拍
//
//  编译运行(ModelSim)
//    vlog AXI4_Master_Write.v ; vlog tb_AXI4_Master_Write.v
//    vsim -c tb_AXI4_Master_Write -do "run -all; quit -f"
////////////////////////////////////////////////////////////////////////////////
`timescale 1ns/100ps

module tb_AXI4_Master_Write;

localparam DATA_WIDTH  = 128;
localparam STRB_WIDTH  = DATA_WIDTH/8;        // 16
localparam ADDR_WIDTH  = 32;
localparam ID_W    = 8;
localparam LEN_WIDTH   = 8;
localparam CLK_HALF = 5.0;
localparam MEM_BYTES = 65536;         // 64KB 假内存
localparam [7:0] MEM_INIT = 8'h5A;    // 没被写过的字节应保持它

reg clk, rst_n;

// ---- 命令口 ----
reg                    cmd_valid;
wire                   cmd_ready;
reg  [ADDR_WIDTH-1:0]      cmd_addr;
reg  [LEN_WIDTH-1:0]       cmd_len;
wire                   cmd_done;
wire                   cmd_err;

// ---- 写数据口(TB 当数据源, 拉式)----
reg  [DATA_WIDTH-1:0]      wr_data;
reg  [STRB_WIDTH-1:0]      wr_strb;
wire                   wr_valid;
wire                   wr_ready;

// ---- AW / W / B ----
wire [ID_W-1:0]        m_AXI_AWID;
wire [ADDR_WIDTH-1:0]      m_AXI_AWADDR;
wire [LEN_WIDTH-1:0]       m_AXI_AWLEN;
wire [2:0]             m_AXI_AWSIZE;
wire [1:0]             m_AXI_AWBURST;
wire                   m_AXI_AWLOCK;
wire [3:0]             m_AXI_AWCACHE;
wire [2:0]             m_AXI_AWPROT;
wire [3:0]             m_AXI_AWQOS;
wire [3:0]             m_AXI_AWREGION;
wire                   m_AXI_AWVALID;
wire                   m_AXI_AWREADY;
wire [DATA_WIDTH-1:0]      m_AXI_WDATA;
wire [STRB_WIDTH-1:0]      m_AXI_WSTRB;
wire                   m_AXI_WLAST;
wire                   m_AXI_WVALID;
wire                   m_AXI_WREADY;
wire [ID_W-1:0]        m_AXI_BID;
wire [1:0]             m_AXI_BRESP;
wire                   m_AXI_BVALID;
wire                   m_AXI_BREADY;

AXI4_Master_Write #(
    .ADDR_WIDTH (ADDR_WIDTH),
    .DATA_WIDTH (DATA_WIDTH),
    .STRB_WIDTH (STRB_WIDTH),
    .ID_WIDTH   (ID_W),
    .LEN_WIDTH  (LEN_WIDTH),
    .AXI_ID     ({ID_W{1'b0}})
) u_dut (
    .clk                (clk),
    .rst_n              (rst_n),
    .cmd_valid          (cmd_valid),
    .cmd_ready          (cmd_ready),
    .cmd_addr           (cmd_addr),
    .cmd_len            (cmd_len),
    .cmd_done           (cmd_done),
    .cmd_err            (cmd_err),
    .wr_data            (wr_data),
    .wr_strb            (wr_strb),
    .wr_valid           (wr_valid),
    .wr_ready           (wr_ready),
    .m_AXI_AWID         (m_AXI_AWID),
    .m_AXI_AWADDR       (m_AXI_AWADDR),
    .m_AXI_AWLEN        (m_AXI_AWLEN),
    .m_AXI_AWSIZE       (m_AXI_AWSIZE),
    .m_AXI_AWBURST      (m_AXI_AWBURST),
    .m_AXI_AWLOCK       (m_AXI_AWLOCK),
    .m_AXI_AWCACHE      (m_AXI_AWCACHE),
    .m_AXI_AWPROT       (m_AXI_AWPROT),
    .m_AXI_AWQOS        (m_AXI_AWQOS),
    .m_AXI_AWREGION     (m_AXI_AWREGION),
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
    .m_AXI_BREADY       (m_AXI_BREADY)
);

integer err_cnt;
integer i, j, k;

//==============================================================================
// 假 DDR 从机(可写): 收 AW → 收 W(按 WSTRB 逐字节落内存) → 回 B
//==============================================================================
reg  [7:0]  mem [0:MEM_BYTES-1];
reg  [7:0]  exp_byte [0:MEM_BYTES-1];       // 期望模型

reg  [ADDR_WIDTH-1:0]   aw_addr_r;
reg  [LEN_WIDTH-1:0]    aw_len_r;
reg                 aw_active;
reg  [9:0]          beat_cnt;
reg                 b_valid_r;
reg                 slv_w_ready_en;
reg                 bp_en;                  // 反压开关
reg  [3:0]          bp_cnt;

assign m_AXI_AWREADY = ~aw_active;
assign m_AXI_WREADY  = slv_w_ready_en && aw_active;
assign m_AXI_BID     = {ID_W{1'b0}};
assign m_AXI_BRESP   = 2'b00;
assign m_AXI_BVALID  = b_valid_r;

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        aw_addr_r <= 32'd0; aw_len_r <= 8'd0; aw_active <= 1'b0;
        beat_cnt  <= 10'd0; b_valid_r <= 1'b0;
    end
    else begin
        if (m_AXI_AWVALID && m_AXI_AWREADY) begin
            aw_addr_r <= m_AXI_AWADDR;
            aw_len_r  <= m_AXI_AWLEN;
            aw_active <= 1'b1;
            beat_cnt  <= 10'd0;
        end
        if (m_AXI_WVALID && m_AXI_WREADY) begin
            for (k = 0; k < 16; k = k + 1)
                if (m_AXI_WSTRB[k])
                    mem[aw_addr_r + beat_cnt*16 + k] <= m_AXI_WDATA[k*8 +: 8];
            beat_cnt <= beat_cnt + 10'd1;
            if (m_AXI_WLAST) begin
                if (beat_cnt != {2'b0, aw_len_r}) begin
                    $display("FAIL: WLAST 落在第 %0d 拍, 但 AWLEN=%0d", beat_cnt, aw_len_r);
                    err_cnt = err_cnt + 1;
                end
                aw_active <= 1'b0;
                b_valid_r <= 1'b1;
            end
        end
        if (b_valid_r && m_AXI_BREADY) b_valid_r <= 1'b0;
    end
end

// 反压: 每 4 拍拉低 1 拍 WREADY
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin bp_cnt <= 4'd0; slv_w_ready_en <= 1'b1; end
    else if (bp_en) begin
        bp_cnt <= bp_cnt + 4'd1;
        slv_w_ready_en <= (bp_cnt[1:0] != 2'b00);
    end
    else begin bp_cnt <= 4'd0; slv_w_ready_en <= 1'b1; end
end

// 反压期间 payload 必须保持
reg              stall_q;
reg [DATA_WIDTH-1:0] stall_data_q;
reg [STRB_WIDTH-1:0] stall_strb_q;
reg              stall_last_q;
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        stall_q <= 1'b0; stall_data_q <= 0; stall_strb_q <= 0; stall_last_q <= 1'b0;
    end
    else if (m_AXI_WVALID && !m_AXI_WREADY) begin
        if (stall_q && (m_AXI_WDATA !== stall_data_q ||
                       m_AXI_WSTRB !== stall_strb_q ||
                       m_AXI_WLAST !== stall_last_q)) begin
            $display("FAIL: W 被反压期间 payload 变了  t=%0t", $time);
            err_cnt = err_cnt + 1;
        end
        stall_q      <= 1'b1;
        stall_data_q <= m_AXI_WDATA;
        stall_strb_q <= m_AXI_WSTRB;
        stall_last_q <= m_AXI_WLAST;
    end
    else stall_q <= 1'b0;
end

//==============================================================================
// 数据源: 第 b 拍第 j 字节 = (b*16+j) & 0xFF
//==============================================================================
reg  [8:0]      src_beat;
reg             src_active;
reg  [1:0]      strb_mode;      // 0=全1  1=首拍遮头+末拍遮尾  2=偶有整拍不写
reg  [3:0]      head_mask_n;    // 遮掉前 n 个字节
reg  [3:0]      tail_keep_n;    // 末拍只写前 n 个字节
reg  [8:0]      cur_len;

wire [STRB_WIDTH-1:0] strb_all  = {STRB_WIDTH{1'b1}};
reg  [STRB_WIDTH-1:0] mask_head;   // 首拍遮头掩码(在任务里用阻塞赋值算, 避免 delta 竞争)
reg  [STRB_WIDTH-1:0] mask_tail;   // 末拍只写低 n 字节的掩码

assign wr_valid = src_active;

always @(*) begin
    wr_data = {DATA_WIDTH{1'b0}};
    for (j = 0; j < 16; j = j + 1)
        wr_data[j*8 +: 8] = (src_beat * 16 + j) & 8'hFF;
end

always @(*) begin
    wr_strb = strb_all;
    case (strb_mode)
        2'd1: begin
            if (src_beat == 9'd0)         wr_strb = mask_head;
            else if (src_beat == cur_len) wr_strb = mask_tail;
        end
        2'd2: if (src_beat[1:0] == 2'b01) wr_strb = {STRB_WIDTH{1'b0}};
        default: ;
    endcase
end

// 期望模型: 掩码全部由参数传入(不读模块内部的任何信号, 避免作用域/delta 问题)
task expect_cmd;
    input [ADDR_WIDTH-1:0] a;
    input [8:0]        l;
    input [1:0]        mode;
    input [STRB_WIDTH-1:0] m_head;
    input [STRB_WIDTH-1:0] m_tail;
    integer b, kk;
    reg [STRB_WIDTH-1:0] s;
    begin
        for (b = 0; b <= l; b = b + 1) begin
            s = {STRB_WIDTH{1'b1}};
            case (mode)
                2'd1: begin
                    if (b == 0)   s = m_head;
                    else if (b == l) s = m_tail;
                end
                2'd2: if (b[1:0] == 2'b01) s = {STRB_WIDTH{1'b0}};
                default: ;
            endcase
            for (kk = 0; kk < 16; kk = kk + 1)
                if (s[kk]) exp_byte[a + b*16 + kk] = ((b*16 + kk) & 8'hFF);
        end
    end
endtask

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin src_beat <= 9'd0; src_active <= 1'b0; end
    else if (wr_valid && wr_ready) begin
        if (src_beat == cur_len) begin src_active <= 1'b0; src_beat <= 9'd0; end
        else src_beat <= src_beat + 9'd1;
    end
end

//==============================================================================
// cmd_done 计数(每笔命令应当恰好一拍)
//==============================================================================
integer done_cnt;
always @(posedge clk or negedge rst_n)
    if (!rst_n) done_cnt <= 0;
    else if (cmd_done) done_cnt <= done_cnt + 1;

//==============================================================================
// 激励
//==============================================================================
integer n_cmds;

task run_cmd;
    input [ADDR_WIDTH-1:0] a;
    input [8:0]        l;
    input [1:0]        mode;
    input [3:0]        hn;
    input [3:0]        tn;
    integer wd;
    begin
        cur_len     = l;
        strb_mode   = mode;
        head_mask_n = hn;
        tail_keep_n = tn;
        mask_head   = strb_all << hn;                      // 遮掉低 hn 个字节
        mask_tail   = (16'h0001 << tn) - 16'h0001;         // 只保留低 tn 个字节
        expect_cmd(a, l, mode, mask_head, mask_tail);
        n_cmds      = n_cmds + 1;
        if (mode == 2'd1)
            $display("     (首拍掩码=0x%04x 末拍掩码=0x%04x)", mask_head, mask_tail);
        @(posedge clk);
        cmd_addr  = a;
        cmd_len   = l[7:0];
        cmd_valid = 1'b1;
        @(posedge clk);
        while (!cmd_ready) @(posedge clk);
        cmd_valid = 1'b0;
        src_beat  = 9'd0;
        @(posedge clk);
        src_active = 1'b1;
        wd = 0;
        while (!cmd_done && wd < 20000) begin @(posedge clk); wd = wd + 1; end
        if (wd >= 20000) begin
            $display("FAIL: 命令 0x%08x len=%0d 超时未完成", a, l);
            err_cnt = err_cnt + 1;
        end
        repeat (4) @(posedge clk);
    end
endtask

task check_mem;
    integer bad;
    begin
        bad = 0;
        for (i = 0; i < MEM_BYTES; i = i + 1) begin
            if (mem[i] !== exp_byte[i]) begin
                if (bad < 10)
                    $display("    内存不符 mem[0x%04x] = 0x%02x, 期望 0x%02x", i, mem[i], exp_byte[i]);
                bad = bad + 1;
            end
        end
        if (bad == 0) $display("PASS: 内存逐字节比对通过(全 %0d 字节)", MEM_BYTES);
        else begin $display("FAIL: 共 %0d 个字节不一致", bad); err_cnt = err_cnt + 1; end
    end
endtask

initial begin
    clk = 1'b0;
    forever #(CLK_HALF) clk = ~clk;
end

initial begin
    err_cnt = 0; n_cmds = 0;
    rst_n = 1'b0; cmd_valid = 1'b0; cmd_addr = 0; cmd_len = 0;
    src_active = 1'b0; src_beat = 0; strb_mode = 2'd0;
    head_mask_n = 4'd5; tail_keep_n = 4'd4; bp_en = 1'b0;
    for (i = 0; i < MEM_BYTES; i = i + 1) begin
        mem[i]      = MEM_INIT;
        exp_byte[i] = MEM_INIT;
    end
    repeat (10) @(posedge clk);
    rst_n = 1'b1;
    repeat (5) @(posedge clk);

    $display("---- 用例1: 对齐整拍写, 16 拍 = 256 字节 ----");
    run_cmd(32'h0000_0000, 9'd15, 2'd0, 4'd0, 4'd0);

    $display("---- 用例2: WSTRB 部分字节(首拍遮 5 字节 / 末拍只写 4 字节) ----");
    run_cmd(32'h0000_0100, 9'd3, 2'd1, 4'd5, 4'd4);

    $display("---- 用例3: 逐行两笔(各 180 拍 = 2880 字节, 地址跳 0x1680) ----");
    run_cmd(32'h0000_2000, 9'd179, 2'd0, 4'd0, 4'd0);
    run_cmd(32'h0000_2000 + 32'h0000_1680, 9'd179, 2'd0, 4'd0, 4'd0);

    $display("---- 用例4: 反压(WREADY 每 4 拍低 1 拍) + 偶有整拍不写 ----");
    bp_en = 1'b1;
    run_cmd(32'h0000_5000, 9'd31, 2'd2, 4'd0, 4'd0);
    bp_en = 1'b0;

    repeat (20) @(posedge clk);

    $display("---- 结果 ----");
    check_mem();
    if (done_cnt !== n_cmds) begin
        $display("FAIL: cmd_done 脉冲数 %0d, 期望 %0d", done_cnt, n_cmds);
        err_cnt = err_cnt + 1;
    end
    else $display("PASS: cmd_done 脉冲数 = %0d, 与命令数一致", done_cnt);
    if (cmd_err !== 1'b0) begin
        $display("FAIL: cmd_err 应为恒 0");
        err_cnt = err_cnt + 1;
    end
    else $display("PASS: cmd_err 恒 0");

    if (err_cnt == 0) $display("\n*** 全部通过 ***");
    else              $display("\n*** 失败: err_cnt = %0d ***", err_cnt);
    $finish;
end

initial begin
    #2000000;
    $display("FAIL: 全局超时");
    $finish;
end

endmodule