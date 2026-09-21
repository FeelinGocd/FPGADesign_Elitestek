////////////////////////////////////////////////////////////////////////////////
// tb_APB0_lineBuffer.v —— 行缓冲器总线解析层(纯寄存器 + 翻页)验证
//
//  验什么
//    1) 读 ID / 六个基址写回读
//    2) 基址 16 字节对齐:写非对齐 -> 丢掉, 读回还是旧值
//    3) STATUS 的 ENABLE/SPLIT 写回读, 并驱动 lineBuffer_enable/mode
//    4) DONE_CPU / DONE_ACC 的 W1S 语义 + FPS 只在 0->1 时 +1
//    5) 加速器 acc_frame_done 硬件置位
//    6) 帧边界翻页:地址寄存器不动, 只有索引翻 -> 显示地址跟着变, DONE 被清
//    7) 两位独立: 只有 DONE 的那一路翻, 另一路不动
//    8) 没有 DONE 时来 frame_start 不该翻
//    9) 非法地址 -> PSLVERROR
//
//  编译运行(ModelSim)
//    vlog APB0_lineBuffer.v ; vlog tb_APB0_lineBuffer.v
//    vsim -c tb_APB0_lineBuffer -do "run -all; quit -f"
////////////////////////////////////////////////////////////////////////////////
`timescale 1ns/100ps

module tb_APB0_lineBuffer;

localparam CLK_HALF   = 5.0;
localparam FPS_WINDOW = 4;                  // 和 DUT 的窗口参数保持一致(4 帧算一秒)

reg         clk, rst_n;
reg  [11:0] PADDR;
reg         PSEL, PENABLE, PWRITE;
reg  [31:0] PWDATA;
wire [31:0] PRDATA;
wire        PREADY, PSLVERROR;

reg         acc_frame_done;
reg         frame_start;

wire        en, mode;
wire [63:0] lb_addr;                    // {段0, 段1}

wire [31:0] fps_acc;

integer     err_cnt;
integer     i;
reg         fps_seen;
reg  [31:0] rd;

initial begin clk = 1'b0; forever #CLK_HALF clk = ~clk; end

//------------------------------------------------------------------------------
// 两级相连:APB(寄存器) -> handler(翻译成有效信号)
//------------------------------------------------------------------------------
wire        apb_enable, apb_split;
wire [31:0] apb_fps_acc_val, apb_fps_cpu_val;
wire [1:0]  done_clr;
wire        apb_done_acc, apb_done_cpu;
wire [63:0] base_a, base_b;                  // {acc 半, cpu 半}
wire [63:0] base_full;                       // {全屏 C, 全屏 D}

APB0_lineBuffer #(.ADDR_WIDTH(32)) u_apb (
    .clk                        (clk),          .rst_n              (rst_n),
    .PADDR                      (PADDR),        .PSEL               (PSEL),
    .PENABLE                    (PENABLE),      .PWRITE             (PWRITE),
    .PWDATA                     (PWDATA),       .PRDATA             (PRDATA),
    .PREADY                     (PREADY),       .PSLVERROR          (PSLVERROR),
    .acc_frame_done             (acc_frame_done),
    .done_clr                   (done_clr),
    .fps_acc_val                (apb_fps_acc_val),
    .fps_cpu_val                (apb_fps_cpu_val),
    .enable                     (apb_enable),
    .split_pending              (apb_split),
    .framea_base_addr           (base_a),
    .frameb_base_addr           (base_b),
    .frame_full_base_addr       (base_full),
    .done_acc                   (apb_done_acc),
    .done_cpu                   (apb_done_cpu),
    .fps_acc                    (fps_acc)
);

bufferAPBHandler #(.ADDR_WIDTH(32), .FPS_WINDOW(FPS_WINDOW)) u_hdl (
    .clk                        (clk),          .rst_n              (rst_n),
    .enable                     (apb_enable),
    .split_pending              (apb_split),
    .done_acc                   (apb_done_acc),
    .done_cpu                   (apb_done_cpu),
    .framea_base_addr           (base_a),
    .frameb_base_addr           (base_b),
    .frame_full_base_addr       (base_full),
    .lineBuffer_frame_start     (frame_start),
    .done_clr                   (done_clr),
    .fps_acc_val                (apb_fps_acc_val),
    .fps_cpu_val                (apb_fps_cpu_val),
    .lineBuffer_enable          (en),
    .lineBuffer_mode            (mode),
    .lineBuffer_addr            (lb_addr)
);


//------------------------------------------------------------------------------
// APB 读写任务
//------------------------------------------------------------------------------
task apb_write(input [11:0] addr, input [31:0] data);
begin
    @(posedge clk); PADDR = addr; PWDATA = data; PWRITE = 1'b1; PSEL = 1'b1; PENABLE = 1'b0;
    @(posedge clk); PENABLE = 1'b1;
    @(posedge clk); PSEL = 1'b0; PENABLE = 1'b0; PWRITE = 1'b0;
end
endtask

task apb_read(input [11:0] addr, output [31:0] data);
begin
    @(posedge clk); PADDR = addr; PWRITE = 1'b0; PSEL = 1'b1; PENABLE = 1'b0;
    @(posedge clk); PENABLE = 1'b1;
    @(posedge clk); data = PRDATA; PSEL = 1'b0; PENABLE = 1'b0;
end
endtask

task check(input [255:0] name, input [63:0] got, input [63:0] exp);
begin
    if (got !== exp) begin
        err_cnt = err_cnt + 1;
        $display("  FAIL %0s: got=%0h exp=%0h @%0t", name, got, exp, $time);
    end
end
endtask

// 帧边界脉冲
task pulse_frame_start;
begin
    @(posedge clk); frame_start = 1'b1;
    @(posedge clk); frame_start = 1'b0;
end
endtask

//------------------------------------------------------------------------------
// 主流程
//------------------------------------------------------------------------------
initial begin
    rst_n = 1'b0; PADDR = 12'd0; PSEL = 1'b0; PENABLE = 1'b0; PWRITE = 1'b0; PWDATA = 32'd0;
    acc_frame_done = 1'b0; frame_start = 1'b0;
    err_cnt = 0;
    repeat (5) @(posedge clk);
    rst_n = 1'b1;
    repeat (2) @(posedge clk);

    // 1) ID
    apb_read(12'h000, rd);  check("ID", rd, 32'h4C423031);

    // 2) 六个基址写回读
    apb_write(12'h004, 32'h0020_0000);   // A_ACC
    apb_write(12'h008, 32'h0020_1000);   // A_CPU
    apb_write(12'h00C, 32'h0020_2000);   // B_ACC
    apb_write(12'h010, 32'h0020_3000);   // B_CPU
    apb_write(12'h014, 32'h00C0_0000);   // C_FULL
    apb_write(12'h018, 32'h00C0_1000);   // D_FULL
    apb_read (12'h004, rd);  check("A_ACC", rd, 32'h0020_0000);
    apb_read (12'h010, rd);  check("B_CPU", rd, 32'h0020_3000);
    apb_read (12'h018, rd);  check("D_FULL", rd, 32'h00C0_1000);

    // 3) 非对齐基址:丢掉, 读回旧值
    apb_write(12'h004, 32'h0020_0002);
    apb_read (12'h004, rd);  check("unaligned dropped", rd, 32'h0020_0000);

    // 4) STATUS.ENABLE / SPLIT
    apb_write(12'h01C, 32'h0000_0003);              // EN=1 SPLIT=1
    pulse_frame_start;                              // SPLIT 到帧边界才生效
    @(negedge clk);
    check("enable (立即生效)", {63'd0, en}, 64'd1);
    check("mode=split", {63'd0, mode}, 64'd1);
    check("half acc = A_ACC", lb_addr[63:32], 64'h0020_0000);
    check("half cpu = A_CPU", lb_addr[31:0],  64'h0020_1000);
    apb_read (12'h01C, rd);  check("STATUS back", rd, 32'h0000_0003);   // idx 都是 0

    // 5) DONE_CPU 的 W1S
    apb_write(12'h028, 32'h0000_0001);
    apb_read (12'h028, rd);  check("DONE_CPU set", rd[0], 1'b1);
    apb_write(12'h028, 32'h0000_0000);              // 写 0 必须不动
    apb_read (12'h028, rd);  check("DONE_CPU stays", rd[0], 1'b1);

    // 6) 加速器硬件交帧
    @(posedge clk); acc_frame_done = 1'b1;
    @(posedge clk); acc_frame_done = 1'b0;
    apb_read (12'h02C, rd);  check("DONE_ACC set", rd[0], 1'b1);

    // 7) 帧边界翻页(分屏 acc):地址寄存器不动, 显示地址切到 B, DONE 清掉
    pulse_frame_start;
    apb_read (12'h004, rd);  check("A_ACC keeps", rd, 32'h0020_0000);
    apb_read (12'h00C, rd);  check("B_ACC keeps", rd, 32'h0020_2000);
    apb_read (12'h02C, rd);  check("DONE_ACC cleared", rd[0], 1'b0);
    @(negedge clk);
    check("display acc -> B", lb_addr[63:32], 64'h0020_2000);

    // 8) 翻页(分屏 cpu):cpu 那一路独立翻
    pulse_frame_start;
    apb_read (12'h028, rd);  check("DONE_CPU cleared", rd[0], 1'b0);
    @(negedge clk);
    check("display cpu -> B", lb_addr[31:0], 64'h0020_3000);
    check("display acc stays B", lb_addr[63:32], 64'h0020_2000);

    // 9) 全屏模式:用 lb_addr[63:32], C <-> D
    apb_write(12'h01C, 32'h0000_0001);              // EN=1 SPLIT=0
    pulse_frame_start;                              // SPLIT 到帧边界才生效
    @(negedge clk);
    check("mode=full", {63'd0, mode}, 64'd0);
    check("full = D (索引沿用, 不因切模式复位)", lb_addr[63:32], 64'h00C0_1000);
    check("full: 段1 = 0", lb_addr[31:0], 64'd0);
    @(posedge clk); acc_frame_done = 1'b1;
    @(posedge clk); acc_frame_done = 1'b0;
    pulse_frame_start;
    @(negedge clk);
    check("full -> C", lb_addr[63:32], 64'h00C0_0000);
    apb_read (12'h014, rd);  check("C_FULL keeps", rd, 32'h00C0_0000);
    apb_read (12'h018, rd);  check("D_FULL keeps", rd, 32'h00C0_1000);

    // 10) 没有 DONE 时来 frame_start:不该翻
    pulse_frame_start;
    @(negedge clk);          check("no-DONE: stays C", lb_addr[63:32], 64'h00C0_0000);

    // 10b) 帧中间写 SPLIT:mode 要等帧边界才变(此时是全屏)
    apb_write(12'h01C, 32'h0000_0003);              // 想切成分屏
    @(negedge clk);          check("mode 不立即变", {63'd0, mode}, 64'd0);
    pulse_frame_start;
    @(negedge clk);          check("帧边界后 mode=1", {63'd0, mode}, 64'd1);
    apb_read (12'h01C, rd);  check("回读写进去的 mode", rd[1], 1'b1);

    // 10c) 带 DONE 切模式:DONE 被清掉, 不会拿旧 DONE 去翻新模式的索引
    @(posedge clk); acc_frame_done = 1'b1;          // 加速器(分屏)交一帧
    @(posedge clk); acc_frame_done = 1'b0;
    apb_read (12'h02C, rd);  check("DONE_ACC set", rd[0], 1'b1);
    apb_write(12'h01C, 32'h0000_0001);              // 切回全屏
    apb_read (12'h02C, rd);  check("切模式清 DONE_ACC", rd[0], 1'b0);
    apb_write(12'h028, 32'h0000_0001);              // CPU 也交一帧
    apb_read (12'h028, rd);  check("DONE_CPU set", rd[0], 1'b1);
    apb_write(12'h01C, 32'h0000_0003);              // 再切(又改 SPLIT)
    apb_read (12'h028, rd);  check("切模式清 DONE_CPU", rd[0], 1'b0);

    // 11) 帧率:窗口 = FPS_WINDOW 个 frame_start, 数的是"交帧事件"
    @(posedge clk); acc_frame_done = 1'b1;
    @(posedge clk); acc_frame_done = 1'b0;              // 加速器交 1 帧
    apb_write(12'h028, 32'h0000_0001);                  // CPU 交 1 帧
    fps_seen = 1'b0;
    for (i = 0; i < 2*FPS_WINDOW; i = i + 1) begin       // 逐帧推进, 总有一个窗口锁存到 1
        pulse_frame_start;
        apb_read (12'h020, rd);
        if (rd == 32'd1) fps_seen = 1'b1;
    end
    check("FPS_ACC=1", {63'd0, fps_seen}, 64'd1);
    for (i = 0; i < 2*FPS_WINDOW; i = i + 1) begin
        pulse_frame_start;
        apb_read (12'h024, rd);
        if (rd == 32'd1) fps_seen = 1'b1;
    end
    check("FPS_CPU=1", {63'd0, fps_seen}, 64'd1);

    // 13) ★ DONE_ACC 的【软件 W1S】 —— 上板踩过的漏: 写 0x202C 毫无作用,
    //     导致交帧事件不产生、帧率恒 0、翻页不发生(顶层加速器没接, 硬件那路是 0)
    apb_write(12'h02C, 32'h0000_0001);
    apb_read (12'h02C, rd);  check("DONE_ACC sw set", rd[0], 1'b1);
    apb_write(12'h02C, 32'h0000_0000);              // 写 0 必须不动
    apb_read (12'h02C, rd);  check("DONE_ACC sw stays", rd[0], 1'b1);
    // 软件交帧也要产生事件(帧率会数到); 帧边界消费后清掉
    pulse_frame_start;
    apb_read (12'h02C, rd);  check("DONE_ACC sw consumed", rd[0], 1'b0);

    // 14) 非法地址
    @(posedge clk); PADDR = 12'hF00; PWRITE = 1'b0; PSEL = 1'b1; PENABLE = 1'b1;
    @(posedge clk); check("PSLVERROR", {63'd0, PSLVERROR}, 64'd1);
    PSEL = 1'b0; PENABLE = 1'b0;
    @(posedge clk);

    if (err_cnt == 0) $display("---- ALL CHECKS PASSED ----");
    else              $display("---- FAIL: %0d errors ----", err_cnt);
    $finish;
end

initial begin
    #200000;
    $display("TIMEOUT");
    $display("---- FAIL ----");
    $finish;
end

endmodule
