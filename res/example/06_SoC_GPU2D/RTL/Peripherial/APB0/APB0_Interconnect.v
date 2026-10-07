//==============================================================================
//  APB0_Interconnect.v —— 用户 APB0 窗口的外设互连
//------------------------------------------------------------------------------
//  功能
//      把一次 APB 传输交给被选中的那个外设总线层，再把回程选回给 SoC。
//
//  结构
//      APB0_Decoder              地址 → 片选（PSEL_key / PSEL_gpu2d）
//      APB0_key      (槽 0)      按键总线层，在本模块内例化
//      APB0_gpu2d    (槽 2)      GPU_2D 总线层，在本模块内例化
//                                 槽内：0x2000 观测 / 0x2040 显示（PADDR[6] 分）
//      GPU_2D 的功能层在顶层，通过本模块的功能侧端口连过去
//
//  连接
//      SoC APB ─▶ 译码 ─▶ 被选中的外设 ─▶ 回程三选一
//      没被任何外设选中的访问（比如空掉的槽 1）回 PSLVERROR=1
//
//  槽位分配见 APB0_Decoder.v
//==============================================================================

module APB0_Interconnect #(
    parameter   KEY_NUM         = 4             // 按键个数(透传给 APB0_key)
)(
    input  wire         clk,
    input  wire         rst_n,

    //------------------------------------------------------------
    // 面向 SoC 的 APB3 从机口(io_apbSlave_0_*)
    //------------------------------------------------------------
    input  wire [15:0]  APB_0_PADDR,
    input  wire         APB_0_PSEL,
    input  wire         APB_0_PENABLE,
    input  wire         APB_0_PWRITE,
    input  wire [31:0]  APB_0_PWDATA,
    output wire [31:0]  APB_0_PRDATA,
    output wire         APB_0_PREADY,
    output wire         APB_0_PSLVERROR,

    //------------------------------------------------------------
    // 外设 0:按键 KEY(槽 0,0x0000 - 0x0FFF)
    //------------------------------------------------------------
    input  wire [3:0]   key_state,      //(I) 来自顶层 key:消抖后的按键状态,按下 = 1
    input  wire [3:0]   key_edge,       //(I) 来自顶层 key:按下沿,单周期脉冲
    output wire         IRQ_frame,      //(O) 显示帧起始中断【专用线, 电平型】-> SoC userInterruptA

    //------------------------------------------------------------
    // 外设 1: GPU_2D(槽 2, 0x2000 - 0x2FFF, 与 AXI 从口 S2/S3 同号)
    //   总线层(APB0_gpu2d)在【本模块内】例化;
    //   功能层(加速器 + 显示通道 + Handler)在【顶层】例化,
    //   通过下面这两组功能侧端口连过去(名字就和 APB0_gpu2d 的端口一致)
    //------------------------------------------------------------
    // 显示子块(0x2040): 从功能侧进来
    input  wire         acc_frame_done,         //(I) 加速器:FENCE 完成 = 这半幅画完了【单拍】
    input  wire [1:0]   done_clr,               //(I) {cpu,acc} 翻页消费后清位
    input  wire [31:0]  fps_acc_val,            //(I) Handler 算好的帧率
    input  wire [31:0]  fps_cpu_val,

    // 观测子块(0x2000): 从功能侧进来(全只读)
    input  wire [31:0]  busy_val,               //(I) STATUS.BUSY
    input  wire [31:0]  cmd_done_val,           //(I) 已完成命令数
    input  wire [31:0]  frame_done_val,         //(I) 已完成帧数
    input  wire [31:0]  wr_beats_val,           //(I) 已写出的 AXI 拍数
    input  wire [31:0]  rd_beats_val,           //(I) 已读回的 AXI 拍数
    input  wire [31:0]  busy_cyc_val,           //(I) 忙的周期数

    // 显示子块(0x2040): 出去给功能侧
    output wire         enable,                 //(O) STATUS.ENABLE
    output wire         split_pending,          //(O) STATUS.SPLIT(待生效值)
    output wire [63:0]  framea_base_addr,       //(O) {A左半(acc), A右半(cpu)}
    output wire [63:0]  frameb_base_addr,       //(O) {B左半(acc), B右半(cpu)}
    output wire [63:0]  frame_full_base_addr,   //(O) {全屏 C, 全屏 D}
    output wire         done_gpu2d,             //(O) 加速器那半有没有新帧等着显示
    output wire         done_cpu,
    output wire [23:0]  ckey_color              //(O) 颜色键控(ck)的键色(显示子块 0x2078, 默认白)
                                                //    功能层拿它去和源像素比: 相等 = 不写
);

//------------------------------------------------------------
// wire inst
//------------------------------------------------------------
// key(槽 0)
wire [11:0]             PADDR_key;
wire                    PSEL_key;
wire                    PENABLE_key;
wire                    PWRITE_key;
wire [31:0]             PWDATA_key;
wire [31:0]             PRDATA_key;
wire                    PREADY_key;
wire                    PSLVERROR_key;

// GPU_2D(槽 2): 片选 + 总线三根回程
wire [11:0]             PADDR_gpu2d;
wire                    PSEL_gpu2d;
wire                    PENABLE_gpu2d;
wire                    PWRITE_gpu2d;
wire [31:0]             PWDATA_gpu2d;
wire [31:0]             PRDATA_gpu2d;
wire                    PREADY_gpu2d;
wire                    PSLVERROR_gpu2d;

