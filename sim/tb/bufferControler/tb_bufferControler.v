////////////////////////////////////////////////////////////////////////////////
// tb_bufferControler.v —— 行预取控制器验证（假 DDR + 假 PingPong）
//
//  验什么
//    1) 每笔 ≤256 拍、不跨 4KB 线、cmd_write 恒 0
//    2) 全屏: 每行 241 拍(240 + 1 拍对齐余量), block_done 与最后一拍同拍
//       ★ 行首地址 【向下 16B 对齐】后再发命令, line_skip = 行首 addr[3:0]
//    3) 行基址逐行 +ROW_STRIDE(5760), 行号 0..FRAME_LINES-1
//    4) 帧尾停手: 收完最后一行不再发命令, 直到 frame_start
//    5) cmd_err: 本行补 0 到 241 拍, 块大小仍然精确
//    6) enable=0 不出命令; 读侧没让出 bank(block_ready=0) 也不出命令
//    7) 分屏(mode=1): 一行拆两段各 120 拍(1920B), 段0/段1 各自 +=2880,
//       逐笔地址连续; 分屏不做字节对齐, line_skip 恒 0
//
//  编译运行(ModelSim)
//    vlog bufferControler.v ; vlog tb_bufferControler.v
//    vsim -c tb_bufferControler -do "run -all; quit -f"
////////////////////////////////////////////////////////////////////////////////
`timescale 1ns/100ps

module tb_bufferControler;

localparam  LINE_BYTES  = 3856;                 // 3840(240 拍) + 16 对齐余量
localparam  ROW_STRIDE  = 5760;                 // 物理行距(1080p 布局)
localparam  LINE_BEATS  = LINE_BYTES/16;        // 241
parameter   FRAME_LINES = 4;                    // 默认缩到 4 行
localparam  AR_LAT      = 4;                    // 假 DDR 的命令往返
localparam  BASE0       = 32'h0020_0A05;        // 行首 skip = 5，验证对齐读取
localparam  BASE1       = 32'h0030_0A05;

// 分屏(mode=1)用: 一行两段, 每段 120 拍 = 1920 字节
localparam  SEG_BYTES   = 1920;
localparam  SEG_BEATS   = SEG_BYTES/16;         // 120
localparam  SEG_STRIDE  = ROW_STRIDE/2;         // 2880
localparam  SEG0_BASE   = 32'h0010_0000;        // 段0 页内偏移 0    -> 一笔 120 拍
localparam  SEG1_BASE   = 32'h0018_0800;        // 段1 页内偏移 2048 -> 拆 128+52 两笔
localparam  SPLIT_LINES = 4;                    // 分屏验收几行

integer     err_row;                            // 故意让 AXI 报错的行号(-1 = 不造错)
integer     err_burst1;                          // 该行第一笔的拍数(算应补几拍用)

integer exp_line;                               // 期望的行号(假从机和检查都要用)
localparam  CLK_HALF    = 5.0;

reg         clk, rst_n;
reg         lineBuffer_enable, lineBuffer_mode;
reg  [63:0] lineBuffer_addr;
reg         lineBuffer_frame_start;
reg         buffer_wr_block_ready;
reg         slv_err_en;                         // 让某一笔报错

wire        buffer_wr_en;
wire [127:0] buffer_wr_data;
wire        buffer_wr_block_done;
wire [3:0]  line_skip;
wire        cmd_valid, cmd_write;
wire [31:0] cmd_addr;
wire [7:0]  cmd_len;
wire        cmd_ready, cmd_done, cmd_err;
wire [127:0] rd_data;
reg         rd_valid, rd_last;                  // 假 DDR 在 always 里驱动
wire        rd_ready;

integer     err_cnt;

initial begin clk = 1'b0; forever #CLK_HALF clk = ~clk; end

bufferControler #(
    .AXI_ADDR_WIDTH (32),
    .AXI_DATA_WIDTH (128),
    .AXI_LEN_WIDTH  (8),
    .LINE_BYTES     (LINE_BYTES),
    .ROW_STRIDE_BYTES (ROW_STRIDE),
    .FRAME_LINES    (FRAME_LINES),
    .LINE_WIDTH     (11)
) u_dut (
    .clk                        (clk),
    .rst_n                      (rst_n),
    .lineBuffer_enable          (lineBuffer_enable),
    .lineBuffer_mode            (lineBuffer_mode),
    .lineBuffer_addr            (lineBuffer_addr),
    .lineBuffer_frame_start     (lineBuffer_frame_start),
    .line_skip                  (line_skip),
    .buffer_wr_en               (buffer_wr_en),
    .buffer_wr_data             (buffer_wr_data),
    .buffer_wr_block_done       (buffer_wr_block_done),
    .buffer_wr_block_ready      (buffer_wr_block_ready),
    .cmd_valid                  (cmd_valid),
    .cmd_write                  (cmd_write),
    .cmd_addr                   (cmd_addr),
    .cmd_len                    (cmd_len),
    .cmd_ready                  (cmd_ready),
    .cmd_done                   (cmd_done),
    .cmd_err                    (cmd_err),
    .rd_data                    (rd_data),
    .rd_valid                   (rd_valid),
    .rd_last                    (rd_last),
    .rd_ready                   (rd_ready)
);

//------------------------------------------------------------------------------
// 假 DDR: 收命令 → 等 AR_LAT → 按地址吐数据, 最后一拍 rd_last
//   每个字节 = 该字节地址的低 8 位，方便对齐检查
//------------------------------------------------------------------------------
reg  [31:0] slv_addr;
reg  [9:0]  slv_left;
reg  [3:0]  slv_lat;
reg         slv_busy;

assign cmd_ready = ~slv_busy;
assign cmd_done  = rd_valid && rd_last;          // 简化: 末拍即命令完成
assign cmd_err   = slv_err_en;                   // 场景控制

genvar gi;
generate
    for (gi = 0; gi < 16; gi = gi + 1) begin : g_bytes
        localparam [7:0] OFF = gi;
        assign rd_data[gi*8 +: 8] = slv_addr[7:0] + OFF;
    end
endgenerate

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        slv_addr <= 32'd0; slv_left <= 10'd0; slv_lat <= 4'd0; slv_busy <= 1'b0;
        rd_valid <= 1'b0; rd_last <= 1'b0;
    end
    else begin
        rd_valid <= 1'b0;
        rd_last  <= 1'b0;
        if (!slv_busy) begin
            if (cmd_valid && cmd_ready) begin
                slv_addr <= cmd_addr;
                slv_left <= cmd_len + 10'd1;
                slv_lat  <= AR_LAT[3:0];
                slv_busy <= 1'b1;
            end
        end
        else if (slv_lat != 4'd0) begin
            slv_lat <= slv_lat - 4'd1;
        end
        else if (slv_left != 10'd0) begin
            rd_valid <= 1'b1;
            rd_last  <= (slv_left == 10'd1);
            slv_addr <= slv_addr + 32'd16;
            slv_left <= slv_left - 10'd1;
            if (slv_left == 10'd1) slv_busy <= 1'b0;
        end
    end
end

//------------------------------------------------------------------------------
// 假 PingPong 读侧: block_done 之后把 block_ready 拉低几拍(让出 bank)
//------------------------------------------------------------------------------
reg  [3:0] ready_gap;
initial begin buffer_wr_block_ready = 1'b1; ready_gap = 4'd0; end
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin buffer_wr_block_ready <= 1'b1; ready_gap <= 4'd0; end
    else if (buffer_wr_block_done) begin buffer_wr_block_ready <= 1'b0; ready_gap <= 4'd4; end
    else if (ready_gap != 4'd0) begin
        ready_gap <= ready_gap - 4'd1;
        if (ready_gap == 4'd1) buffer_wr_block_ready <= 1'b1;
    end
end

//------------------------------------------------------------------------------
// 检查
//------------------------------------------------------------------------------
localparam  EXP_BEATS_FULL  = LINE_BEATS;          // 241
localparam  EXP_BEATS_SPLIT = SEG_BEATS*2;         // 240

integer line_beats;         // 本行已写入的拍数
integer lines_done;         // 已交完的块数
integer cmd_cnt;            // 命令笔数
integer cmd_during_wait;    // 在读侧没让位时发命令的次数(应为 0)
integer cmd_when_disabled;  // enable=0 时发命令的次数(应为 0)
integer zero_beats;         // 本行写进去的全零拍数(出错行用来核对补零)
integer split_addr_err;     // 分屏模式: 每笔起始地址不符的次数(应为 0)
integer skip_err;           // line_skip 不符的次数(应为 0)
integer beatalign_err;      // 全屏首发地址没向下 16B 对齐的次数(应为 0)
integer split_base;         // 进分屏用例那一刻的 lines_done(用来判断跑够了没)
reg  [31:0] split_exp_addr; // 分屏模式: 本笔应有的起始地址

task check(input [255:0] name, input integer got, input integer exp);
begin
    if (got !== exp) begin
        err_cnt = err_cnt + 1;
        $display("  FAIL %0s: got=%0d exp=%0d @%0t", name, got, exp, $time);
    end
end
endtask

initial begin
    line_beats = 0; lines_done = 0; cmd_cnt = 0; exp_line = 0;
    cmd_during_wait = 0; cmd_when_disabled = 0; err_cnt = 0;
    zero_beats = 0; err_row = -1; err_burst1 = 0;
    split_addr_err = 0; split_base = 0; split_exp_addr = 32'd0;
    skip_err = 0; beatalign_err = 0;
end

// 命令检查
always @(posedge clk) begin
    if (rst_n && cmd_valid && cmd_ready) begin
        cmd_cnt = cmd_cnt + 1;
        if (cmd_write !== 1'b0) begin err_cnt = err_cnt + 1; $display("  FAIL cmd_write != 0"); end
        if ((cmd_len + 1) > 256) begin err_cnt = err_cnt + 1; $display("  FAIL 单笔超过 256 拍: %0d", cmd_len+1); end
        if ((cmd_addr[11:0] + (cmd_len + 1) * 16) > 4096) begin
            err_cnt = err_cnt + 1;
            $display("  FAIL 跨 4KB: addr=%08h off=%0d len=%0d", cmd_addr, cmd_addr[11:0], cmd_len+1);
        end
        if (!lineBuffer_mode) begin
            // ---- 全屏: 一行一段 241 拍, 行距 5760, 行首向下 16B 对齐 ----
            if (line_beats == 0) begin
                if (cmd_addr !== ((BASE0 + exp_line * ROW_STRIDE) & 32'hFFFF_FFF0)) begin
                    err_cnt = err_cnt + 1;
                    beatalign_err = beatalign_err + 1;
                    $display("  FAIL 行%0d 首地址: got=%08h exp=%08h",
                             exp_line, cmd_addr, (BASE0 + exp_line*ROW_STRIDE) & 32'hFFFF_FFF0);
                end
                if (line_skip !== ((BASE0 + exp_line * ROW_STRIDE) & 32'hF)) begin
                    skip_err = skip_err + 1;
                    $display("  FAIL 行%0d line_skip: got=%0d exp=%0d",
                             exp_line, line_skip, (BASE0 + exp_line*ROW_STRIDE) & 32'hF);
                end
            end
        end
        else begin
            // ---- 分屏: 一行两段各 120 拍, 段地址各自 +=2880 ----
            if (line_beats == 0) begin
                split_exp_addr = SEG0_BASE + exp_line * SEG_STRIDE;
                if (line_skip !== 4'd0) begin
                    skip_err = skip_err + 1;
                    $display("  FAIL 分屏 行%0d line_skip 应为 0, got=%0d", exp_line, line_skip);
                end
            end
            else if (line_beats == SEG_BEATS)
                split_exp_addr = SEG1_BASE + exp_line * SEG_STRIDE;

            if (cmd_addr !== split_exp_addr) begin
                split_addr_err = split_addr_err + 1;
                if (split_addr_err <= 6)
                    $display("  FAIL 分屏 行%0d 已收%0d拍 本笔地址: got=%08h exp=%08h",
                             exp_line, line_beats, cmd_addr, split_exp_addr);
            end

            if ((line_beats < SEG_BEATS) && ((line_beats + (cmd_len + 1)) >= SEG_BEATS))
                split_exp_addr = SEG1_BASE + exp_line * SEG_STRIDE;
            else
                split_exp_addr = cmd_addr + ((cmd_len + 1) * 16);
        end
        if (!lineBuffer_mode && line_beats == 0 && exp_line == err_row)
            err_burst1 = cmd_len + 1;                   // 出错行: 记下第一笔拍数
        if (!buffer_wr_block_ready) cmd_during_wait = cmd_during_wait + 1;
        if (!lineBuffer_enable)     cmd_when_disabled = cmd_when_disabled + 1;
    end
end

// 写拍 / 收尾检查
always @(posedge clk) begin
    if (rst_n) begin
        // ★ 块结束与最后一拍同拍
        if (buffer_wr_block_done) begin
            if (buffer_wr_data == 128'd0) zero_beats = zero_beats + 1;  // 最后一拍也算了
            check("块拍数", line_beats + 1, lineBuffer_mode ? EXP_BEATS_SPLIT : EXP_BEATS_FULL);
            if (!lineBuffer_mode && exp_line == err_row) begin
                check("出错行补零拍数", zero_beats, LINE_BEATS - err_burst1);
                $display("  [info] 行%0d 出错补零: 实读 %0d 拍 + 补零 %0d 拍 = %0d",
                         exp_line, err_burst1, zero_beats, err_burst1 + zero_beats);
            end
            zero_beats = 0;
            line_beats = 0;
            lines_done = lines_done + 1;
            exp_line   = exp_line + 1;
        end
        else if (buffer_wr_en) begin
            line_beats = line_beats + 1;
            if (buffer_wr_data == 128'd0) zero_beats = zero_beats + 1;
        end
    end
end

//------------------------------------------------------------------------------
// 主流程
//------------------------------------------------------------------------------
initial begin
    rst_n = 1'b0;
    lineBuffer_enable = 1'b0; lineBuffer_mode = 1'b0;
    lineBuffer_addr = {BASE0, BASE1};
    lineBuffer_frame_start = 1'b0; slv_err_en = 1'b0;
    repeat (5) @(posedge clk);
    rst_n = 1'b1;
    repeat (3) @(posedge clk);

    // ---- 1) enable=0: 不该出命令 ----
    repeat (20) @(posedge clk);
    check("enable=0 无命令", cmd_cnt, 0);

    // ---- 2) 开使能 + 帧起始: 跑 FRAME_LINES 行 ----
    //   第 1 行故意让 AXI 报错。行首对齐后这一行【正好一笔 241 拍】，
    //   错误就报在这笔的 rd_last 上 = 行的最后一拍 —— 专门测"没什么可补"那条边
    err_row = 1;
    lineBuffer_enable = 1'b1;
    lineBuffer_frame_start = 1'b1;              // ★ 和使能同一拍给出
    @(posedge clk); lineBuffer_frame_start = 1'b0;

    wait (exp_line == 1);
    slv_err_en = 1'b1;
    wait (exp_line == 2);
    slv_err_en = 1'b0;

    // ---- 3) 帧尾停手: 收完最后一行后不该再发命令 ----
    wait (lines_done == FRAME_LINES);
    cmd_cnt = 0;
    repeat (30) @(posedge clk);
    check("帧尾停手", cmd_cnt, 0);

    // ---- 4) frame_start 之后应该重新从第 0 行开始 ----
    exp_line = 0;
    @(posedge clk); lineBuffer_frame_start = 1'b1;
    @(posedge clk); lineBuffer_frame_start = 1'b0;
    wait (lines_done == FRAME_LINES + 1);

    // ---- 5) 关使能: 不该再出命令 ----
    lineBuffer_enable = 1'b0;
    wait (buffer_wr_block_ready);
    cmd_cnt = 0;
    repeat (30) @(posedge clk);
    check("关使能后无命令", cmd_cnt, 0);

    check("读侧没让位时没发命令", cmd_during_wait, 0);
    check("enable=0 期间没发命令", cmd_when_disabled, 0);
    check("全屏首发地址对齐错次", beatalign_err, 0);
    check("line_skip 错次", skip_err, 0);

    // ---- 6) 出错落在【行中间】: 第 2 行是两笔(144 + 97) ----
    //      第 1 笔收完报错 -> 剩下 97 拍必须补零, 块仍然是 241 拍
    err_row = 2;
    lineBuffer_enable = 1'b1;
    exp_line = 0;
    @(posedge clk); lineBuffer_frame_start = 1'b1;
    @(posedge clk); lineBuffer_frame_start = 1'b0;
    split_base = lines_done;
    wait (exp_line == 2);
    slv_err_en = 1'b1;
    wait (exp_line == 3);
    slv_err_en = 1'b0;
    wait (lines_done >= (split_base + FRAME_LINES));    // 跑完这一帧再换模式
    repeat (10) @(posedge clk);
    err_row = -1;

    // ---- 7) 分屏(mode=1): 一行两段, 每段 120 拍, 段地址各自 +=2880 ----
    lineBuffer_enable = 1'b1;
    lineBuffer_mode   = 1'b1;
    lineBuffer_addr   = {SEG0_BASE, SEG1_BASE};
    exp_line          = 0;
    split_addr_err    = 0;
    @(posedge clk); lineBuffer_frame_start = 1'b1;
    @(posedge clk); lineBuffer_frame_start = 1'b0;
    split_base = lines_done;
    wait (lines_done >= (split_base + SPLIT_LINES));
    check("分屏 每笔起始地址错次", split_addr_err, 0);
    check("分屏 line_skip 错次", skip_err, 0);
    $display("  [info] 分屏: 跑完 %0d 行, 地址错次 %0d", lines_done - split_base, split_addr_err);

    if (err_cnt == 0) $display("---- ALL CHECKS PASSED ----");
    else              $display("---- FAIL: %0d errors ----", err_cnt);
    $finish;
end

initial begin
    #2_000_000;
    $display("TIMEOUT");
    $display("---- FAIL ----");
    $finish;
end

endmodule
