////////////////////////////////////////////////////////////////////////////////
// tb_WriteController.v —— 一行 → 若干笔 AXI 写
//
//  测的是什么
//      WriteController 的输出挂到真的 AXIMastersHandler（内含 AXI4_Master_Write），
//      再挂一个极简的 AXI 写从设备模型把 AW/W/B 全收下来。
//      → 测的是真链路：WriteController → AXIMastersHandler → AXI
//
//  改名说明
//      待测模块原来叫 writeControler，现在叫 WriteController，TB 跟着改名。
//
//  时序约定
//      所有驱动与采样都在 posedge 之后 #1 进行
//
//  ★ 接口对齐（RTL 现状；旧 TB 按老接口写的地方）
//    1) 没有 line_op / line_color 了 —— WriteController 现在是【纯写入器】：
//       不再自己生成常数色和相位（那件事搬到了 Compositor + DDRPacker），
//       数据由上游给：src_data[127:0] + src_valid。
//       所以 TB 必须【假扮上游】供数（见下面的"上游供数模型"）——
//       给完 line_start 之后把 src_valid 拉起来，一拍一拍喂 128 位。
//    2) 新增 wr_free —— "本行连 B 响应都回来了，能接下一行任务"（上游拿它做发送节奏）。
//    3) 行任务【先锁存再开工】：line_start 那一拍只锁行上下文并置 line_pend_r，
//       等 src_valid 才进 S_CMD 发 AW。数据没起来就永远不发 AW（用例10 专门钉这条路径）。
//    4) line_done 是单拍脉冲，在"最后一笔的 cmd_wr_done"那一拍；
//       空行(w*3 == 0，拍数 0)在 line_start 同拍就回 done（组合出来的）。
//
//  验什么
//    1) 对齐 + 整数拍        → 拍数、地址、WSTRB 全开
//    2) 地址不对齐           → 首拍 WSTRB 遮头、末拍遮尾
//    3) 末拍不满             → 末拍 WSTRB 遮尾
//    4) 跨 4KB 边界          → 拆成两笔，第二笔地址正确
//    5) 单笔超 256 拍        → 拆成两笔，第一笔正好 256 拍
//    6) 逐拍数据与拍序一致   → 第 k 拍必须是上游按拍号给的那一拍
//                              （旧用例是"常数色相位"，相位生成已经搬走了，改成这条）
//    7) 空行(w=0, 0 拍)      → 一笔都不发，不挂死
//    8) 收尾                 → 前面跳过没留后遗症
//    9) 行首不对齐 + 页尾不满一拍 → 旧实现的死锁点（cmd_len 下溢成 255）
//   10) 数据晚到             → 不许提前发 AW/收数；数据到了照常发 AW、正常 line_done
//
//  编译运行(ModelSim)
//    vlog -quiet -work work ../../../project/verified/06_SoC_2DGPU/RTL/Peripherial/AXI4/Master/AXI4_Master_Write.v
//    vlog -quiet -work work ../../../project/verified/06_SoC_2DGPU/RTL/Peripherial/AXI4/Master/AXI4_Master_Read.v
//    vlog -quiet -work work ../../../project/verified/06_SoC_2DGPU/RTL/Peripherial/GPU_2D/interface/AXIMastersHandler.v
//    vlog -quiet -work work ../../../project/verified/06_SoC_2DGPU/RTL/Peripherial/GPU_2D/factory/WriteController.v
//    vlog -quiet -work work tb_WriteController.v
//    vsim -c -quiet -do "run -all; quit -f" work.tb_WriteController
////////////////////////////////////////////////////////////////////////////////
`timescale 1ns/100ps

module tb_WriteController;

localparam CLK_HALF = 5.0;

reg          clk, rst_n;

// ---- 行任务口 ----
//   ★ 没有 line_op / line_color 了；新增 wr_free（能不能接下一行任务）
reg          line_start;
reg  [31:0]  line_dst_addr;
reg  [15:0]  line_w;
wire         line_done;
wire         wr_free;

// ---- 上游供数口（TB 假扮 Compositor + DDRPacker）----
reg  [127:0] src_data;
reg          src_valid;

// ---- 地址任务 / 数据口（WriteController ↔ AXIMastersHandler）----
wire         cmd_wr_valid, cmd_wr_ready;
wire  [31:0] cmd_wr_addr;
wire  [7:0]  cmd_wr_len;
wire         cmd_wr_done, cmd_wr_err;

wire  [127:0] wr_data;
wire  [15:0]  wr_strb;
wire          wr_valid, wr_ready;

// ---- AXI4 主口 ----
wire  [7:0]   m_AXI_AWID;
wire  [31:0]  m_AXI_AWADDR;
wire  [7:0]   m_AXI_AWLEN;
wire  [2:0]   m_AXI_AWSIZE;
wire  [1:0]   m_AXI_AWBURST;
wire          m_AXI_AWLOCK;
wire  [3:0]   m_AXI_AWCACHE;
wire  [2:0]   m_AXI_AWPROT;
wire  [3:0]   m_AXI_AWQOS;
wire  [3:0]   m_AXI_AWREGION;
wire          m_AXI_AWVALID;
reg           m_AXI_AWREADY;

wire  [127:0] m_AXI_WDATA;
wire  [15:0]  m_AXI_WSTRB;
wire          m_AXI_WLAST;
wire          m_AXI_WVALID;
reg           m_AXI_WREADY;

reg   [7:0]   m_AXI_BID;
reg   [1:0]   m_AXI_BRESP;
reg           m_AXI_BVALID;
wire          m_AXI_BREADY;

// ---- 读侧（本用例只用写侧，显式接空）----
//   AXIMastersHandler 里读主 AXI4_Master_Read 是并排的另一半，本用例用不到；
//   不接的话 vopt 会刷 28 条 TFMPC "missing connection" 警告，把真问题淹掉
wire          rd_cmd_ready, rd_cmd_done, rd_cmd_err;
wire  [127:0] rd_data;
wire          rd_valid, rd_last;
wire  [7:0]   m_AXI_ARID;
wire  [31:0]  m_AXI_ARADDR;
wire  [7:0]   m_AXI_ARLEN;
wire  [2:0]   m_AXI_ARSIZE;
wire  [1:0]   m_AXI_ARBURST;
wire          m_AXI_ARLOCK;
wire  [3:0]   m_AXI_ARCACHE;
wire  [2:0]   m_AXI_ARPROT;
wire  [3:0]   m_AXI_ARQOS;
wire  [3:0]   m_AXI_ARREGION;
wire          m_AXI_ARVALID;
wire          m_AXI_RREADY;

integer err_cnt, k;
integer done_evt_cnt;                   // line_done 累计个数（空行的 done 是组合脉冲，靠计数才不漏）
integer done_base;                      // 本行的基线
integer wf_low_cnt;                     // wr_free = 0 的累计拍数
integer wf_before;
integer up_idx;                         // 上游已经喂到第几拍（0 起）
integer up_wait;                        // 数据还有几拍才备好
reg     up_run;
integer data_delay;                     // line_start 之后几拍数据才备好（用例10 拉到 30）
reg     early_aw, early_beat;           // 用例10：等待期里有没有"提前开工"

//==============================================================================
// 数据图案：第 k 拍 = 16 个字节全是 8'(k*7)
//   为什么用它：拍号直接印在数据里 —— 多一拍、少一拍、慢一拍、相位错一格，
//   都能从数据本身查出来，不用去数波形
//==============================================================================
function [127:0] pat_of;
    input [31:0] k;
    reg   [7:0]  b;
    begin
        b = (k * 7);                    // 只取低 8 位：它就是拍号的指纹
        pat_of = {b,b,b,b,b,b,b,b, b,b,b,b,b,b,b,b};
    end
endfunction

WriteController u_dut (
    .clk_acc        (clk),
    .rst_acc_n      (rst_n),
    .line_start     (line_start),
    .line_dst_addr  (line_dst_addr),
    .line_w         (line_w),
    .line_done      (line_done),
    .wr_free        (wr_free),
    .cmd_wr_valid   (cmd_wr_valid),
    .cmd_wr_ready   (cmd_wr_ready),
    .cmd_wr_addr    (cmd_wr_addr),
    .cmd_wr_len     (cmd_wr_len),
    .cmd_wr_done    (cmd_wr_done),
    .cmd_wr_err     (cmd_wr_err),
    .src_data       (src_data),
    .src_valid      (src_valid),
    .wr_data        (wr_data),
    .wr_strb        (wr_strb),
    .wr_valid       (wr_valid),
    .wr_ready       (wr_ready)
);

AXIMastersHandler u_axi (
    .clk            (clk),
    .rst_n          (rst_n),
    .cmd_wr_valid   (cmd_wr_valid),
    .cmd_wr_ready   (cmd_wr_ready),
    .cmd_wr_addr    (cmd_wr_addr),
    .cmd_wr_len     (cmd_wr_len),
    .cmd_wr_done    (cmd_wr_done),
    .cmd_wr_err     (cmd_wr_err),
    .wr_data        (wr_data),
    .wr_strb        (wr_strb),
    .wr_valid       (wr_valid),
    .wr_ready       (wr_ready),
    .m_AXI_AWID     (m_AXI_AWID),
    .m_AXI_AWADDR   (m_AXI_AWADDR),
    .m_AXI_AWLEN    (m_AXI_AWLEN),
    .m_AXI_AWSIZE   (m_AXI_AWSIZE),
    .m_AXI_AWBURST  (m_AXI_AWBURST),
    .m_AXI_AWLOCK   (m_AXI_AWLOCK),
    .m_AXI_AWCACHE  (m_AXI_AWCACHE),
    .m_AXI_AWPROT   (m_AXI_AWPROT),
    .m_AXI_AWQOS    (m_AXI_AWQOS),
    .m_AXI_AWREGION (m_AXI_AWREGION),
    .m_AXI_AWVALID  (m_AXI_AWVALID),
    .m_AXI_AWREADY  (m_AXI_AWREADY),
    .m_AXI_WDATA    (m_AXI_WDATA),
    .m_AXI_WSTRB    (m_AXI_WSTRB),
    .m_AXI_WLAST    (m_AXI_WLAST),
    .m_AXI_WVALID   (m_AXI_WVALID),
    .m_AXI_WREADY   (m_AXI_WREADY),
    .m_AXI_BID      (m_AXI_BID),
    .m_AXI_BRESP    (m_AXI_BRESP),
    .m_AXI_BVALID   (m_AXI_BVALID),
    .m_AXI_BREADY   (m_AXI_BREADY),

    // 读侧：本用例用不到，显式接空（命令口接 0，读数据口悬空，R 通道全 0）
    .rd_cmd_valid   (1'b0),
    .rd_cmd_ready   (rd_cmd_ready),
    .rd_cmd_addr    (32'd0),
    .rd_cmd_len     (8'd0),
    .rd_cmd_done    (rd_cmd_done),
    .rd_cmd_err     (rd_cmd_err),
    .rd_data        (rd_data),
    .rd_valid       (rd_valid),
    .rd_last        (rd_last),
    .rd_ready       (1'b1),
    .m_AXI_ARID     (m_AXI_ARID),
    .m_AXI_ARADDR   (m_AXI_ARADDR),
    .m_AXI_ARLEN    (m_AXI_ARLEN),
    .m_AXI_ARSIZE   (m_AXI_ARSIZE),
    .m_AXI_ARBURST  (m_AXI_ARBURST),
    .m_AXI_ARLOCK   (m_AXI_ARLOCK),
    .m_AXI_ARCACHE  (m_AXI_ARCACHE),
    .m_AXI_ARPROT   (m_AXI_ARPROT),
    .m_AXI_ARQOS    (m_AXI_ARQOS),
    .m_AXI_ARREGION (m_AXI_ARREGION),
    .m_AXI_ARVALID  (m_AXI_ARVALID),
    .m_AXI_ARREADY  (1'b0),
    .m_AXI_RID      (8'd0),
    .m_AXI_RDATA    (128'd0),
    .m_AXI_RRESP    (2'd0),
    .m_AXI_RLAST    (1'b0),
    .m_AXI_RVALID   (1'b0),
    .m_AXI_RREADY   (m_AXI_RREADY)
);

////////////////////////////////////////////////////////////////////////////////
// AXI 写从设备模型：收 AW → 收 W（记下来）→ 回 B
////////////////////////////////////////////////////////////////////////////////
localparam SLV_IDLE = 2'd0, SLV_DATA = 2'd1, SLV_RESP = 2'd2;

reg  [1:0]   slv_state;
reg  [8:0]   slv_left;
integer      nburst, nbeat;
reg  [31:0]  burst_addr [0:63];
reg  [8:0]   burst_len  [0:63];
reg  [127:0] beat_data  [0:2047];
reg  [15:0]  beat_strb  [0:2047];

task clear_rec; begin nburst = 0; nbeat = 0; end endtask

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        slv_state   <= SLV_IDLE;
        m_AXI_AWREADY <= 1'b0;
        m_AXI_WREADY  <= 1'b0;
        m_AXI_BVALID  <= 1'b0;
        slv_left    <= 9'd0;
        nburst      <= 0;
        nbeat       <= 0;
        m_AXI_BID   <= 8'd0;
        m_AXI_BRESP <= 2'd0;
    end
    else begin
        case (slv_state)
            // ---- 等 AW ----
            SLV_IDLE: begin
                m_AXI_AWREADY <= 1'b1;
                m_AXI_BVALID  <= 1'b0;
                if (m_AXI_AWVALID && m_AXI_AWREADY) begin
                    burst_addr[nburst] <= m_AXI_AWADDR;
                    burst_len [nburst] <= m_AXI_AWLEN;
                    nburst        <= nburst + 1;
                    slv_left      <= {1'b0, m_AXI_AWLEN} + 9'd1;
                    m_AXI_AWREADY <= 1'b0;
                    m_AXI_WREADY  <= 1'b1;
                    slv_state     <= SLV_DATA;
                end
            end
            // ---- 收 W，直到把这一笔收满 ----
            SLV_DATA: begin
                if (m_AXI_WVALID && m_AXI_WREADY) begin
                    beat_data[nbeat] <= m_AXI_WDATA;
                    beat_strb[nbeat] <= m_AXI_WSTRB;
                    nbeat    <= nbeat + 1;
                    slv_left <= slv_left - 9'd1;
                    // ★ WLAST 必须正好落在 AWLEN 定的最后一拍。
                    //   Axi_Mux 的 W 通道是靠 WLAST 解锁的（Axi_Mux_Wr_Ctrl 里
                    //   W_Act_Cnl_End = M_W_Valid & M_W_Ready & M_W_Last）：
                    //   晚一拍 → W 通道永不解锁，整条 AXI 上的写（含 CPU 打串口）全堵死；
                    //   早一拍 → 本笔剩下的拍被 mux 丢掉，同样堵死。
                    if (m_AXI_WLAST !== (slv_left == 9'd1)) begin
                        $display("FAIL [WLAST]: 第 %0d 笔第 %0d 拍 WLAST=%b，AWLEN=%0d 说它在第 %0d 拍",
                                 nburst - 1, nbeat, m_AXI_WLAST,
                                 burst_len[nburst-1], burst_len[nburst-1] + 1);
                        err_cnt = err_cnt + 1;
                    end
                    if (slv_left == 9'd1) begin
                        m_AXI_WREADY <= 1'b0;
                        m_AXI_BVALID <= 1'b1;
                        slv_state    <= SLV_RESP;
                    end
                end
            end
            // ---- 回 B ----
            SLV_RESP: begin
                if (m_AXI_BVALID && m_AXI_BREADY) begin
                    m_AXI_BVALID <= 1'b0;
                    slv_state    <= SLV_IDLE;
                end
            end
            default: slv_state <= SLV_IDLE;
        endcase
    end
end

////////////////////////////////////////////////////////////////////////////////
// line_done / wr_free 监视
//   ★ 空行的 line_done 与 line_start 同拍（组合），等一拍去采就漏了 ——
//     所以统一在 posedge 上计数，用"计数变化"判完成，不再去追那个脉冲
////////////////////////////////////////////////////////////////////////////////
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        done_evt_cnt <= 0;
        wf_low_cnt   <= 0;
    end
    else begin
        if (line_done) done_evt_cnt <= done_evt_cnt + 1;
        if (!wr_free)  wf_low_cnt   <= wf_low_cnt + 1;
    end
end

////////////////////////////////////////////////////////////////////////////////
// 上游供数模型（TB 假扮 Compositor + DDRPacker）
//   收到 line_start 之后：
//     · 先等 data_delay 拍（模拟 BLIT 时源还在从 DDR 往回读）—— 期间 src_valid=0
//     · 然后拉高 src_valid，按【拍号】给数据：第 k 拍 = pat_of(k)
//     · 每被写主收走一拍（wr_valid & wr_ready），拍号 +1，数据跟着换
//   ★ 数据必须与被收走的那一拍同拍对齐：wr_data 是组合直连 src_data 的
//     （AXI4_Master_Write 里 m_AXI_WDATA = wr_data），所以"数据在第 k 次握手那一拍
//     呈现 pat_of(k)"就是唯一正确的相位。
//   ★ 拍号在 line_start 清 0 —— 换行必须重新从第 0 拍算起
////////////////////////////////////////////////////////////////////////////////
wire up_fire = wr_valid & wr_ready;     // 写主收走一拍

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        src_valid <= 1'b0;
        src_data  <= 128'd0;
        up_idx    <= 0;
        up_wait   <= 0;
        up_run    <= 1'b0;
    end
    else begin
        if (line_start) begin                    // 新的一行：重新备数
            up_idx    <= 0;
            up_wait   <= data_delay;
            up_run    <= 1'b1;
            src_valid <= 1'b0;
            src_data  <= pat_of(0);
        end
        else if (up_run) begin
            if (up_wait != 0) begin              // 数据还没备好
                up_wait   <= up_wait - 1;
                src_valid <= 1'b0;
            end
            else begin                           // 数据一直备着，被收走才换下一拍
                src_valid <= 1'b1;
                if (up_fire) begin
                    up_idx   <= up_idx + 1;
                    src_data <= pat_of(up_idx + 1);
                end
            end
        end
    end
end

////////////////////////////////////////////////////////////////////////////////
// 任务
////////////////////////////////////////////////////////////////////////////////
task tick; begin @(posedge clk); #1; end endtask

// 推一个行任务（line_start 单拍，只跨一个 posedge）
//   ★ done_base 必须在【拉起 line_start 之前】取：空行的 done 与 line_start 同拍，
//     晚一拍取基线会把这一行的 done 算到上一行头上
task push_line;
    input [31:0] dst;
    input [15:0] w;
    begin
        tick;
        done_base     = done_evt_cnt;
        // ★ 拉起 line_start 之前先把 src_valid 收回来
        //   上一行的数据早供完了，但 src_valid 可能还挂在 1 上；带着它给 line_start，
        //   DUT 那一拍就会看到 "line_ready & src_valid" 直接开进 S_CMD —— 那就不是
        //   "数据晚到"的场景了。真实上游（Compositor/DDRPacker）换行时会重新备数，
        //   这里照它的规矩来：数据没备好就是 0
        src_valid     = 1'b0;
        src_data      = pat_of(0);
        line_dst_addr = dst;
        line_w        = w;
        line_start    = 1'b1;
        tick;
        line_start    = 1'b0;
    end
endtask

// 等 line_done
task wait_done;
    input [8*24-1:0] tag;
    integer wd;
    begin
        wd = 0;
        while (done_evt_cnt <= done_base && wd < 200000) begin tick; wd = wd + 1; end
        if (done_evt_cnt <= done_base) begin
            $display("FAIL [%0s]: 等不到 line_done", tag);
            err_cnt = err_cnt + 1;
        end
        else $display("PASS [%0s]: line_done 到了（%0d 拍）", tag, wd);
        repeat (2) tick;                    // 让从设备回到 IDLE
    end
endtask

// 检查第 idx 笔的地址和拍数
task check_burst;
    input integer    idx;
    input [31:0]     eaddr;
    input [8:0]      elen;              // AWLEN = 拍数 - 1
    input [8*24-1:0] tag;
    begin
        if (burst_addr[idx] !== eaddr || burst_len[idx] !== elen) begin
            $display("FAIL [%0s]: 第 %0d 笔 addr=%h len=%0d，期望 addr=%h len=%0d",
                     tag, idx, burst_addr[idx], burst_len[idx], eaddr, elen);
            err_cnt = err_cnt + 1;
        end
        else $display("PASS [%0s]: 第 %0d 笔 addr=%h 拍数=%0d", tag, idx, burst_addr[idx], burst_len[idx] + 1);
    end
endtask

// 检查某一拍的 WSTRB
task check_strb;
    input integer    idx;
    input [15:0]     es;
    input [8*24-1:0] tag;
    begin
        if (beat_strb[idx] !== es) begin
            $display("FAIL [%0s]: 第 %0d 拍 WSTRB=%h，期望 %h", tag, idx, beat_strb[idx], es);
            err_cnt = err_cnt + 1;
        end
        else $display("PASS [%0s]: 第 %0d 拍 WSTRB=%h", tag, idx, beat_strb[idx]);
    end
endtask

// 逐拍查数据：第 k 拍必须是 pat_of(k)
//   ★ 这就是原来"常数色相位"那条用例的替身：相位生成搬走了，
//     现在这一行数据是上游按拍号喂的，写侧一个字节都不该改、
//     也不该多一拍少一拍（错位立刻能从数据里看出来）
task check_all_beats;
    input [8*24-1:0] tag;
    integer b, e0;
    begin
        e0 = err_cnt;
        for (b = 0; b < nbeat; b = b + 1) begin
            if (beat_data[b] !== pat_of(b)) begin
                $display("FAIL [%0s]: 第 %0d 拍数据 = %h，期望 %h（拍序错位）",
                         tag, b, beat_data[b], pat_of(b));
                err_cnt = err_cnt + 1;
            end
        end
        if (err_cnt == e0)
            $display("PASS [%0s]: %0d 拍数据与拍序一致（第 k 拍 = 16 个 8'(k*7)）", tag, nbeat);
    end
endtask

////////////////////////////////////////////////////////////////////////////////
// 主流程
////////////////////////////////////////////////////////////////////////////////
initial begin
    clk = 1'b0;
    forever #(CLK_HALF) clk = ~clk;
end

initial begin
    err_cnt = 0;
    rst_n = 1'b0;
    line_start = 1'b0; line_dst_addr = 32'd0; line_w = 16'd0;
    src_valid  = 1'b0; src_data = 128'd0;
    data_delay = 0; up_idx = 0; up_wait = 0; up_run = 1'b0;
    repeat (10) @(posedge clk);
    rst_n = 1'b1;
    repeat (5) @(posedge clk);

    //==========================================================
    $display("---- 用例1: 对齐 + 3 拍（16 像素 = 48 字节）----");
    clear_rec;
    data_delay = 0;
    wf_before = wf_low_cnt;
    push_line(32'h0800_0000, 16'd16);
    wait_done("对齐3拍");
    if (nburst !== 1 || nbeat !== 3) begin
        $display("FAIL: 笔数 %0d 总拍数 %0d，期望 1 / 3", nburst, nbeat);
        err_cnt = err_cnt + 1;
    end
    check_burst(0, 32'h0800_0000, 8'd2, "对齐3拍");
    check_strb(0, 16'hFFFF, "对齐3拍-首拍");
    check_strb(2, 16'hFFFF, "对齐3拍-末拍");
    check_all_beats("对齐3拍");
    // wr_free：干活期间必须是 0（上游才不会把下一行塞进来）
    if (wf_low_cnt == wf_before) begin
        $display("FAIL [对齐3拍]: 整行期间 wr_free 一直是 1（上游会以为能接新行）");
        err_cnt = err_cnt + 1;
    end
    else
        $display("PASS [对齐3拍]: 干活期间 wr_free 掉低 %0d 拍，收工后又回到 1（当前 %b）",
                 wf_low_cnt - wf_before, wr_free);

    //==========================================================
    $display("---- 用例2: 地址不对齐（低 4 位 = 4）----");
    clear_rec;
    push_line(32'h0800_0004, 16'd16);
    wait_done("不对齐");
    if (nburst !== 1 || nbeat !== 4) begin
        $display("FAIL: 笔数 %0d 总拍数 %0d，期望 1 / 4", nburst, nbeat);
        err_cnt = err_cnt + 1;
    end
    // ★ 突发地址是"清低 4 位"的拍网格基准（AXI 从机本来就按对齐地址逐拍走）；
    //   行首真实偏移 0x...004 由首拍 WSTRB 遮头体现，不再出现在 AWADDR 里
    check_burst(0, 32'h0800_0000, 8'd3, "不对齐");
    check_strb(0, 16'hFFF0, "不对齐-首拍遮头");
    check_strb(3, 16'h000F, "不对齐-末拍遮尾");
    check_all_beats("不对齐");

    //==========================================================
    $display("---- 用例3: 末拍不满（5 像素 = 15 字节，1 拍）----");
    clear_rec;
    push_line(32'h0800_1000, 16'd5);
    wait_done("末拍不满");
    if (nburst !== 1 || nbeat !== 1) begin
        $display("FAIL: 笔数 %0d 总拍数 %0d，期望 1 / 1", nburst, nbeat);
        err_cnt = err_cnt + 1;
    end
    check_burst(0, 32'h0800_1000, 8'd0, "末拍不满");
    check_strb(0, 16'h7FFF, "末拍不满-遮尾");
    check_all_beats("末拍不满");

    //==========================================================
    $display("---- 用例4: 跨 4KB 边界（从 0x0FF0 起写 48 字节）----");
    clear_rec;
    push_line(32'h0800_0FF0, 16'd16);
    wait_done("跨4KB");
    if (nburst !== 2 || nbeat !== 3) begin
        $display("FAIL: 笔数 %0d 总拍数 %0d，期望 2 / 3", nburst, nbeat);
        err_cnt = err_cnt + 1;
    end
    check_burst(0, 32'h0800_0FF0, 8'd0, "跨4KB-第0笔(1拍到页尾)");
    check_burst(1, 32'h0800_1000, 8'd1, "跨4KB-第1笔(2拍)");
    check_all_beats("跨4KB");

    //==========================================================
    $display("---- 用例5: 单笔 256 拍上限（1400 像素 = 4200 字节 = 263 拍）----");
    clear_rec;
    push_line(32'h0800_4000, 16'd1400);
    wait_done("256拍上限");
    if (nburst !== 2 || nbeat !== 263) begin
        $display("FAIL: 笔数 %0d 总拍数 %0d，期望 2 / 263", nburst, nbeat);
        err_cnt = err_cnt + 1;
    end
    check_burst(0, 32'h0800_4000, 8'd255, "上限-第0笔(256拍)");
    check_burst(1, 32'h0800_5000, 8'd6,   "上限-第1笔(7拍)");
    check_strb(262, 16'h00FF, "上限-整行末拍遮尾");
    check_all_beats("256拍上限");

    //==========================================================
    //  ★ 用例6（原来是"常数色相位"）：数据由上游按拍号给，
    //     这里故意让数据【晚 5 拍】才备好 —— 既验逐拍数据与拍序一致，
    //     也验"数据晚到不会让写侧提前收拍、把图案整体挪一格"
    $display("---- 用例6: 逐拍数据与拍序一致（数据晚 5 拍备好，160 像素 = 30 拍）----");
    clear_rec;
    data_delay = 5;
    push_line(32'h0800_7000, 16'd160);
    wait_done("逐拍数据");
    if (nburst !== 1 || nbeat !== 30) begin
        $display("FAIL: 笔数 %0d 总拍数 %0d，期望 1 / 30", nburst, nbeat);
        err_cnt = err_cnt + 1;
    end
    check_burst(0, 32'h0800_7000, 8'd29, "逐拍数据");
    check_all_beats("逐拍数据");
    data_delay = 0;

    //==========================================================
    $display("---- 用例7: 空行（宽 0）不挂死 ----");
    clear_rec;
    push_line(32'h0800_3000, 16'd0);
    wait_done("空行");
    if (nburst !== 0 || nbeat !== 0) begin
        $display("FAIL: 空行竟然发了 %0d 笔 %0d 拍", nburst, nbeat);
        err_cnt = err_cnt + 1;
    end
    else $display("PASS [空行]: 一笔都没发");

    //==========================================================
    $display("---- 用例8: 再来一条，确认前面跳过没留后遗症 ----");
    clear_rec;
    push_line(32'h0800_6000, 16'd8);
    wait_done("收尾");
    if (nburst !== 1 || nbeat !== 2) begin
        $display("FAIL: 笔数 %0d 总拍数 %0d，期望 1 / 2", nburst, nbeat);
        err_cnt = err_cnt + 1;
    end
    check_burst(0, 32'h0800_6000, 8'd1, "收尾");
    check_strb(1, 16'h00FF, "收尾-末拍(24字节→末拍8字节)");
    check_all_beats("收尾");

    //==========================================================
    //  ★ 用例9：行首不对齐 且 页尾只剩不到一拍 —— 死锁复现用例
    //     0x0800_0FFC 低 4 位 = 12，48 字节 → 跨度 0x0FFC~0x1030，跨 4KB。
    //     第 0 笔从对齐基准 0x0FF0 起，页内只够 1 拍；第 1 笔落到 0x1000。
    //     【旧实现】首笔地址用的是未对齐的 0x0FFC：页内余量 (4096-4092)>>4 整除得 0
    //     → 本笔拍数 0 → cmd_len 下溢成 255（256 拍），而数据只够 3 拍
    //     → 写主等不到 WLAST → Axi_Mux 的 W 通道被永久锁死（WLAST 才解锁）
    //     → 整条 AXI 的写全部堵死（CPU 打串口也堵）。本用例就是钉住这个坑。
    $display("---- 用例9: 行首不对齐 + 页尾不满一拍（旧实现的死锁点）----");
    clear_rec;
    push_line(32'h0800_0FFC, 16'd16);
    wait_done("页尾不对齐");
    if (nburst !== 2 || nbeat !== 4) begin
        $display("FAIL: 笔数 %0d 总拍数 %0d，期望 2 / 4", nburst, nbeat);
        err_cnt = err_cnt + 1;
    end
    check_burst(0, 32'h0800_0FF0, 8'd0, "页尾不对齐-第0笔(页内只够1拍)");
    check_burst(1, 32'h0800_1000, 8'd2, "页尾不对齐-第1笔(3拍)");
    check_strb(0, 16'hF000, "页尾不对齐-首拍遮头");
    check_strb(3, 16'h0FFF, "页尾不对齐-末拍遮尾");
    check_all_beats("页尾不对齐");

    //==========================================================
    //  ★ 用例10（新增）：数据晚到几十拍
    //     行任务在 line_start 那一拍就锁下来了，但数据要等 src_valid。
    //     验两件事：
    //       a) 数据没到之前【绝不开工】—— 不发 AW、不收一拍数据、line_done 不响
    //          （数据没备好就发 AW，Axi_Mux 的 W 通道会从那一刻锁给我们而写不出字节）
    //       b) 数据到了以后照常发 AW、正常走完、正常 line_done
    $display("---- 用例10: line_start 后晚 30 拍才拉 src_valid ----");
    clear_rec;
    data_delay = 30;
    push_line(32'h0800_9000, 16'd16);
    early_aw   = 1'b0;
    early_beat = 1'b0;
    for (k = 0; k < 15; k = k + 1) begin      // 这 15 拍里数据还没来
        tick;
        if (cmd_wr_valid) early_aw   = 1'b1;
        if (wr_valid)     early_beat = 1'b1;
    end
    if (early_aw || (nburst !== 0)) begin
        $display("FAIL [数据晚到]: 数据还没到就发了 AW（awvalid 出现过 %b，已收笔数 %0d）",
                 early_aw, nburst);
        err_cnt = err_cnt + 1;
    end
    else $display("PASS [数据晚到]: 等待期间一个 AW 都没发");

    if (early_beat || (nbeat !== 0)) begin
        $display("FAIL [数据晚到]: 数据还没到就在供数（wr_valid 出现过 %b，已收拍数 %0d）",
                 early_beat, nbeat);
        err_cnt = err_cnt + 1;
    end
    else $display("PASS [数据晚到]: 等待期间一拍数据都没被收走");

    //   观察（不断言）：这一拍的 wr_free 是 RTL 的 "state == S_IDLE"，
    //   行任务锁下之后、数据没来的这段时间状态【还在 S_IDLE】—— 打印出来存档
    $display("OBSERVE [数据晚到]: 等 src_valid 期间 wr_free=%b（line_pend 已置起，状态仍是 S_IDLE）",
             wr_free);

    wait_done("数据晚到");
    if (nburst !== 1 || nbeat !== 3) begin
        $display("FAIL: 笔数 %0d 总拍数 %0d，期望 1 / 3", nburst, nbeat);
        err_cnt = err_cnt + 1;
    end
    check_burst(0, 32'h0800_9000, 8'd2, "数据晚到");
    check_strb(0, 16'hFFFF, "数据晚到-首拍");
    check_strb(2, 16'hFFFF, "数据晚到-末拍");
    check_all_beats("数据晚到");
    data_delay = 0;

    $display("---- 结果 ----");
    if (err_cnt == 0) $display("\n*** 全部通过 ***");
    else              $display("\n*** 失败: err_cnt = %0d ***", err_cnt);
    //  ★ 最后打一行纯 ASCII 的结论：中文串在非 UTF-8 locale 的 ModelSim 里会被打成乱码
    //     （vlog 按系统 ANSI 读源文件，注释不受影响，字符串字面量会），
    //     这一行保证任何终端/日志编码下都能一眼看出过没过
    $display("SUMMARY: err_cnt=%0d  RESULT=%0s", err_cnt, (err_cnt == 0) ? "ALL-PASS" : "FAIL");
    $finish;
end

initial begin
    #2_000_000;
    $display("FAIL: 全局超时（很可能挂死了）");
    $finish;
end

endmodule