//------------------------------------------------------------------------------
// 地址译码
//------------------------------------------------------------------------------

APB0_Decoder u_APB0_Decoder (
    .APB_0_PADDR                (APB_0_PADDR),
    .APB_0_PSEL                 (APB_0_PSEL),
    .PSEL_key                   (PSEL_key),
    .PSEL_gpu2d                 (PSEL_gpu2d)
);

//------------------------------------------------------------------------------
// 前向路径
//   PADDR/PWRITE/PWDATA 广播路由(APB 单主,不会有冲突)
//   PSEL 用片选圈定 —— 没被选中的外设完全看不到这次访问
//   PENABLE 原样透传,让从机看到完整的 SETUP -> ACCESS
//------------------------------------------------------------------------------
assign PADDR_key            =   APB_0_PADDR[11:0];       // 槽内偏移,12 位够 4 KB
assign PENABLE_key          =   APB_0_PENABLE;
assign PWRITE_key           =   APB_0_PWRITE;
assign PWDATA_key           =   APB_0_PWDATA;

//   槽 2(GPU_2D): 槽内偏移 12 位够 4 KB
assign PADDR_gpu2d          =   APB_0_PADDR[11:0];
assign PENABLE_gpu2d        =   APB_0_PENABLE;
assign PWRITE_gpu2d         =   APB_0_PWRITE;
assign PWDATA_gpu2d         =   APB_0_PWDATA;

//------------------------------------------------------------------------------
// 返回路径
//   每加一个槽就在上面加一个分支;最后一个 else 是兜底,必须有
//------------------------------------------------------------------------------

assign APB_0_PRDATA         =   PSEL_key                                ? PRDATA_key    :
                                PSEL_gpu2d                              ? PRDATA_gpu2d  :
                                32'h0;

assign APB_0_PREADY         =   PSEL_key                                ? PREADY_key    :
                                PSEL_gpu2d                              ? PREADY_gpu2d  :
                                1'b1;

assign APB_0_PSLVERROR      =   PSEL_key                                ? PSLVERROR_key :
                                PSEL_gpu2d                              ? PSLVERROR_gpu2d :
                                1'b1;

//------------------------------------------------------------------------------
// 外设总线层例化
//------------------------------------------------------------------------------

APB0_key #(
    .KEY_NUM                    (KEY_NUM)
) u_APB0_key (
    .clk                        (clk),
    .rst_n                      (rst_n),
    .PADDR                      (PADDR_key),
    .PSEL                       (PSEL_key),
    .PENABLE                    (PENABLE_key),
    .PWRITE                     (PWRITE_key),
    .PWDATA                     (PWDATA_key),
    .PRDATA                     (PRDATA_key),
    .PREADY                     (PREADY_key),
    .PSLVERROR                  (PSLVERROR_key),

    .key_state                  (key_state),
    .key_edge                   (key_edge)
    // 按键中断(key_irq)没人用: 软件是轮询按键的, 这里不接。
    // 顶层那条 userInterruptA 现在专用于【显示帧起始中断 IRQ_frame】。
);

//------------------------------------------------------------------------------
// GPU_2D(槽 2)总线层: 一整块
//   槽内两个子块都由它按 PADDR[6] 分: 0x2000 观测 / 0x2040 显示
//   功能侧端口原样引到本模块端口, 顶层连给 GPU_2D
//   ★ 复位用 rst_n(= sys_rst): 寄存器可以在 DDR 校准完成前就被 CPU 写
//------------------------------------------------------------------------------
APB0_gpu2d u_APB0_gpu2d (
    .clk                        (clk),
    .rst_n                      (rst_n),
    .PADDR                      (PADDR_gpu2d),
    .PSEL                       (PSEL_gpu2d),
    .PENABLE                    (PENABLE_gpu2d),
    .PWRITE                     (PWRITE_gpu2d),
    .PWDATA                     (PWDATA_gpu2d),
    .PRDATA                     (PRDATA_gpu2d),
    .PREADY                     (PREADY_gpu2d),
    .PSLVERROR                  (PSLVERROR_gpu2d),

    // 显示子块(0x2040)
    .acc_frame_done             (acc_frame_done),
    .done_clr                   (done_clr),
    .fps_acc_val                (fps_acc_val),
    .fps_cpu_val                (fps_cpu_val),
    .IRQ_frame_o                (IRQ_frame),        // 帧起始中断(电平, 软件写 0x2070 清)
    .enable                     (enable),
    .split_pending              (split_pending),
    .framea_base_addr           (framea_base_addr),
    .frameb_base_addr           (frameb_base_addr),
    .frame_full_base_addr       (frame_full_base_addr),
    .done_gpu2d                   (done_gpu2d),
    .done_cpu                   (done_cpu),
    .ckey_color                 (ckey_color),       // 键色 -> 顶层 -> GPU_2D 的 func_colorKey

    // 观测子块(0x2000, 全只读)
    .busy_val                   (busy_val),
    .cmd_done_val               (cmd_done_val),
    .frame_done_val             (frame_done_val),
    .wr_beats_val               (wr_beats_val),
    .rd_beats_val               (rd_beats_val),
    .busy_cyc_val               (busy_cyc_val)
);

endmodule
