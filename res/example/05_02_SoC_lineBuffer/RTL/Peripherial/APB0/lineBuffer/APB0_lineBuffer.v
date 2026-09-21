//==============================================================================
//  APB0_lineBuffer.v  ——  行缓冲器(显示通道)的总线解析层
//------------------------------------------------------------------------------
//  本文件只管"总线 + 寄存器", 不含功能逻辑:
//      寄存器值从这里出去, 交给 bufferAPBHandler 翻译成 controller 要的信号;
//      功能的"事件"和"值"再从 handler 送回来(DONE 清零脉冲 / 帧率值 / 生效的模式)
//
//  寄存器表(槽内偏移, 绝对地址 = 0xf8102000 + 偏移):
//0x2000   LINEBUFFER_ID                RO     = 0x4C423031 ("LB01")
//0x2004   FRAMEA_BASE_ADDR_ACC         RW     缓冲A·左半(分屏时加速器画的)
//0x2008   FRAMEA_BASE_ADDR_CPU         RW     缓冲A·右半(分屏时 CPU 画的)
//0x200C   FRAMEB_BASE_ADDR_ACC         RW     缓冲B·左半
//0x2010   FRAMEB_BASE_ADDR_CPU         RW     缓冲B·右半
//0x2014   FRAMEC_FULL_BASE_ADDR_ACC    RW     缓冲C·全屏(加速器独占)
//0x2018   FRAMED_FULL_BASE_ADDR_ACC    RW     缓冲D·全屏
//0x201C   STATUS                       RW     bit0=ENABLE bit1=SPLIT(0=全屏 1=分屏)
//0x2020   FPS_ACC                      RO     加速器帧率(handler 算好送回来)
//0x2024   FPS_CPU                      RO     CPU 帧率(同上)
//0x2028   DONE_CPU                     RW1S   bit0: CPU 这半幅画完, 写1置位
//0x202C   DONE_ACC                     RW1S   bit0: ACC 这半幅画完(加速器也可硬件置位)
//
//  DONE_CPU / DONE_ACC 的寄存器归本模块
//      置位: CPU 写 1(W1S) / 加速器走 acc_frame_done 端口硬件置位
//      清零: handler 在帧边界翻页后发 done_clr 回来(软件不许清)
//      写 SPLIT 的那一拍, 本模块自己把两个 DONE 丢掉(它们属于旧模式)
//
//  帧率数字不在这里算: 计数在 handler 里, 算好的值经 fps_*_val 送回来, 供读回和回显
//==============================================================================
module APB0_lineBuffer #(
    parameter ADDR_WIDTH = 32
)(
    input  wire                         clk,
    input  wire                         rst_n,

    //------------------------------------------------------------
    // APB3 从机侧(接 APB0_Interconnect)
    //------------------------------------------------------------
    input  wire [11:0]                  PADDR,
    input  wire                         PSEL,
    input  wire                         PENABLE,
    input  wire                         PWRITE,
    input  wire [31:0]                  PWDATA,
    output wire [31:0]                  PRDATA,
    output wire                         PREADY,
    output wire                         PSLVERROR,

    //------------------------------------------------------------
    // 功能侧:进(来自 bufferAPBHandler)
    //------------------------------------------------------------
    input  wire                         acc_frame_done,         //(I) 加速器交帧【单拍脉冲】
    input  wire [1:0]                   done_clr,               //(I) {cpu,acc} 翻页消费后清位
    input  wire [31:0]                  fps_acc_val,            //(I) handler 算好的帧率
    input  wire [31:0]                  fps_cpu_val,

    //------------------------------------------------------------
    // 功能侧:出(给 bufferAPBHandler)
    //------------------------------------------------------------
    output wire                         enable,                 //(O) STATUS.ENABLE
    output wire                         split_pending,          //(O) STATUS.SPLIT(待生效值)
    output wire [2*ADDR_WIDTH-1:0]      framea_base_addr,       //(O) {左半(acc), 右半(cpu)}
    output wire [2*ADDR_WIDTH-1:0]      frameb_base_addr,       //(O) {左半(acc), 右半(cpu)}
    output wire [2*ADDR_WIDTH-1:0]      frame_full_base_addr,   //(O) {全屏 C, 全屏 D}
    output wire                         done_acc,
    output wire                         done_cpu,
    output wire [31:0]                  fps_acc                 //(O) 给加速器自己显示用
);

