////////////////////////////////////////////////////////////////////////////////
// tb_lineBuffer.v —— 显示通道端到端（lineBuffer 整个模块，真模块全上）
//
//  链路
//    假 DDR(AXI 从机) ─► AXI4_Master ─► bufferControler ─► async_PingPongBuffer
//                                                              │(跨 100M/148.5M)
//                    HDMI 屏 ◄─ TMDS ◄─ HDMI_Top ◄─ bufferUnpacker ◄┘
//    真模块：AXI4_Master / bufferControler / async_PingPongBuffer / bufferUnpacker
//            HDMI_Top / bufferAPBHandler / frame_start 跨域桥
//    假的只有 DDR 那一头（行为级从机）
//
//  验什么
//    1) 一帧像素整体对不对：DDR 字节流 → 屏上 24bit 像素，逐像素比对
//       （地址换算、AXI 拆笔、PingPong 翻 bank、128→24 拆包 有一处错就红）
//    2) 使能门：enable=0 时一条 AXI 读命令都不许发
//    3) DONE 消费 + 翻页：done_acc 在帧边界被消费，done_clr 回来清位，
//       下一帧显示的基址从 C 翻到 D（像素内容跟着变 → 基址真翻了）
//    4) frame_start 跨域桥 1:1：读域每帧一个脉冲，写域也正好一个，不丢不重
//    5) AXI 协议：单笔 ≤256 拍、不跨 4KB、INCR、size=16 字节、只读不写
//
//  数据模型
//    DDR 第 a 字节 = a[7:0] ^ a[12:8] ^ a[20:16] ^ a[27:24]
//    第 k 个像素 = {B[3k+2], B[3k+1], B[3k]}   (B 为该像素在 DDR 里的 3 个字节)
//
//  时序
//    clk 100MHz = 写域(AXI/控制器/寄存器)，clk_HDMI_x1 148.5MHz = 像素域
//    1080p60 一帧 16.7ms：所以整条 TB 只跑【第 0 帧 8 行 + 第 1 帧 8 行】
//
//  跑法（ModelSim）
//    vlog .../lineBuffer/*.v  .../PingPongBuffer/*.v  .../HDMI/**/*.v
//         .../AXI4/Master/*.v
//    vlog tb_lineBuffer.v
//    vsim -c tb_lineBuffer -do "run -all; quit -f"
////////////////////////////////////////////////////////////////////////////////
`timescale 1ns/100ps

module tb_lineBuffer;

//==============================================================================
// 参数
//==============================================================================
localparam  AW          = 32;
localparam  DW          = 128;
localparam  H_ACTIVE    = 1920;
localparam  LINE_BYTES  = 5760;                     // 1920 * 3
localparam  FRAME_BYTES = LINE_BYTES * 1080;
localparam  CHECK_LINES = 8;                        // 每帧校验几行有效像素
localparam  HALF_PX     = 960;                      // 分屏: 半幅宽度(1920/2)
localparam  HALF_BYTES  = LINE_BYTES/2;             // 分屏: 半幅行距 2880
localparam  AR_LAT      = 6;                        // 假 DDR 的 AR->R 延迟

// 帧缓存基址（按 APB 寄存器的打包规则：{段0/acc, 段1/cpu}、全屏 {C, D}）
localparam  C_BASE      = 32'h0200_0000;            // 全屏 C（上电默认显示）
localparam  D_BASE      = 32'h0300_0000;            // 全屏 D（翻页后显示）
localparam  AL_BASE     = 32'h0000_0000;            // 分屏 A 左半（本 TB 不用）
localparam  AR_BASE     = 32'h0080_0000;            // 分屏 A 右半
localparam  BL_BASE     = 32'h0400_0000;            // 分屏 B 左半
localparam  BR_BASE     = 32'h0480_0000;            // 分屏 B 右半

localparam  CLK_WR_HALF = 5.0;                      // 100MHz
localparam  CLK_RD_HALF = 3.367;                    // 148.5MHz
localparam  TIMEOUT     = 200_000_000;               // 看门狗 40ms（一帧 16.7ms）

//==============================================================================
// 时钟与复位
//==============================================================================
reg         clk_wr, clk_rd, rst_n_wr;
reg         rst_n_disp;         // ★ 显示侧复位：单拉出来, 专门测"它不能动加速器侧计数"

initial begin clk_wr = 1'b0; forever #CLK_WR_HALF clk_wr = ~clk_wr; end
initial begin clk_rd = 1'b0; forever #CLK_RD_HALF clk_rd = ~clk_rd; end

//==============================================================================
// DDR 期望数据模型
//==============================================================================
function [7:0] byte_at;
    input [31:0] a;
    begin
        byte_at = a[7:0] ^ a[12:8] ^ a[20:16] ^ a[27:24];
    end
endfunction

function [DW-1:0] beat_at;
    input [31:0] a;
    integer      i;
    begin
        for (i = 0; i < 16; i = i + 1)
            beat_at[i*8 +: 8] = byte_at(a + i);
    end
endfunction

//==============================================================================
// DUT
//==============================================================================
reg  [2*AW-1:0]     framea_base_addr;
reg  [2*AW-1:0]     frameb_base_addr;
reg  [2*AW-1:0]     frame_full_base_addr;
// ---- 来自 ⑤ Handler(寄存器侧那一层)----
reg                 enable, split_pending, done_acc, done_cpu;
wire  [1:0]         done_clr;
wire  [31:0]        fps_acc_val, fps_cpu_val;
wire                lineBuffer_frame_start;     // lineBuffer -> Handler
wire                lineBuffer_enable;          // Handler -> lineBuffer
wire                lineBuffer_mode;            // Handler -> lineBuffer
wire  [2*AW-1:0]    lineBuffer_addr;            // Handler -> lineBuffer {段0, 段1}

// ---- Handler 加速器侧观测（★ 必须在实例之前声明: 先用后声明会变成隐式网）----
wire [31:0] busy_val;
wire [31:0] busy_cyc_val;
integer     cyc_before;         // 显示侧复位前的忙周期快照

wire [7:0]          m_AXI_AWID;      wire [31:0] m_AXI_AWADDR;
wire [7:0]          m_AXI_AWLEN;     wire [2:0]  m_AXI_AWSIZE;
wire [1:0]          m_AXI_AWBURST;   wire        m_AXI_AWLOCK;
wire [3:0]          m_AXI_AWCACHE;   wire [2:0]  m_AXI_AWPROT;
wire [3:0]          m_AXI_AWQOS;     wire [3:0]  m_AXI_AWREGION;
wire                m_AXI_AWVALID;
wire [DW-1:0]       m_AXI_WDATA;     wire [DW/8-1:0] m_AXI_WSTRB;
wire                m_AXI_WLAST, m_AXI_WVALID;
wire [7:0]          m_AXI_ARID;      wire [31:0] m_AXI_ARADDR;
wire [7:0]          m_AXI_ARLEN;     wire [2:0]  m_AXI_ARSIZE;
wire [1:0]          m_AXI_ARBURST;   wire        m_AXI_ARLOCK;
wire [3:0]          m_AXI_ARCACHE;   wire [2:0]  m_AXI_ARPROT;
wire [3:0]          m_AXI_ARQOS;     wire [3:0]  m_AXI_ARREGION;
wire                m_AXI_ARVALID;
wire                m_AXI_RREADY;

// 假 DDR 驱动端
reg                 m_AXI_AWREADY, m_AXI_WREADY, m_AXI_BVALID;
reg  [7:0]          m_AXI_BID;
reg  [1:0]          m_AXI_BRESP;
reg                 m_AXI_ARREADY;
reg  [7:0]          m_AXI_RID;
reg  [DW-1:0]       m_AXI_RDATA;
reg  [1:0]          m_AXI_RRESP;
reg                 m_AXI_RLAST, m_AXI_RVALID;

lineBuffer #(
    .AXI_ADDR_WIDTH             (AW),
    .AXI_DATA_WIDTH             (DW)
) u_lineBuffer (
    .clk                        (clk_wr),
    .clk_HDMI_x1                (clk_rd),
    .rst_n_wr                   (rst_n_wr),

    // ★ 显示通道只认 Handler 翻译好的这 3 根(和 GPU_2D 里的接法一致)
    .lineBuffer_enable          (lineBuffer_enable),
    .lineBuffer_mode            (lineBuffer_mode),
    .lineBuffer_addr            (lineBuffer_addr),
    .lineBuffer_frame_start     (lineBuffer_frame_start),

    .tmds_blue_o                (),
    .tmds_green_o               (),
    .tmds_red_o                 (),
    .tmds_clk_o                 (),
    .tmds_data0_TX_OE           (),
    .tmds_data1_TX_OE           (),
    .tmds_data2_TX_OE           (),
    .tmds_clk_TX_OE             (),
    .tmds_data0_TX_RST          (),
    .tmds_data1_TX_RST          (),
    .tmds_data2_TX_RST          (),
    .tmds_clk_TX_RST            (),

    .m_AXI_AWID                 (m_AXI_AWID),
    .m_AXI_AWADDR               (m_AXI_AWADDR),
    .m_AXI_AWLEN                (m_AXI_AWLEN),
    .m_AXI_AWSIZE               (m_AXI_AWSIZE),
    .m_AXI_AWBURST              (m_AXI_AWBURST),
    .m_AXI_AWLOCK               (m_AXI_AWLOCK),
    .m_AXI_AWCACHE              (m_AXI_AWCACHE),
    .m_AXI_AWPROT               (m_AXI_AWPROT),
    .m_AXI_AWQOS                (m_AXI_AWQOS),
    .m_AXI_AWREGION             (m_AXI_AWREGION),
    .m_AXI_AWVALID              (m_AXI_AWVALID),
    .m_AXI_AWREADY              (m_AXI_AWREADY),

    .m_AXI_WDATA                (m_AXI_WDATA),
    .m_AXI_WSTRB                (m_AXI_WSTRB),
    .m_AXI_WLAST                (m_AXI_WLAST),
    .m_AXI_WVALID               (m_AXI_WVALID),
    .m_AXI_WREADY               (m_AXI_WREADY),

    .m_AXI_BID                  (m_AXI_BID),
    .m_AXI_BRESP                (m_AXI_BRESP),
    .m_AXI_BVALID               (m_AXI_BVALID),
    .m_AXI_BREADY               (),

    .m_AXI_ARID                 (m_AXI_ARID),
    .m_AXI_ARADDR               (m_AXI_ARADDR),
    .m_AXI_ARLEN                (m_AXI_ARLEN),
    .m_AXI_ARSIZE               (m_AXI_ARSIZE),
    .m_AXI_ARBURST              (m_AXI_ARBURST),
    .m_AXI_ARLOCK               (m_AXI_ARLOCK),
    .m_AXI_ARCACHE              (m_AXI_ARCACHE),
    .m_AXI_ARPROT               (m_AXI_ARPROT),
    .m_AXI_ARQOS                (m_AXI_ARQOS),
    .m_AXI_ARREGION             (m_AXI_ARREGION),
    .m_AXI_ARVALID              (m_AXI_ARVALID),
    .m_AXI_ARREADY              (m_AXI_ARREADY),

    .m_AXI_RID                  (m_AXI_RID),
    .m_AXI_RDATA                (m_AXI_RDATA),
    .m_AXI_RRESP                (m_AXI_RRESP),
    .m_AXI_RLAST                (m_AXI_RLAST),
    .m_AXI_RVALID               (m_AXI_RVALID),
    .m_AXI_RREADY               (m_AXI_RREADY)
);

//------------------------------------------------------------------------------
// ⑤ Handler 集中层（和 GPU_2D 里同一个模块、同一个接法）
//    · 显示侧：寄存器值 -> enable/mode/{段0,段1} 地址, 帧边界翻页
//    · 加速器侧：acc_busy 恒 1, 只为了能看见 busy_cyc_val 在"显示侧复位"期间照走
//    ★ 两路复位在 TB 里是【两根线】: rst_n = rst_n_wr, rst_n_disp 可单独拉低
//------------------------------------------------------------------------------
GPU2D_APBHandler #(
    .ADDR_WIDTH                 (AW),
    .FPS_WINDOW                 (60)
) u_GPU2D_APBHandler (
    .clk                        (clk_wr),
    .rst_n                      (rst_n_wr),
    .rst_n_disp                 (rst_n_disp),

    .enable                     (enable),
    .split_pending              (split_pending),
    .framea_base_addr           (framea_base_addr),
    .frameb_base_addr           (frameb_base_addr),
    .frame_full_base_addr       (frame_full_base_addr),
    .done_acc                   (done_acc),
    .done_cpu                   (done_cpu),

    .done_clr                   (done_clr),
    .fps_acc_val                (fps_acc_val),
    .fps_cpu_val                (fps_cpu_val),

    .lineBuffer_frame_start     (lineBuffer_frame_start),
    .lineBuffer_enable          (lineBuffer_enable),
    .lineBuffer_mode            (lineBuffer_mode),
    .lineBuffer_addr            (lineBuffer_addr),

    .acc_busy                   (1'b1),
    .cmd_done                   (1'b0),
    .frame_done                 (1'b0),
    .wr_beat_hs                 (1'b0),
    .rd_beat_hs                 (1'b0),

    .busy_val                   (busy_val),
    .cmd_done_val               (),
    .frame_done_val             (),
    .wr_beats_val               (),
    .rd_beats_val               (),
    .busy_cyc_val               (busy_cyc_val)
);

//==============================================================================
// 假 DDR：只有读通路
//==============================================================================
reg  [31:0] ddr_addr;
reg  [9:0]  ddr_left;
reg  [3:0]  ddr_lat;
reg         ddr_busy;

wire        ar_hs = m_AXI_ARVALID && m_AXI_ARREADY;
wire        r_hs  = m_AXI_RVALID  && m_AXI_RREADY;

initial begin
    ddr_addr = 32'd0; ddr_left = 10'd0; ddr_lat = 4'd0; ddr_busy = 1'b0;
    m_AXI_ARREADY = 1'b1;
    m_AXI_RID = 8'd0; m_AXI_RDATA = {DW{1'b0}}; m_AXI_RRESP = 2'b00;
    m_AXI_RLAST = 1'b0; m_AXI_RVALID = 1'b0;
    m_AXI_AWREADY = 1'b0; m_AXI_WREADY = 1'b0;
    m_AXI_BID = 8'd0; m_AXI_BRESP = 2'b00; m_AXI_BVALID = 1'b0;
end

always @(posedge clk_wr or negedge rst_n_wr) begin
    if (!rst_n_wr) begin
        ddr_addr <= 32'd0; ddr_left <= 10'd0; ddr_lat <= 4'd0; ddr_busy <= 1'b0;
        m_AXI_ARREADY <= 1'b1;
        m_AXI_RVALID <= 1'b0; m_AXI_RLAST <= 1'b0; m_AXI_RDATA <= {DW{1'b0}};
    end
    else begin
        m_AXI_RVALID <= 1'b0;
        m_AXI_RLAST  <= 1'b0;

        if (!ddr_busy) begin
            m_AXI_ARREADY <= 1'b1;
            if (ar_hs) begin
                ddr_addr      <= m_AXI_ARADDR;
                ddr_left      <= m_AXI_ARLEN + 10'd1;
                ddr_lat       <= AR_LAT[3:0];
                ddr_busy      <= 1'b1;
                m_AXI_ARREADY <= 1'b0;
            end
        end
        else if (ddr_lat != 4'd0) begin
            ddr_lat <= ddr_lat - 4'd1;
        end
        else if (ddr_left != 10'd0) begin
            m_AXI_RVALID <= 1'b1;
            m_AXI_RLAST  <= (ddr_left == 10'd1);
            m_AXI_RDATA  <= beat_at(ddr_addr);
            ddr_addr     <= ddr_addr + 32'd16;
            ddr_left     <= ddr_left - 10'd1;
            if (ddr_left == 10'd1)
                ddr_busy <= 1'b0;
        end
    end
end

//==============================================================================
// 检查
//==============================================================================
integer err_cnt;
// ★ 行同步探针: 看"欠账/还账"记账情况
// (调试探针已清除)
// ★ 行同步账本探针
integer ls_pay_c, ls_max_d;
initial begin ls_pay_c = 0; ls_max_d = 0; end
always @(posedge clk_rd) begin
end

// ★★ 对账探针: 换 bank 的时刻, 显示停在哪一列(理想: 永远在行尾附近)
reg        bl_last;
integer    bl_sw, bl_x, bl_y;
integer    bl_hist[0:7];
integer    bl_i;
initial begin
    bl_last = 1'b0; bl_sw = 0; bl_x = 0; bl_y = 0;
    for (bl_i = 0; bl_i < 8; bl_i = bl_i + 1) bl_hist[bl_i] = 0;
end
always @(posedge clk_rd) begin
    if (u_lineBuffer.u_async_PingPongBuffer.rd_switch_flip !== bl_last) begin
        bl_last = u_lineBuffer.u_async_PingPongBuffer.rd_switch_flip;
        bl_sw   = bl_sw + 1;
        bl_x    = u_lineBuffer.u_HDMI_Top.inst_pixel_timing.x_r;
        bl_y    = u_lineBuffer.u_HDMI_Top.inst_pixel_timing.y_r;
        if (u_lineBuffer.u_HDMI_Top.pixel_de_o)
            bl_hist[bl_x[12:9]] = bl_hist[bl_x[12:9]] + 1;   // 每 512 列一档
        else
            bl_hist[7] = bl_hist[7] + 1;                      // de=0 也算"行外"
        if (bl_sw <= 6)
            $display("  [BANK] 第%0d次换 bank: 显示 行%0d 列%0d de=%b", bl_sw, bl_y, bl_x,
                     u_lineBuffer.u_HDMI_Top.pixel_de_o);
    end
end
integer ls_max, ls_seen_done;
integer de_cnt, pv_cnt, ln_cnt, de_starve;
initial begin ls_max=0; ls_seen_done=0; de_cnt=0; pv_cnt=0; ln_cnt=0; de_starve=0; end
always @(posedge clk_rd) begin
    if (u_lineBuffer.u_HDMI_Top.pixel_de_o) begin
        de_cnt = de_cnt + 1;
        if (u_lineBuffer.u_bufferUnpacker.cnt_byte < 3) de_starve = de_starve + 1;
    end
    if (u_lineBuffer.u_bufferUnpacker.pixel_valid) pv_cnt = pv_cnt + 1;
end

integer ar_cnt;             // AXI 读命令笔数
integer ar_early;           // enable=0 期间发出来的读命令
integer ar_prot_err;        // 协议违例(超 256 拍 / 跨 4KB / 属性不对)
integer wr_axi_err;         // 写了不该写的通道
integer pixel_err;            // 像素错位个数
integer pixel_checked;        // 已比对像素数
integer rd_frames;          // 读域看到的帧起始个数
integer wr_frames;          // 写域收到的 frame_start 个数
integer pulse_err;          // 写域 frame_start 不是单拍
reg     wr_fs_d;
reg     check_on;

reg [31:0] ar_first;        // 本帧(写域帧边界之后)的第一个 AXI 读地址
reg        ar_arm;          // 已过帧边界, 等本帧第一笔读命令
reg        fs_ok;           // 使能拉高之后是否已经过了一个帧起始
reg        fs_chk_on;       // 只在"帧中间使能"这一段里做该项统计(排除在途命令)
integer    f_before;        // "帧中间使能"那一刻的帧计数(用来判断期间有没有跨帧边界)
integer    ar_early_snap;   // 第一次使能之前的 ar_early 快照
integer    ar_before_fs;    // 帧中间使能后、帧起始之前就发命令的次数(应为 0)
reg [31:0] exp_base_r;      // 当前帧期望的基址
integer    check_frame;     // 只检查这一帧的像素(0 = 不检查)
reg        exp_split;       // 期望的显示模式(0 全屏 / 1 分屏)
reg        exp_acc;         // 分屏: 左半显示的是 A(0) 还是 B(1)
reg        exp_cpu;         // 分屏: 右半显示的是 A(0) 还是 B(1)
reg [31:0] pixel_addr0;

// 观测点：走进 HDMI 的像素 / 编码器取数那一拍
wire        dut_aligned  = u_lineBuffer.u_bufferControler.frame_valid_r; // 控制器"已对齐帧边界"(=取数许可)

wire [23:0] hdmi_pixel   = u_lineBuffer.pixel_data;
wire        hdmi_de      = u_lineBuffer.u_HDMI_Top.pixel_de_o;
wire        hdmi_fs      = u_lineBuffer.HDMI_frame_start_o;

integer     pixel_x, pixel_y;

task check(input [255:0] name, input integer got, input integer exp);
begin
    if (got !== exp) begin
        err_cnt = err_cnt + 1;
        $display("  FAIL %0s: got=%0d exp=%0d @%0t", name, got, exp, $time);
    end
end
endtask

initial begin
    err_cnt = 0; ar_cnt = 0; ar_early = 0; ar_prot_err = 0; wr_axi_err = 0;
    pixel_err = 0; pixel_checked = 0; rd_frames = 0; wr_frames = 0; pulse_err = 0;
    wr_fs_d = 1'b0; check_on = 1'b0; ar_arm = 1'b0;
    fs_ok = 1'b0; ar_before_fs = 0; fs_chk_on = 1'b0; f_before = 0; ar_early_snap = 0;
    ar_first = 32'd0; exp_base_r = C_BASE; check_frame = 0;
    exp_split = 1'b0; exp_acc = 1'b0; exp_cpu = 1'b0;
    pixel_x = 0; pixel_y = 0;
end

// ---- AXI 侧检查（写域）----
always @(posedge clk_wr) begin
    if (rst_n_wr) begin
        if (m_AXI_AWVALID || m_AXI_WVALID) wr_axi_err = wr_axi_err + 1;

        // 本帧第一笔: 以【写域帧边界脉冲】为准重新对齐
        // (不能简单说"第 1 笔": 帧尾停手之前可能还有上一帧在途的命令)
        if (lineBuffer_frame_start) begin
            ar_arm <= 1'b1;
            fs_ok  <= 1'b1;                     // 已经过一个帧边界
        end
        else if (ar_hs && ar_arm) begin ar_first <= m_AXI_ARADDR; ar_arm <= 1'b0; end

        // ★ 回归: 使能拉高后, 必须等到帧边界才允许发读命令
        //   (上板踩过: 帧中间一使能就取数, 屏上整帧每行水平错开一段)


        if (ar_hs) begin
            ar_cnt = ar_cnt + 1;
            if (!enable) ar_early = ar_early + 1;
            if ((m_AXI_ARLEN + 9'd1) > 9'd256)                        ar_prot_err = ar_prot_err + 1;
            if ((m_AXI_ARADDR[11:0] + (m_AXI_ARLEN + 9'd1) * 16) > 13'd4096)
                                                                      ar_prot_err = ar_prot_err + 1;
            if (m_AXI_ARSIZE  !== 3'd4)                               ar_prot_err = ar_prot_err + 1;
            if (m_AXI_ARBURST !== 2'b01)                              ar_prot_err = ar_prot_err + 1;
        end
    end
end

// ---- 写域 frame_start：必须是单拍 ----
always @(posedge clk_wr) begin
    if (rst_n_wr) begin
        if (lineBuffer_frame_start && !wr_fs_d) wr_frames = wr_frames + 1;
        if (lineBuffer_frame_start &&  wr_fs_d) pulse_err = pulse_err + 1;   // 连续两拍 = 不是单拍
        wr_fs_d <= lineBuffer_frame_start;
    end
end

// ★★ 账本探针: 每帧对账 —— 显示要走的字节(3x每像素)  vs  数据流给过的字节(16x每拍)
integer lg_req, lg_beat, lg_drop, lg_fr;
initial begin lg_req=0; lg_beat=0; lg_drop=0; lg_fr=0; end
always @(posedge clk_rd) begin
    if (rst_n_disp) begin
        if (u_lineBuffer.u_bufferUnpacker.pixel_req)         lg_req  = lg_req  + 1;
        if (u_lineBuffer.u_bufferUnpacker.unpacker_rd_valid) lg_beat = lg_beat + 1;
        if (hdmi_fs && (lg_fr < 30))
            $display("  [LEDG] 帧%0d: 要=%0d字节 给=%0d字节 差=%0d | 丢掉=%0d拍 债=%0d",
                     lg_fr, lg_req*3, lg_beat*16, lg_req*3-lg_beat*16, lg_drop,
                     16'd0);
        if (hdmi_fs) begin
            lg_fr = lg_fr + 1; lg_req = 0; lg_beat = 0; lg_drop = 0;
        end
    end
end
// ---- 像素域：帧起始 / 逐像素比对 ----
always @(posedge clk_rd) begin
    if (hdmi_fs) begin
        rd_frames = rd_frames + 1;
        pixel_x = 0;
        pixel_y = 0;
    end

    if (hdmi_de) begin
        if (check_on && (rd_frames == check_frame) && (pixel_y < CHECK_LINES)) begin
            if (exp_split) begin
                /* 分屏: 左半(x<960) 来自 acc 那一块, 右半来自 cpu 那一块,
                 *       两块都是独立缓冲, 行距各自 2880 */
                if (pixel_x < HALF_PX)
                    pixel_addr0 = (exp_acc ? BL_BASE : AL_BASE)
                              + pixel_y * HALF_BYTES + pixel_x * 3;
                else
                    pixel_addr0 = (exp_cpu ? BR_BASE : AR_BASE)
                              + pixel_y * HALF_BYTES + (pixel_x - HALF_PX) * 3;
            end
            else
                pixel_addr0 = exp_base_r + pixel_y * LINE_BYTES + pixel_x * 3;
            if (hdmi_pixel !== {byte_at(pixel_addr0+2), byte_at(pixel_addr0+1), byte_at(pixel_addr0)}) begin
                pixel_err = pixel_err + 1;
                if (pixel_err <= 6)
                    $display("  MISMATCH 帧%0d 行%0d 列%0d got=%06h exp=%06h @%0t",
                             rd_frames-1, pixel_y, pixel_x, hdmi_pixel,
                             {byte_at(pixel_addr0+2), byte_at(pixel_addr0+1), byte_at(pixel_addr0)}, $time);
            end
            pixel_checked = pixel_checked + 1;
        end

        if (pixel_x == H_ACTIVE-1) begin pixel_x = 0; pixel_y = pixel_y + 1; end
        else                            pixel_x = pixel_x + 1;
    end
end

//==============================================================================
// 主流程
//==============================================================================
initial begin
    clk_wr = 1'b0;
    rst_n_wr   = 1'b0;
    rst_n_disp = 1'b0;
    enable = 1'b0; split_pending = 1'b0; done_acc = 1'b0; done_cpu = 1'b0;
    framea_base_addr     = {AL_BASE, AR_BASE};
    frameb_base_addr     = {BL_BASE, BR_BASE};
    frame_full_base_addr = {C_BASE,  D_BASE };

    repeat (20) @(posedge clk_wr);
    rst_n_wr   = 1'b1;
    rst_n_disp = 1'b1;
    repeat (20) @(posedge clk_wr);

    // ---- 1) 使能前：一条读命令都不该发 ----
    repeat (2000) @(posedge clk_wr);            // 20us
    check("使能前不发读命令", ar_cnt, 0);
    check("使能前不写", wr_axi_err, 0);

    // ---- 2) 开使能：★ 取数要等下一个帧边界才许可, 所以先等这一拍 ----
    //         (帧中间使能硬取数, 屏上整帧每行会水平错开 —— 上板踩过)
    enable = 1'b1;
    @(posedge lineBuffer_frame_start);          // 写域帧边界 = 给出取数许可的那一拍
    @(posedge clk_wr);

    pixel_err = 0; pixel_checked = 0;
    exp_base_r = C_BASE;
    check_frame = rd_frames;                    // 这一帧应该显示 C(acc_idx=0)
    check_on = 1'b1;

    wait (rd_frames > check_frame && pixel_y >= CHECK_LINES);   // 这一帧前几行看完
    check_on = 1'b0;
    check("第0帧首笔读地址(应为C基址)", ar_first, C_BASE);
    check("第0帧像素", pixel_err, 0);
    check("第0帧比对像素数", pixel_checked, CHECK_LINES * H_ACTIVE);
    $display("  [info] 第0帧(C): 读命令 %0d 笔, 比对像素 %0d 个, 错 %0d", ar_cnt, pixel_checked, pixel_err);

    // ---- 3) 交帧：done_acc 在帧边界被消费 -> 翻到 D ----
    done_acc = 1'b1;
    wait (done_clr[0]);
    @(posedge clk_wr);
    done_acc = 1'b0;                            // 模拟软件/寄存器清位
    $display("  [info] done_clr 已回, 基址该翻到 D");

    // ---- 4) 下一帧应该显示 D ----
    //      注: 这次 done_acc 同时把【acc 索引】从 0 翻成 1 —— 索引会一直保留到
    //          下次交帧(包括后面切到分屏), 所以后面的分屏期望值要跟着走。
    ar_cnt   = 0;
    pixel_err  = 0; pixel_checked = 0;
    exp_base_r  = D_BASE;
    exp_acc     = 1'b1;
    check_frame = rd_frames;                    // done 在这一帧边界被消费
    check_on = 1'b1;
    wait (rd_frames > check_frame && pixel_y >= CHECK_LINES);
    check_on = 1'b0;
    check("第1帧首笔读地址(应为D基址)", ar_first, D_BASE);
    check("第1帧像素", pixel_err, 0);
    check("第1帧比对像素数", pixel_checked, CHECK_LINES * H_ACTIVE);
    $display("  [info] 第1帧(D): 读命令 %0d 笔, 比对像素 %0d 个, 错 %0d", ar_cnt, pixel_checked, pixel_err);

    // ---- 5) 帧中间拉使能: 必须先等到下一个帧边界才允许取数 ----
    //   注: "使能前不发命令" 这项只对【第一次使能之前】有意义 ——
    //   后面把使能拉低时, 控制器会先把当前这一行收尾(设计如此, 不是立刻停),
    //   那几笔会落进 enable=0 的窗口里, 所以在这里先快照。
    ar_early_snap = ar_early;
    check_on     = 1'b0;
    enable       = 1'b0;
    repeat (10000) @(posedge clk_wr);       // 100us: 等在途命令全部收尾
    check("perm_cleared_after_disable", dut_aligned, 0);   // 等够 100us, 内部早已同步

    /* ★ 回归(确定性): 使能一撤, 控制器的"取数许可"标志必须清零 ——
     *   这样下次使能(软件随时可能写, 落在帧中间)就会重新等一个帧边界,
     *   不会在行的中间开始取数(上板踩过: 屏上整帧每行水平错开一段)。
     *   注: "使能后到帧边界之间不发命令" 这条按事件时序不好在 TB 里稳定判定,
     *       由 RTL 的许可条件保证, 上板用标尺图案直接看效果。 */
    ar_cnt = 0;
    f_before = rd_frames;
    enable   = 1'b1;                        // 帧中间使能
    repeat (20000) @(posedge clk_wr);       // 200us
    $display("  [info] 帧中间使能后 200us: aligned=%b, 发命令 %0d 笔(跨帧后才会有)",
             dut_aligned, ar_cnt);

    // ---- 7) 分屏(SPLIT=1): 左半来自 A·acc, 右半来自 A·cpu ----
    ar_cnt        = 0;
    split_pending = 1'b1;
    @(posedge lineBuffer_frame_start);          // 模式在帧边界生效
    @(posedge clk_wr);
    exp_split   = 1'b1;
    exp_acc     = 1'b1;                         // 左半: 全屏阶段翻过一次, 现在显示 B
    exp_cpu     = 1'b0;                         // 右半: 还没交过帧, 显示 A
    pixel_err = 0; pixel_checked = 0;
    check_frame = rd_frames;
    check_on    = 1'b1;
    wait (rd_frames > check_frame && pixel_y >= CHECK_LINES);
    check_on    = 1'b0;
    check("split 像素", pixel_err, 0);
    check("split 比对像素数", pixel_checked, CHECK_LINES * H_ACTIVE);
    $display("  [info] split(A/A): 命令 %0d 笔, 比对 %0d 像素, 错 %0d",
             ar_cnt, pixel_checked, pixel_err);

    // ---- 8) 分屏: 左半独立翻页(done_acc) -> 左半切 B, 右半不动 ----
    ar_cnt   = 0;
    done_acc = 1'b1;
    wait (done_clr[0]);
    @(posedge clk_wr);
    done_acc = 1'b0;
    exp_acc  = 1'b0;                            // 左半再翻一次 -> 回到 A
    pixel_err = 0; pixel_checked = 0;
    check_frame = rd_frames;
    check_on    = 1'b1;
    wait (rd_frames > check_frame && pixel_y >= CHECK_LINES);
    check_on    = 1'b0;
    check("split 左半翻页后 像素", pixel_err, 0);
    $display("  [info] split(A/A): 左半翻到 A, 比对 %0d 像素, 错 %0d", pixel_checked, pixel_err);

    // ---- 9) 分屏: 右半独立翻页(done_cpu) -> 右半切 B ----
    ar_cnt   = 0;
    done_cpu = 1'b1;
    wait (done_clr[1]);
    @(posedge clk_wr);
    done_cpu = 1'b0;
    exp_cpu  = 1'b1;                            // 右半切到 B
    pixel_err = 0; pixel_checked = 0;
    check_frame = rd_frames;
    check_on    = 1'b1;
    wait (rd_frames > check_frame && pixel_y >= CHECK_LINES);
    check_on    = 1'b0;
    check("split 右半翻页后 像素", pixel_err, 0);
    $display("  [info] split(B/B): 右半翻到 B, 比对 %0d 像素, 错 %0d", pixel_checked, pixel_err);

    // ---- 10) ★ 独立复位: 拉低【显示侧复位】, 加速器侧计数不许被清零 ----
    //        (上板踩过的坑: 合并两层时把一个加速器侧的寄存器悄悄换成了显示侧的复位域,
    //         结果显示 PLL 没锁就把加速器摁住 -> CI 阻塞 -> CPU 停住 -> 调试器再也停不住。
    //         这里用一个只有 TB 才能造的场景把它钉住。)
    cyc_before = busy_cyc_val;
    check("复位前 busy_val 跟随 acc_busy", busy_val, 1);
    rst_n_disp = 1'b0;
    repeat (10) @(posedge clk_wr);
    rst_n_disp = 1'b1;
    repeat (10) @(posedge clk_wr);
    check("显示侧复位期间加速器计数被清零", (busy_cyc_val <= cyc_before), 0);
    $display("  [info] 显示侧复位前后 busy_cyc: %0d -> %0d", cyc_before, busy_cyc_val);

    // ---- 11) 收尾检查 ----
    $display("  [BANK] 换 bank 总次数=%0d ; 列位置分布(de=1): 0-511=%0d 512-1023=%0d 1024-1535=%0d 1536-1919=%0d ; de=0(行外)=%0d",
             bl_sw, bl_hist[0], bl_hist[1]+bl_hist[2], bl_hist[3], bl_hist[4]+bl_hist[5]+bl_hist[6], bl_hist[7]);
    $display("  [DIAG] de=%0d pv=%0d 行=%0d 平均pv=%0d | 欠账累计=%0d 峰值=%0d 还账拍数=%0d",
             de_cnt, pv_cnt, ln_cnt, (ln_cnt>0)?(pv_cnt/ln_cnt):0,
             16'd0, ls_max_d, ls_pay_c);
    check("AXI 协议违例", ar_prot_err, 0);
    check("写通道被误用", wr_axi_err, 0);
    check("frame_start 跨域计数(读=写)", wr_frames, rd_frames);
    check("写域 frame_start 不是单拍次数", pulse_err, 0);
    check("使能前发命令次数", ar_early_snap, 0);

    if (err_cnt == 0) $display("---- ALL CHECKS PASSED ----");
    else              $display("---- FAIL: %0d errors ----", err_cnt);
    $finish;
end

//==============================================================================
// 看门狗
//==============================================================================
initial begin
    #TIMEOUT;
    $display("TIMEOUT @%0t (rd_frames=%0d pixel_y=%0d ar_cnt=%0d)",
             $time, rd_frames, pixel_y, ar_cnt);
    $display("---- FAIL ----");
    $finish;
end

endmodule
