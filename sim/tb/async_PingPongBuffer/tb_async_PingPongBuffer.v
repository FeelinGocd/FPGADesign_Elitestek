////////////////////////////////////////////////////////////////////////////////
// tb_Async_PingPongBuffer.v  —— async_PingPongBuffer 双时钟域功能测试
//
// 测试内容
//   写侧：clk_wr 域连续写入递增数据（从 0 开始），每 BLOCK 拍给一次 block_done
//   读侧：clk_rd 域把数据取出来，逐拍比对是否等于同一个递增序列
//         读激励按真实 HDMI 的形状给：有效区连续取一串（取满 BLOCK 拍），取完进消隐
//   两个时钟相互独立，复位为单源异步断言
//
// 通过标准
//   读出的 READ_TOTAL 个数据与期望序列完全一致，且不超时
//
// 编译 / 仿真（ModelSim）
//   vlib work
//   vlog <工程>/project/05_SoC_AXI/RTL/Peripherial/LineBuffer/PingPongBuffer/async_fifo.v
//   vlog <工程>/project/05_SoC_AXI/RTL/Peripherial/LineBuffer/PingPongBuffer/BlockRAM.v
//   vlog <工程>/project/05_SoC_AXI/RTL/Peripherial/LineBuffer/PingPongBuffer/async_PingPongBuffer.v
//   vlog tb_Async_PingPongBuffer.v
//   vsim -c tb_Async_PingPongBuffer -do "run -all; quit -f"
//
// 说明：本测试只验证"双缓冲 + 跨时钟域"本身，块大小/位宽都是简化值；
//       真实工程用 DATA_WIDTH=128 / DEPTH=512 / 一行 360 拍。
////////////////////////////////////////////////////////////////////////////////
`timescale 1ns/100ps

module tb_async_PingPongBuffer;

//==============================================================================
// 参数
//==============================================================================
localparam DW          = 32;        // FIFO 数据位宽
localparam DEPTH       = 64;        // 每个 bank 的深度（必须是 2^n）
localparam BLOCK       = 16;        // 每块拍数（写侧 done 的间隔）

localparam CLK_WR_HALF = 5.0;       // 写时钟 100MHz
localparam CLK_RD_HALF = 4.0;       // 读时钟 125MHz（真实工程 148.5MHz）

localparam READ_TOTAL  = 256;       // 总共校验多少个数据（= 16 块）
localparam TIMEOUT     = 100000;    // 看门狗（ns）

//==============================================================================
// 时钟与复位
//==============================================================================
reg  clk_wr;
reg  clk_rd;
reg  rst_n;

initial begin
    clk_wr = 1'b0;
    forever #CLK_WR_HALF clk_wr = ~clk_wr;
end

initial begin
    clk_rd = 1'b0;
    forever #CLK_RD_HALF clk_rd = ~clk_rd;
end

initial begin
    rst_n = 1'b0;
    repeat (10) @(posedge clk_wr);
    @(negedge clk_rd);
    rst_n = 1'b1;
end

//==============================================================================
// DUT
//==============================================================================
wire [DW-1:0]   wr_data;
wire            wr_en;
wire            wr_done;
wire            wr_ready;

wire [DW-1:0]   rd_data;
wire            rd_en;
wire            rd_valid;
wire            rd_ready;

wire            buf_select;

async_PingPongBuffer #(
    .DATA_WIDTH             (DW),
    .DEPTH                  (DEPTH)
) u_dut (
    .clk_wr                 (clk_wr),
    .clk_rd                 (clk_rd),
    .rst_n                  (rst_n),

    .buffer_wr_en           (wr_en),
    .buffer_wr_data         (wr_data),
    .buffer_wr_block_done   (wr_done),
    .buffer_wr_block_ready  (wr_ready),

    .buffer_rd_en           (rd_en),
    .buffer_rd_data         (rd_data),
    .buffer_rd_valid        (rd_valid),
    .buffer_rd_block_ready  (rd_ready),

    .buf_select             (buf_select)
);

//==============================================================================
// 写侧：DUT 说能收就送下一个数；每 BLOCK 拍在最后一拍同拍给 done
//==============================================================================
reg  [DW-1:0]   wr_cnt;         // 待写入的数（从 0 开始递增）
reg  [15:0]     wr_blk_cnt;     // 本块已写拍数

wire            wr_fire = wr_ready;     // 这一拍 DUT 一定会收下

assign  wr_en   = wr_fire;
assign  wr_data = wr_cnt;
assign  wr_done = wr_fire && (wr_blk_cnt == BLOCK - 1);

always @(posedge clk_wr or negedge rst_n) begin
    if (!rst_n) begin
        wr_cnt     <= {DW{1'b0}};
        wr_blk_cnt <= 16'd0;
    end
    else if (wr_fire) begin
        wr_cnt     <= wr_cnt + 1'b1;
        wr_blk_cnt <= (wr_blk_cnt == BLOCK - 1) ? 16'd0 : wr_blk_cnt + 1'b1;
    end
end

//==============================================================================
// 读侧：模拟真实 HDMI —— 有效区连续取一串，取满 BLOCK 拍后进消隐
//   有效区：每 POP_DIV 个读时钟取一拍（真实工程一个 128bit 拍 ≈ 5.33 个像素）
//   消隐期：BLANK_LEN 个读时钟完全不取数（行消隐/场消隐的简化）
//==============================================================================
localparam POP_DIV   = 6;                                   // 取数间隔（读时钟数）
localparam BLANK_LEN = 200;                                 // 消隐长度（读时钟数）
localparam LINE_LEN  = BLOCK * POP_DIV + BLANK_LEN;         // 一个"行"的总长

reg  [3:0]      rd_div;
reg  [15:0]     line_cnt;

wire            rd_active   = (line_cnt < BLOCK * POP_DIV); // 有效区
wire            rd_stb      = rd_active && (rd_div == 4'd0);
wire            rd_line_end = (line_cnt == LINE_LEN - 1);

always @(posedge clk_rd or negedge rst_n) begin
    if (!rst_n) begin
        rd_div   <= 4'd0;
        line_cnt <= 16'd0;
    end
    else begin
        // 行首把分频相位清零，保证每个有效区正好取 BLOCK 拍
        rd_div   <= (rd_line_end || (rd_div == POP_DIV - 1)) ? 4'd0 : rd_div + 1'b1;
        line_cnt <= rd_line_end ? 16'd0 : line_cnt + 1'b1;
    end
end

assign  rd_en = rd_stb;

//==============================================================================
// 数据比对
//   buffer_rd_valid 与 buffer_rd_data 同拍，都是"该拍弹出一拍"后延后一拍的结果
//==============================================================================
reg  [DW-1:0]   exp_cnt;
integer         rd_num;
integer         err_cnt;

initial begin
    exp_cnt = {DW{1'b0}};
    rd_num  = 0;
    err_cnt = 0;
end

always @(posedge clk_rd) begin
    if (rst_n && rd_valid && (rd_num < READ_TOTAL)) begin
        if (rd_data !== exp_cnt) begin
            err_cnt = err_cnt + 1;
            if (err_cnt <= 10)
                $display("  [%0t] MISMATCH: got %0d, expected %0d", $time, rd_data, exp_cnt);
        end
        exp_cnt <= exp_cnt + 1'b1;
        rd_num  = rd_num + 1;
    end
end

//==============================================================================
// 观测：读 bank 每换一次打一行，便于确认乒乓在正常翻转
//==============================================================================
always @(buf_select)
    $display("  [%0t] read bank -> %0d   (read %0d)", $time, buf_select, rd_num);

`ifdef DUMP_VCD
initial begin
    $dumpfile("tb_Async_PingPongBuffer.vcd");
    $dumpvars(0, tb_Async_PingPongBuffer);
end
`endif

//==============================================================================
// 结束判定
//==============================================================================
task report_result;
    input       is_timeout;
    begin
        if (is_timeout || (err_cnt != 0))
            $display("=== FAIL : read %0d / %0d, errors = %0d ===", rd_num, READ_TOTAL, err_cnt);
        else
            $display("=== PASS : read %0d / %0d, all data matched ===", rd_num, READ_TOTAL);
        $finish;
    end
endtask

initial begin
    $display("=== tb_Async_PingPongBuffer start ===");
    $display("    DW=%0d  DEPTH=%0d  BLOCK=%0d  READ_TOTAL=%0d",
             DW, DEPTH, BLOCK, READ_TOTAL);
    $display("    read : active %0d beats in %0d clk (POP_DIV=%0d), blank %0d clk",
             BLOCK, BLOCK*POP_DIV, POP_DIV, BLANK_LEN);
    @(posedge rst_n);
    wait (rd_num >= READ_TOTAL);
    report_result(1'b0);
end

initial begin
    #TIMEOUT;
    $display("  [TIMEOUT] only read %0d of %0d", rd_num, READ_TOTAL);
    report_result(1'b1);
end

endmodule