localparam  LINEBUFFER_ID   = 32'h4C423031;    // "LB01"

//------------------------------------------------------------------------------
// wire / reg 定义区
//------------------------------------------------------------------------------
wire                    trans_en;
wire                    write_en;
wire                    read_en;
wire [3:0]              reg_sel;
wire                    addr_hit;
wire                    read_err;
wire                    write_err;
wire                    split_change;

reg  [31:0]             PRDATA_r;
reg  [ADDR_WIDTH-1:0]   framea_acc_r;
reg  [ADDR_WIDTH-1:0]   framea_cpu_r;
reg  [ADDR_WIDTH-1:0]   frameb_acc_r;
reg  [ADDR_WIDTH-1:0]   frameb_cpu_r;
reg  [ADDR_WIDTH-1:0]   framec_full_r;
reg  [ADDR_WIDTH-1:0]   framed_full_r;
reg  [31:0]             status_r;       // {SPLIT_pending, ENABLE}
reg                     done_cpu_r;
reg                     done_acc_r;

// 交帧事件条件
//   两路 DONE 都是"0->1 才算一次事件"(事件同时用于帧率统计):
//     DONE_ACC: 加速器硬件脉冲 acc_frame_done  /  软件 W1S 写 0x202C
//     DONE_CPU: 软件 W1S 写 0x2028
//   ★ 软件 W1S 的 DONE_ACC 曾经漏了 —— 只有硬件那一路,
//     而顶层加速器还没接(acc_frame_done 接 0) → DONE_ACC 永远置不上,
//     表现为: 写 0x202C 后回读还是 0、没有交帧事件、帧率恒 0、翻页不发生。
wire                    acc_evt;        // 加速器交帧事件
wire                    cpu_evt;        // CPU 交帧事件
wire                    acc_wr_set;     // 软件 W1S 写 DONE_ACC

