//==============================================================================
// tb_gpu2d_apb.v —— GPU_2D 总线层(APB0_gpu2d)的寄存器表 + 地址译码
//------------------------------------------------------------------------------
//  为什么单独有这个 TB
//      合并之后槽 2 是"一整块、一个模块"(APB0_gpu2d)：观测子块 + 显示子块
//      靠 PADDR[6] 分。子块选择、地址不命中报错、W1S 的 DONE、切模式丢旧 DONE
//      这几条都在这个模块里，用整链 TB 测不方便 —— 这里直接对着 APB 口打。
//
//  判据（全部打印, 一条不对就 FAIL）
//      1) 两个子块的 ID 各自答对: 0x2000 = "GP2D" / 0x2040 = "LB01"
//      2) 子块内的洞 (0x2020 / 0x2070) 报 PSLVERROR, 且 PREADY 恒 1
//      3) 高位非零的地址 (0x2080 / 0x2240 / 0x20C0) 报错 ——【不许】别名回子块开头
//      4) 显示子块 RW: 基址可写可读回; 低位不对齐的写被丢掉(读回还是旧值)
//      5) 观测子块只读: 写进去被忽略, 读回的是外部送进来的值
//      6) DONE_ACC: acc_frame_done 单拍置位; DONE_CPU: 软件 W1S; done_clr 清位
//      7) 切模式(SPLIT 0<->1)那一拍把两个 DONE 丢掉
//
//  跑法(ModelSim)
//    vlog -work work ...../RTL/Peripherial/APB0/GPU_2D/APB0_GPU_2D.v tb_gpu2d_apb.v
//    vsim -c -quiet -do "run -all; quit -f" tb_gpu2d_apb
//==============================================================================
`timescale 1ns/100ps

module tb_gpu2d_apb;

localparam  GPU2D_ID      = 32'h47503244;       // "GP2D"
localparam  LINEBUFFER_ID = 32'h4C423031;       // "LB01"
localparam  CLK_HALF      = 5.0;                // 100 MHz

reg          clk, rst_n;
integer      err_cnt, n_chk;

//---- APB 主机侧 ----
reg  [11:0]  PADDR;
reg          PSEL, PENABLE, PWRITE;
reg  [31:0]  PWDATA;
wire [31:0]  PRDATA;
wire         PREADY, PSLVERROR;

//---- 功能侧(显示子块) ----
reg          acc_frame_done;
reg  [1:0]   done_clr;
reg  [31:0]  fps_acc_val, fps_cpu_val;
wire         enable, split_pending;
wire [63:0]  framea_base_addr, frameb_base_addr, frame_full_base_addr;
wire         done_acc, done_cpu;

//---- 功能侧(观测子块) ----
reg  [31:0]  busy_val, cmd_done_val, frame_done_val;
reg  [31:0]  wr_beats_val, rd_beats_val, busy_cyc_val;

//---- 读回暂存 ----
reg  [31:0]  rd;
reg          err;

initial begin clk = 1'b0; forever #CLK_HALF clk = ~clk; end

//==============================================================================
// DUT
//==============================================================================
APB0_gpu2d #(
    .ADDR_WIDTH                 (32)
) u_dut (
    .clk                        (clk),
    .rst_n                      (rst_n),
    .PADDR                      (PADDR),
    .PSEL                       (PSEL),
    .PENABLE                    (PENABLE),
    .PWRITE                     (PWRITE),
    .PWDATA                     (PWDATA),
    .PRDATA                     (PRDATA),
    .PREADY                     (PREADY),
    .PSLVERROR                  (PSLVERROR),

    .acc_frame_done             (acc_frame_done),
    .done_clr                   (done_clr),
    .fps_acc_val                (fps_acc_val),
    .fps_cpu_val                (fps_cpu_val),
    .enable                     (enable),
    .split_pending              (split_pending),
    .framea_base_addr           (framea_base_addr),
    .frameb_base_addr           (frameb_base_addr),
    .frame_full_base_addr       (frame_full_base_addr),
    .done_acc                   (done_acc),
    .done_cpu                   (done_cpu),
    .busy_val                   (busy_val),
    .cmd_done_val               (cmd_done_val),
    .frame_done_val             (frame_done_val),
    .wr_beats_val               (wr_beats_val),
    .rd_beats_val               (rd_beats_val),
    .busy_cyc_val               (busy_cyc_val)
);

//==============================================================================
// 检查
//==============================================================================
task check(input [255:0] name, input [31:0] got, input [31:0] exp);
begin
    n_chk = n_chk + 1;
    if (got !== exp) begin
        err_cnt = err_cnt + 1;
        $display("  FAIL %0s: got=0x%08x exp=0x%08x @%0t", name, got, exp, $time);
    end
    else
        $display("  [ok] %0s = 0x%08x", name, got);
end
endtask

// 一次 APB 传输(两拍: SETUP -> ACCESS; 从机 PREADY 恒 1)
task apb_read(input [11:0] a);
begin
    @(negedge clk);
    PADDR = a; PWRITE = 1'b0; PWDATA = 32'd0;
    PSEL  = 1'b1; PENABLE = 1'b0;
    @(negedge clk);
    PENABLE = 1'b1;
    @(negedge clk);
    rd  = PRDATA;
    err = PSLVERROR | ~PREADY;
    PSEL = 1'b0; PENABLE = 1'b0;
    @(negedge clk);
end
endtask

task apb_write(input [11:0] a, input [31:0] d);
begin
    @(negedge clk);
    PADDR = a; PWRITE = 1'b1; PWDATA = d;
    PSEL  = 1'b1; PENABLE = 1'b0;
    @(negedge clk);
    PENABLE = 1'b1;
    @(negedge clk);
    err = PSLVERROR | ~PREADY;
    PSEL = 1'b0; PENABLE = 1'b0; PWRITE = 1'b0;
    @(negedge clk);
end
endtask

//==============================================================================
// 主流程
//==============================================================================
initial begin
    err_cnt = 0; n_chk = 0;
    PADDR = 12'd0; PSEL = 1'b0; PENABLE = 1'b0; PWRITE = 1'b0; PWDATA = 32'd0;
    acc_frame_done = 1'b0; done_clr = 2'd0;
    fps_acc_val = 32'd0; fps_cpu_val = 32'd0;
    busy_val = 32'd0; cmd_done_val = 32'd0; frame_done_val = 32'd0;
    wr_beats_val = 32'd0; rd_beats_val = 32'd0; busy_cyc_val = 32'd0;

    rst_n = 1'b0;
    repeat (10) @(negedge clk);
    rst_n = 1'b1;
    repeat (5)  @(negedge clk);

    $display("---- APB0_gpu2d: 槽 2 一整块(0x2000 观测 / 0x2040 显示) ----");

    // ---- 1) 两个子块的 ID ----
    apb_read(12'h000);  check("0x2000 GPU2D_ID", rd, GPU2D_ID);
                        check("0x2000 无错    ", {31'd0, err}, 32'd0);
    apb_read(12'h040);  check("0x2040 LB_ID   ", rd, LINEBUFFER_ID);
                        check("0x2040 无错    ", {31'd0, err}, 32'd0);

    // ---- 2) 子块内的洞 ----
    apb_read(12'h020);  check("0x2020 报错    ", {31'd0, err}, 32'd1);
    apb_read(12'h070);  check("0x2070 报错    ", {31'd0, err}, 32'd1);

    // ---- 3) 高位非零: 不许别名 ----
    apb_read(12'h080);  check("0x2080 报错    ", {31'd0, err}, 32'd1);
    apb_read(12'h240);  check("0x2240 报错    ", {31'd0, err}, 32'd1);
    apb_read(12'h0C0);  check("0x20C0 报错    ", {31'd0, err}, 32'd1);

    // ---- 4) 显示子块 RW ----
    apb_write(12'h044, 32'h1234_5000);  check("0x2044 写无错  ", {31'd0, err}, 32'd0);
    apb_read (12'h044);                 check("0x2044 读回    ", rd, 32'h1234_5000);
    apb_write(12'h044, 32'h2222_2228);  // 低位不对齐 -> 丢掉
    apb_read (12'h044);                 check("0x2044 不对齐丢", rd, 32'h1234_5000);

    apb_write(12'h048, 32'h0A00_0000);
    apb_write(12'h04C, 32'h0B00_0000);
    apb_write(12'h050, 32'h0C00_0000);
    apb_write(12'h054, 32'h0D00_0000);
    apb_write(12'h058, 32'h0E00_0000);
    apb_read (12'h048);                 check("0x2048 读回    ", rd, 32'h0A00_0000);
    check("framea 基址打包 ", framea_base_addr, {32'h1234_5000, 32'h0A00_0000});
    check("frameb 基址打包 ", frameb_base_addr, {32'h0B00_0000, 32'h0C00_0000});
    check("全屏 基址打包   ", frame_full_base_addr, {32'h0D00_0000, 32'h0E00_0000});

    // ---- 5) STATUS: ENABLE / SPLIT ----
    apb_write(12'h05C, 32'h3);
    apb_read (12'h05C);                 check("0x205C 读回    ", rd, 32'h3);
    check("enable 输出    ", {31'd0, enable}, 32'd1);
    check("split_pending  ", {31'd0, split_pending}, 32'd1);

    // ---- 6) 观测子块只读 ----
    busy_val       = 32'h0000_0001;
    cmd_done_val   = 32'h0000_0011;
    frame_done_val = 32'h0000_0022;
    wr_beats_val   = 32'h0000_0033;
    rd_beats_val   = 32'h0000_0044;
    busy_cyc_val   = 32'h0000_0055;
    apb_read(12'h004);                  check("0x2004 BUSY    ", rd, 32'h0000_0001);
    apb_read(12'h008);                  check("0x2008 CMD_DONE", rd, 32'h0000_0011);
    apb_read(12'h00C);                  check("0x200C FRM_DONE", rd, 32'h0000_0022);
    apb_read(12'h010);                  check("0x2010 WR_BEATS", rd, 32'h0000_0033);
    apb_read(12'h014);                  check("0x2014 RD_BEATS", rd, 32'h0000_0044);
    apb_read(12'h018);                  check("0x2018 BUSY_CYC", rd, 32'h0000_0055);
    apb_write(12'h008, 32'hdead_beef);  check("0x2008 写只读  ", {31'd0, err}, 32'd0);
    apb_read (12'h008);                 check("0x2008 写后不变", rd, 32'h0000_0011);

    // ---- 7) 帧率(Handler 送来的值) ----
    fps_acc_val = 32'd60;  fps_cpu_val = 32'd30;
    @(negedge clk);
    apb_read(12'h060);                  check("0x2060 FPS_ACC ", rd, 32'd60);
    apb_read(12'h064);                  check("0x2064 FPS_CPU ", rd, 32'd30);

    // ---- 8) DONE_ACC: 硬件单拍置位 ----
    acc_frame_done = 1'b1;
    @(negedge clk); @(negedge clk);
    acc_frame_done = 1'b0;
    @(negedge clk);
    check("acc_frame_done->done_acc", {31'd0, done_acc}, 32'd1);
    apb_read(12'h06C);                  check("0x206C DONE_ACC", rd, 32'd1);

    // ---- 9) DONE_CPU: 软件 W1S; 写 0 不动 ----
    apb_write(12'h068, 32'd0);          check("0x2068 写0不动 ", {31'd0, done_cpu}, 32'd0);
    apb_write(12'h068, 32'd1);          check("0x2068 写1置位 ", {31'd0, done_cpu}, 32'd1);
    apb_read (12'h068);                 check("0x2068 读回    ", rd, 32'd1);

    // ---- 10) done_clr 清位 ----
    done_clr = 2'b11;
    @(negedge clk); @(negedge clk);
    done_clr = 2'd0;
    @(negedge clk);
    check("done_clr 清 acc", {31'd0, done_acc}, 32'd0);
    check("done_clr 清 cpu", {31'd0, done_cpu}, 32'd0);

    // ---- 11) 切模式: 旧 DONE 丢掉 ----
    acc_frame_done = 1'b1;
    @(negedge clk); @(negedge clk);
    acc_frame_done = 1'b0;
    apb_write(12'h068, 32'd1);
    @(negedge clk);
    check("切模式前 acc=1  ", {31'd0, done_acc}, 32'd1);
    check("切模式前 cpu=1  ", {31'd0, done_cpu}, 32'd1);
    apb_write(12'h05C, 32'h1);          // SPLIT 1->0: 模式变了
    @(negedge clk);
    check("切模式后 acc=0  ", {31'd0, done_acc}, 32'd0);
    check("切模式后 cpu=0  ", {31'd0, done_cpu}, 32'd0);

    // ---- 收尾 ----
    $display("");
    $display("  检查项 %0d 项, 错 %0d 项", n_chk, err_cnt);
    if (err_cnt == 0) $display("SUMMARY: APB0_gpu2d ALL-PASS");
    else              $display("SUMMARY: APB0_gpu2d FAIL (%0d)", err_cnt);
    $finish;
end

endmodule
