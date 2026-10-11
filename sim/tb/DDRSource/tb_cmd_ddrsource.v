//==============================================================================
//  tb_cmd_ddrsource.v —— CmdExec + DDRSource 联调：看时序
//------------------------------------------------------------------------------
//  接法（模仿真实顶层）:
//      CmdExec.line_buffer_free  <--  DDRSource.line_src0_free   （只有一路源，直接接）
//      DDRSource 的像素出口由本 TB 一直取走（假扮 Compositor + 写侧）
//      每行取够"该有的像素字数"后，本 TB 拉一次 line_done（假扮写侧落盘）
//  看什么:
//      每行: 行任务地址 -> 读突发(几笔/每笔几拍) -> 像素吐了几拍 -> line_done
//      line_src0_free 什么时候起来、下一行什么时候发出去
//==============================================================================
`timescale 1ns/100ps
module tb_cmd_ddrsource;

localparam CLK_HALF = 5;

reg                 clk, rst_n;

// ---- CmdExec 命令口 ----
reg                 cmd_valid;
wire                cmd_ready;
reg  [2:0]          cmd_type;
reg  [31:0]         cmd_src, cmd_dst;
reg  [15:0]         cmd_w, cmd_h;
reg  [6:0]          cmd_flags;
reg  [23:0]         cmd_args;

//   ★ N 张图：源地址 / 模式 / 参数 这三根线一张图一格；TB 只驱动第 1 张图（原图）那格，
//     宽高是几张图共用的，直接接。上面的量和老 TB 一模一样，一个字都不用改
localparam IMAGE_NUM = 2;
wire [32*IMAGE_NUM-1:0] cmd_src_bus   = {{(32*(IMAGE_NUM-1)){1'b0}}, cmd_src};
wire [7*IMAGE_NUM-1:0]  cmd_flags_bus = {{(7*(IMAGE_NUM-1)){1'b0}},  cmd_flags};
wire [32*IMAGE_NUM-1:0] cmd_args_bus  = {{(32*(IMAGE_NUM-1)){1'b0}}, cmd_args};

// ---- 行任务 ----
wire                line_start;
wire [31:0]         line_dst_addr, line_src_addr;
wire [15:0]         line_w;
wire [6:0]          line_flags;
wire [23:0]         line_param;
wire                line_done;
wire                line_buffer_free;

// ---- AXI 读 ----
wire                rd_cmd_valid;
wire                rd_cmd_ready;
wire [31:0]         rd_cmd_addr;
wire [7:0]          rd_cmd_len;
wire                rd_cmd_done;
reg  [127:0]        rd_data;
reg                 rd_valid;
wire                rd_ready;

// ---- 像素流 ----
reg                 pixel_rd_en;
wire [95:0]         pixel_rd_data;
wire                pixel_rd_valid;
wire                line_src0_free;

localparam          LINE_STRIDE = 5760;       // 全屏一行 1920*3 字节（源、目的共用）

integer             err_cnt;
integer             line_no, pixel_total;
integer             burst_cnt;
reg                 line_done_r;                    // ★ 单拍脉冲（不能用电平，CmdExec 按拍计数）

// ---- ★ 第二条命令（alpha）：源① 必须读【目的自己】----
localparam [2:0] MD_ALPHA = 3'd2;
reg         alpha_phase;                    // 正在跑 alpha 那条命令
reg         exp_bg_rd;                      // 这条命令该不该是 alpha（flags[2:0]==2）
reg  [31:0] bg_row_addr;                    // 源① 这一行该读的行地址（alpha 时 = 目的行地址）
integer     alpha_ar;                       // alpha 阶段收到的 AR 笔数
integer     alpha_line_no;                  // alpha 阶段完成的行数
integer     chk1_state, chk1_lines, chk1_ar;    // 第一条命令的结果（后面要重置计数器）

//==============================================================================
// DUT
//==============================================================================
CmdExec #(.LINE_BYTES(5760)) u_exec (
    .clk_acc            (clk),
    .rst_acc_n          (rst_n),
    .cmd_valid          (cmd_valid),
    .cmd_ready          (cmd_ready),
    .cmd_type           (cmd_type),
    .cmd_dst            (cmd_dst),
    .cmd_w              (cmd_w),
    .cmd_h              (cmd_h),
    .cmd_src            (cmd_src_bus),
    .cmd_flags          (cmd_flags_bus),
    .cmd_args           (cmd_args_bus),
    .lineBuffer_mode    (1'b0),                 // 全屏 5760
    .line_start         (line_start),
    .line_dst_addr      (line_dst_addr),
    .line_src_addr      (line_src_addr),
    .line_w             (line_w),
    .line_flags         (line_flags),
    .line_param         (line_param),
    .line_done          (line_done),
    .line_buffer_free   (line_buffer_free)
);

DDRSource #(
    .ADDR_WIDTH (32),
    .AXI_DATA_WIDTH (128),
    .PIXEL_BYTES  (3),
    .PIXELS_PER_WORD  (4),
    .BANK_WIDTH     (96),
    .BANK_DEPTH (512),
    .LEN_WIDTH  (8)
) u_src (
    .clk                (clk),
    .rst_n              (rst_n),
    .line_start         (line_start),
    .line_src_addr      (line_src_addr),
    .line_dst_addr      (line_dst_addr),
    .line_flags         (line_flags),           // ★ 源自己看模式：要不要动、读源行还是读目的
    .line_w             (line_w),
    .pixel_rd_en          (pixel_rd_en),
    .pixel_rd_data        (pixel_rd_data),
    .pixel_rd_valid       (pixel_rd_valid),
    .line_src0_free           (line_src0_free),
    .rd_cmd_valid       (rd_cmd_valid),
    .rd_cmd_ready       (rd_cmd_ready),
    .rd_cmd_addr        (rd_cmd_addr),
    .rd_cmd_len         (rd_cmd_len),
    .rd_cmd_done        (rd_cmd_done),
    .rd_data            (rd_data),
    .rd_valid           (rd_valid),
    .rd_ready           (rd_ready)
);

assign line_buffer_free = line_src0_free;             // 只有一路源，直接接

initial begin clk = 1'b0; forever #(CLK_HALF) clk = ~clk; end

//==============================================================================
// AXI 读从机：收一笔 AR 就按 len 连吐 len+1 拍，最后一拍后拉 cmd_done
//==============================================================================
reg  [8:0]  slv_left;
reg         slv_run;
reg  [127:0] slv_data;

assign rd_cmd_ready = ~slv_run;
assign rd_cmd_done  = (slv_run & rd_valid & rd_ready & (slv_left == 9'd1)) ? 1'b1 : 1'b0;

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        slv_run  <= 1'b0; slv_left <= 9'd0; rd_valid <= 1'b0; slv_data <= 128'd0;
    end
    else begin
        if (rd_cmd_valid & rd_cmd_ready & ~slv_run) begin
            //   ★ alpha：这笔 AR 必须冲着【目的行】去（背景 = 读回目的自己），
            //     而不是命令给的源地址 —— 这是"背景从哪来"唯一能被直接看见的地方。
            if (alpha_phase) begin
                alpha_ar = alpha_ar + 1;
                if (rd_cmd_addr[31:4] !== bg_row_addr[31:4]) begin
                    err_cnt = err_cnt + 1;
                    $display("  !! [T=%0t] alpha 的 AR 地址 %08h 不对，应该是【目的行】%08h",
                             $time, rd_cmd_addr, bg_row_addr);
                end
                else
                    $display("  [OK] alpha: AR 地址 %08h = 目的行地址（读回目的自己）", rd_cmd_addr);
            end
            slv_run  <= 1'b1;
            slv_left <= {1'b0, rd_cmd_len} + 9'd1;
            slv_data <= {112'd0, rd_cmd_addr[15:0]};        // 图案里带上地址，便于核对
            rd_valid <= 1'b1;
            burst_cnt = burst_cnt + 1;
            $display("    [T=%0t] AR addr=%08h len=%0d (%0d 拍)", $time, rd_cmd_addr,
                     rd_cmd_len, {1'b0, rd_cmd_len} + 1);
        end
        else if (rd_valid & rd_ready) begin
            if (slv_left == 9'd1) begin
                rd_valid <= 1'b0; slv_run <= 1'b0; slv_left <= 9'd0;
            end
            else begin
                slv_left <= slv_left - 9'd1;
                slv_data <= slv_data + 128'h1;
            end
        end
    end
end

integer dbg_n;
initial dbg_n = 0;
//==============================================================================
// 假扮 Compositor + 写侧：一直接像素；累计凑满"一行该吐的拍数"就拉一次 line_done
//------------------------------------------------------------------------------
//   ★ 不能按"line_start 清零、本行数够"来判 —— 那是把源当成【跟着合成器走】。
//     实际上源比合成器快：行任务来的时候，上一行的块还排在 ping-pong 里没读完
//     （实测：第 2 行任务到达时，第 1 行的 52 拍只被取走 5 拍）。
//     按"本行"数就会把上一行的尾巴算到下一行头上，永远差一行 —— 3 行只能数出 2 行。
//   ⇒ 像素流是【一路连续的块流】：每行一块、每块拍数固定。所以按【累计拍数】划行。
//
//   每行吐几拍：读回 {max(src_off,dst_off) + 3w} 字节按拍对齐 → 收下的字节减去 shift
//   再按 12 字节一拍向上取整（最后不足一拍由 unpacker 的 flush 补零吐出）
//==============================================================================
integer shift_off;                          // src_off - dst_off（送给 unpacker 的那个）
integer read_bytes;                         // 本行读回几字节（按 16 字节拍对齐）
integer line_words;                         // 本行会吐几拍像素（一拍 = 4 像素 = 12 字节）
integer line_issued;                        // 已经发出去几行
integer exp_words;

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        pixel_rd_en <= 1'b0; pixel_total = 0; line_words = 0;
        shift_off = 0; read_bytes = 0; line_issued = 0; exp_words = 0;
    end
    else begin
        pixel_rd_en <= 1'b1;                                  // 一直要
        if (line_start) begin
            shift_off  = line_src_addr[3:0] - line_dst_addr[3:0];
            read_bytes = (((((line_src_addr[3:0] > line_dst_addr[3:0])
                            ? line_src_addr[3:0] : line_dst_addr[3:0]) + line_w*3) + 15) / 16) * 16;
            exp_words  = (read_bytes - shift_off + 11) / 12;

            //   ★ 3 行几何相同 ⇒ 每行该吐的拍数必须一致；行地址必须按 stride 逐行递推
            if (line_words == 0) line_words = exp_words;
            else if (exp_words != line_words) begin
                err_cnt = err_cnt + 1;
                $display("  !! [T=%0t] 第 %0d 行该吐 %0d 拍，和第 1 行的 %0d 拍不一致",
                         $time, line_issued, exp_words, line_words);
            end
            if (line_src_addr !== (cmd_src + line_issued*LINE_STRIDE)) begin
                err_cnt = err_cnt + 1;
                $display("  !! [T=%0t] 第 %0d 行源地址 %08h，应为 %08h",
                         $time, line_issued, line_src_addr, cmd_src + line_issued*LINE_STRIDE);
            end
            if (line_dst_addr !== (cmd_dst + line_issued*LINE_STRIDE)) begin
                err_cnt = err_cnt + 1;
                $display("  !! [T=%0t] 第 %0d 行目的地址 %08h，应为 %08h",
                         $time, line_issued, line_dst_addr, cmd_dst + line_issued*LINE_STRIDE);
            end
            //   ★ 这条命令是不是 alpha —— 看广播出去的 flags[2:0]（DDRSource 也是这样自己判的）。
            //     "背景 = 目的自己"的真正判据是下面那笔 AR 的地址：它必须冲着目的行去。
            if ((line_flags[2:0] == 3'd2) !== exp_bg_rd) begin
                err_cnt = err_cnt + 1;
                $display("  !! [T=%0t] flags[2:0]=%0d，alpha 期望 %b", $time, line_flags[2:0], exp_bg_rd);
            end
            bg_row_addr = (line_flags[2:0] == 3'd2) ? line_dst_addr : line_src_addr;
            $display("  [T=%0t] 行任务 #%0d: src=%08h dst=%08h w=%0d  shift=%0d, 读回 %0d 字节 → 每行 %0d 拍像素",
                     $time, line_issued, line_src_addr, line_dst_addr, line_w, shift_off, read_bytes, exp_words);
            line_issued = line_issued + 1;
        end
        //   ★ line_done 必须是【单拍脉冲】：CmdExec 用 line_done_cnt_r 按拍计数，
        //     给电平它会一拍加一次。而块和块之间一定有断档（ping-pong 换块那几拍），
        //     "累计拍数停在整数倍上"的状态会挂住好几拍 → 命令提前收工（实测只发了 2 行）。
        line_done_r <= 1'b0;
        if (pixel_rd_valid & pixel_rd_en) begin
            pixel_total = pixel_total + 1;
            if ((line_words != 0) && ((pixel_total % line_words) == 0)) begin
                line_no      = line_no + 1;
                line_done_r <= 1'b1;                        // 单拍脉冲
                $display("  [T=%0t] 行 #%0d 数据吐完（累计 %0d 拍）  line_src0_free=%b",
                         $time, line_no-1, pixel_total, line_src0_free);
            end
        end
    end
end

assign line_done = line_done_r;

//==============================================================================
// 主流程：发一条 BLIT(REPLACE)，3 行；源和目的故意错开低 4 位
//==============================================================================
initial begin
    err_cnt = 0; line_no = 0; burst_cnt = 0; pixel_total = 0;
    line_words = 0; shift_off = 0; read_bytes = 0; line_issued = 0; exp_words = 0;
    alpha_phase = 1'b0; exp_bg_rd = 1'b0; alpha_ar = 0; alpha_line_no = 0;
    chk1_state = 0; chk1_lines = 0; chk1_ar = 0; bg_row_addr = 32'd0;
    rst_n = 1'b0; cmd_valid = 1'b0;
    cmd_type = 3'd2; cmd_flags = 7'd1;             // BLIT + REPLACE
    cmd_src  = 32'h0900_000C;                      // 低 4 位 = 0xC（既非 0 也非对齐）
    cmd_dst  = 32'h0880_0004;                      // 低 4 位 = 4
    cmd_w    = 16'd200;                            // 200 像素 = 600 字节
    cmd_h    = 16'd3;                              // 3 行
    cmd_args= 24'd0;

    repeat (6) @(posedge clk);
    rst_n = 1'b1;
    repeat (4) @(posedge clk);

    @(posedge clk); #1;
    cmd_valid = 1'b1;
    @(posedge clk); #1;
    cmd_valid = 1'b0;

    repeat (20000) @(posedge clk);

    // ---- 第一条命令收工：结果先记住（下面要清计数器跑第二条）----
    chk1_state = u_exec.state;
    chk1_lines = line_no;
    chk1_ar    = burst_cnt;
    $display("==== unpacker: S_ALIGN %0d 拍 / S_RUN %0d 拍, out_valid %0d 拍, 收进 %0d 拍, cnt高水位 %0d ====",
             up_st0, up_st1, up_ov, up_ini, up_max);
    $display("==== 第一条(BLIT/REPLACE): 完成 %0d 行, AR 共 %0d 笔, state=%0d, line_done %0d 次 ====",
             chk1_lines, chk1_ar, chk1_state, u_exec.line_done_cnt_r);
    if ((chk1_state == 2'd0) && (chk1_lines == 3) && (line_issued == 3))
        $display("==== PASS: 3 行全部完成、命令正常收工 ====");
    else begin
        err_cnt = err_cnt + 1;
        $display("==== FAIL: 行数/命令状态不对 ====");
    end

    //------------------------------------------------------------------
    //   ★ 第二条：MODE_ALPHA —— 源① 必须读【目的自己】
    //     判据：① flags[2:0] = 2（这是 alpha 命令）
    //           ② DDR 读命令(AR) 的地址 = 【目的行地址】，不是命令给的源地址
    //     这条是"背景从哪来"唯一能被直接看见的地方（合成器的混合另由
    //     tb_alphaCompositor 管）。
    //------------------------------------------------------------------
    $display("---- 第二条: MODE_ALPHA（背景 = 读回目的）----");
    alpha_phase = 1'b1;
    exp_bg_rd   = 1'b1;
    line_no = 0; pixel_total = 0; burst_cnt = 0; line_words = 0;
    shift_off = 0; read_bytes = 0; line_issued = 0; exp_words = 0;
    cmd_type  = 3'd2;                       // BLIT
    cmd_flags = {4'd1, MD_ALPHA};           // flags[3]=1（PRAM 精灵源）+ MODE=alpha
    cmd_src   = 32'h0A00_0000;              // 精灵在 PRAM 里的基址（字）—— 【不该被读】
    cmd_dst   = 32'h0880_0100;              // 低 4 位对齐，免得再拆笔
    cmd_w     = 16'd16;
    cmd_h     = 16'd1;
    cmd_args = 24'd0;                      // 0 ⇒ 源行跨距退回目的跨距

    @(posedge clk); #1;
    cmd_valid = 1'b1;
    @(posedge clk); #1;
    cmd_valid = 1'b0;

    repeat (3000) @(posedge clk);
    alpha_line_no = line_no;
    $display("==== 第二条(ALPHA): 完成 %0d 行, AR 共 %0d 笔, state=%0d ====",
             alpha_line_no, alpha_ar, u_exec.state);
    if ((alpha_ar >= 1) && (alpha_line_no >= 1) && (u_exec.state == 2'd0))
        $display("==== PASS(alpha): 背景读的就是目的行 ====");
    else begin
        err_cnt = err_cnt + 1;
        $display("==== FAIL(alpha): 没读目的行 / 命令没收工 ====");
    end

    $display("SUMMARY lines=%0d ar=%0d err=%0d unpack_srun=%0d unpack_ov=%0d unpack_in=%0d maxcnt=%0d alpha_ar=%0d",
             chk1_lines, chk1_ar, err_cnt, up_st1, up_ov, up_ini, up_max, alpha_ar);
    if (err_cnt == 0) $display("RESULT: ALL-PASS");
    else              $display("RESULT: FAIL");
    $finish;
end

//==============================================================================
// 探针：看 DDRSource 的状态和交块那几个信号什么时候动
//==============================================================================
reg [1:0] pr_st; reg pr_hand, pr_low, pr_ready, pr_flush;
initial begin pr_st=2'd0; pr_hand=0; pr_low=0; pr_ready=0; pr_flush=0; end
always @(posedge clk) begin
    if (u_src.state          !== pr_st)    begin pr_st    = u_src.state;          $display("    [T=%0t] state        -> %0d", $time, u_src.state); end
    if (u_src.bank_hand_r    !== pr_hand)  begin pr_hand  = u_src.bank_hand_r;    $display("    [T=%0t] bank_hand_r  -> %b",  $time, u_src.bank_hand_r); end
    if (u_src.bank_low_r     !== pr_low)   begin pr_low   = u_src.bank_low_r;     $display("    [T=%0t] bank_low_r   -> %b",  $time, u_src.bank_low_r); end
    if (u_src.bank_wr_ready  !== pr_ready) begin pr_ready = u_src.bank_wr_ready;  $display("    [T=%0t] bank_wr_ready-> %b",  $time, u_src.bank_wr_ready); end
    if (u_src.unpack_flush_r !== pr_flush) begin pr_flush = u_src.unpack_flush_r; $display("    [T=%0t] unpack_flush -> %b",  $time, u_src.unpack_flush_r); end
end

//   统计：S_RUN 里待了多少拍、其中 rd_ready 高了多少拍、收了几拍数据
integer srun_cnt, rdy_cnt, take_cnt;
reg [1:0] st_d;
initial begin srun_cnt=0; rdy_cnt=0; take_cnt=0; st_d=2'd0; end
always @(posedge clk) begin
    st_d <= u_src.state;
    if (u_src.state == 2'd2) srun_cnt = srun_cnt + 1;
    if (u_src.state == 2'd2 && rd_ready) rdy_cnt = rdy_cnt + 1;
    if (u_src.state == 2'd2 && rd_valid && rd_ready) take_cnt = take_cnt + 1;
    if (st_d == 2'd2 && u_src.state != 2'd2) begin
        $display("    >>> 离开 S_RUN: 本行 %0d 拍, rd_ready 高 %0d 拍, 收下 %0d 拍, rd_cnt_r=%0d",
                 srun_cnt, rdy_cnt, take_cnt, u_src.rd_cnt_r);
        srun_cnt = 0; rdy_cnt = 0; take_cnt = 0;      // 每行清零，看单行
    end
end

//   unpacker 内部统计：卡在哪个状态、兜里水位、吐了多少拍
integer up_st0, up_st1, up_ov, up_ini, up_max;
initial begin up_st0=0; up_st1=0; up_ov=0; up_ini=0; up_max=0; end
always @(posedge clk) begin
    if (u_src.u_DDRUnpacker.state == 1'b0) up_st0 = up_st0 + 1;
    else                                   up_st1 = up_st1 + 1;
    if (u_src.u_DDRUnpacker.out_valid) up_ov = up_ov + 1;
    if (u_src.u_DDRUnpacker.in_valid && u_src.u_DDRUnpacker.in_ready) up_ini = up_ini + 1;
    if (u_src.u_DDRUnpacker.cnt_byte > up_max) up_max = u_src.u_DDRUnpacker.cnt_byte;
end
initial begin #2_000_000; $display("FAIL 全局超时"); $finish; end
endmodule