assign  acc_wr_set      = write_en & ~write_err & (reg_sel == 4'd11) & PWDATA[0];
assign  acc_evt         = (acc_frame_done | acc_wr_set) & ~done_acc_r;
assign  cpu_evt         = write_en & ~write_err & (reg_sel == 4'd10) & PWDATA[0] & ~done_cpu_r;

//------------------------------------------------------------------------------
// APB3 总线解析
//------------------------------------------------------------------------------
// APB3 是两相:
//      SETUP  : PSEL=1, PENABLE=0      地址已经有效
//      ACCESS : PSEL=1, PENABLE=1      传输在这里完成
//
// !! 这里和 AHB 有个关键区别 !!
//      AHB 里锁了 HADDR_r 和 xxx_en_r 各一拍,
//      那是因为 AHB 是流水总线:地址相和数据相错开一拍,从机在数据相必须自己记住地址
//
//      APB 不流水,ACCESS 相里 PADDR / PWRITE / PWDATA 全程稳定,所以
//      "锁存的地址"就是 ACCESS 相本身 —— 总线已经替你锁好了
//
//      这里要是照抄再锁一拍,读数据会晚一个周期才出来,而 PREADY=1 在同一个
//      ACCESS 相就结束了传输,SoC 桥采到的是上一拍的旧值 -> 读回全错。
//------------------------------------------------------------------------------
assign  trans_en        = PSEL & PENABLE;
assign  write_en        = trans_en &  PWRITE;
assign  read_en         = trans_en & ~PWRITE;

assign  PREADY          = 1'b1;                     // 零等待,一次传输固定两拍

// 地址译码:本槽 4 KB,只实现前 0x30 字节(12 个寄存器)
assign  reg_sel         = PADDR[5:2];
assign  addr_hit        = (PADDR[11:6] == 6'h00) & (reg_sel <= 4'd11);

// 非法判据:读和写各一条,都只在 ACCESS 相才可能为 1
assign  read_err        = read_en  & ~addr_hit;
assign  write_err       = write_en & ~addr_hit;

assign  PRDATA          = PRDATA_r;
assign  PSLVERROR       = read_err | write_err;

assign  split_change       = write_en & ~write_err & (reg_sel == 4'd7) & (PWDATA[1] != status_r[1]);

//------------------------------------------------------------------------------
// 功能侧输出
//------------------------------------------------------------------------------
assign  enable                    = status_r[0];
assign  split_pending             = status_r[1];
// 打包出去: 高位 = acc 那一半, 低位 = cpu 那一半(和 lineBuffer_addr 的 {段0,段1} 一致)
assign  framea_base_addr      = {framea_acc_r,  framea_cpu_r};
assign  frameb_base_addr      = {frameb_acc_r,  frameb_cpu_r};
assign  frame_full_base_addr  = {framec_full_r, framed_full_r};
assign  done_acc                  = done_acc_r;
assign  done_cpu                  = done_cpu_r;
assign  fps_acc                   = fps_acc_val;    // 格子在这里, 值由 handler 算

//------------------------------------------------------------------------------
// 写寄存器 + DONE 的置位/清零
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        framea_acc_r    <= {ADDR_WIDTH{1'b0}};
        framea_cpu_r    <= {ADDR_WIDTH{1'b0}};
        frameb_acc_r    <= {ADDR_WIDTH{1'b0}};
        frameb_cpu_r    <= {ADDR_WIDTH{1'b0}};
        framec_full_r   <= {ADDR_WIDTH{1'b0}};
        framed_full_r   <= {ADDR_WIDTH{1'b0}};
        status_r        <= 32'd0;
        done_cpu_r      <= 1'b0;
        done_acc_r      <= 1'b0;
    end
    else begin
        // ---- DONE:交帧事件置位 ----
        if (acc_evt) done_acc_r <= 1'b1;
        if (cpu_evt) done_cpu_r <= 1'b1;

        // ---- DONE:handler 翻页消费后清位 ----
        if (done_clr[0]) done_acc_r <= 1'b0;
        if (done_clr[1]) done_cpu_r <= 1'b0;

        // ---- APB 写 ----
        if (write_en && !write_err) begin
            case (reg_sel)
                // 基址:必须 16 字节对齐(AXI 起始地址按传输大小对齐)。
                //       不对齐的写【直接丢掉】, 读回还是旧值 —— 软件据此能发现写错了
                4'd1    : if (PWDATA[3:0] == 4'h0) framea_acc_r  <= PWDATA;
                4'd2    : if (PWDATA[3:0] == 4'h0) framea_cpu_r  <= PWDATA;
                4'd3    : if (PWDATA[3:0] == 4'h0) frameb_acc_r  <= PWDATA;
                4'd4    : if (PWDATA[3:0] == 4'h0) frameb_cpu_r  <= PWDATA;
                4'd5    : if (PWDATA[3:0] == 4'h0) framec_full_r <= PWDATA;
                4'd6    : if (PWDATA[3:0] == 4'h0) framed_full_r <= PWDATA;
                4'd7    : status_r <= {30'd0, PWDATA[1:0]};     // ENABLE / SPLIT
                default : ;
            endcase
        end

        // ---- 切模式:旧模式的 DONE 丢掉(留着会在帧边界拿它去翻新模式的索引) ----
        if (split_change) begin
            done_acc_r <= 1'b0;
            done_cpu_r <= 1'b0;
        end
    end
end

//------------------------------------------------------------------------------
// 读寄存器(组合)
//      读是组合的 —— ACCESS 相里 PRDATA 必须当拍有效, SoC 桥在同一个边沿就采走
//------------------------------------------------------------------------------
always @(*) begin
    PRDATA_r = 32'd0;
    if (read_en && !read_err) begin
        case (reg_sel)
            4'd0    : PRDATA_r = LINEBUFFER_ID;                     // 0x2000 ID
            4'd1    : PRDATA_r = framea_acc_r;                      // 0x2004
            4'd2    : PRDATA_r = framea_cpu_r;                      // 0x2008
            4'd3    : PRDATA_r = frameb_acc_r;                      // 0x200C
            4'd4    : PRDATA_r = frameb_cpu_r;                      // 0x2010
            4'd5    : PRDATA_r = framec_full_r;                     // 0x2014
            4'd6    : PRDATA_r = framed_full_r;                     // 0x2018
            4'd7    : PRDATA_r = {30'd0, status_r[1:0]};                    // 0x201C(回读你写进去的值)
            4'd8    : PRDATA_r = fps_acc_val;                       // 0x2020
            4'd9    : PRDATA_r = fps_cpu_val;                       // 0x2024
            4'd10   : PRDATA_r = {31'd0, done_cpu_r};               // 0x2028 DONE_CPU
            4'd11   : PRDATA_r = {31'd0, done_acc_r};               // 0x202C DONE_ACC
            default : PRDATA_r = 32'h0;
        endcase
    end
    else
        PRDATA_r = 32'd0;
end

endmodule
