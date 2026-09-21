//==============================================================================
//  APB0_Interconnect.v  ——  APB0 口的外设互连
//------------------------------------------------------------------------------
//  四件事:
//    1) 例化 APB0_Decoder,把窗口内地址翻成片选(PSEL_xxx)
//    2) 例化各外设的"总线解析层"(APB0_key 等),由它们做 APB 时序解析 + 寄存器
//    3) 前向分发:PADDR/PENABLE/PWRITE/PWDATA 广播给被选中的外设
//    4) 回程选择:把被选中外设的 PRDATA/PREADY/PSLVERROR 送回 SoC
//
//  分层约定(重要):
//    - 本模块只管"总线 + 寄存器",不含任何功能逻辑
//    - 外设的**功能层**在顶层例化(按键消抖 key、DDR 自检 DDR_checker 等),
//      通过本模块暴露的功能侧端口连过去
//    - 这样做的原因:功能层往往带物理引脚或 AXI 主口,集中放在顶层
//      便于引脚约束、调试和整块替换
//
//  注意:APB 是单主(只有 CPU),所以这里没有仲裁,也不需要像 AHB 那样寄存
//        片选。APB 的地址在整个 ACCESS 相里都是稳的,回程可以直接组合选。
//
//  !! 回程的兜底非常关键 !!  PREADY 必须始终有值,否则 SoC 桥会永远卡在
//     ACCESS 态,CPU 总线死锁。下面每个选择都带了默认值。
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
    output wire         key_irq,        //(O) -> SoC userInterruptA

    //------------------------------------------------------------
    // 槽 1 空闲: DDR 自检 CHECKER 已整块摘除(源文件保留, 不再接入)
    //------------------------------------------------------------

    //------------------------------------------------------------
    // 外设 2: lineBuffer(槽 2)
    //   总线层(APB0_lineBuffer)在本模块内例化;
    //   功能层(bufferAPBHandler / bufferControler)在【顶层】例化,
    //   通过下面这两组功能侧端口连过去(名字就和 APB0_lineBuffer 的端口一致)
    //------------------------------------------------------------
    // 从功能侧进来
    input  wire         acc_frame_done,             //(I) 加速器:这半幅画完了【单拍脉冲】
    input  wire [1:0]   done_clr,                   //(I) {cpu,acc} 翻页消费后清位
    input  wire [31:0]  fps_acc_val,                //(I) handler 算好的帧率
    input  wire [31:0]  fps_cpu_val,

    // 出去给功能侧
    output wire         enable,                     //(O) STATUS.ENABLE
    output wire         split_pending,              //(O) STATUS.SPLIT(待生效值)
    output wire [63:0]  framea_base_addr,           //(O) {A左半(acc), A右半(cpu)}
    output wire [63:0]  frameb_base_addr,           //(O) {B左半(acc), B右半(cpu)}
    output wire [63:0]  frame_full_base_addr,       //(O) {全屏 C, 全屏 D}
    output wire         done_acc,                   //(O) 有没有新帧等着显示
    output wire         done_cpu,
    output wire [31:0]  fps_acc                     //(O) 加速器帧率(给加速器自己显示用)
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

// lineBuffer(槽 2)
wire [11:0]             PADDR_lineBuffer;
wire                    PSEL_lineBuffer;
wire                    PENABLE_lineBuffer;
wire                    PWRITE_lineBuffer;
wire [31:0]             PWDATA_lineBuffer;
wire [31:0]             PRDATA_lineBuffer;
wire                    PREADY_lineBuffer;
wire                    PSLVERROR_lineBuffer;

//------------------------------------------------------------------------------
// 地址译码
//------------------------------------------------------------------------------

APB0_Decoder u_APB0_Decoder (
    .APB_0_PADDR                (APB_0_PADDR),
    .APB_0_PSEL                 (APB_0_PSEL),
    .PSEL_key                   (PSEL_key),
    .PSEL_lineBuffer            (PSEL_lineBuffer)
);

//------------------------------------------------------------------------------
// 前向路径(以 key 为例)
//   PADDR/PWRITE/PWDATA 广播路由(APB 单主,不会有冲突)
//   PSEL 用片选圈定 —— 没被选中的外设完全看不到这次访问
//   PENABLE 原样透传,让从机看到完整的 SETUP -> ACCESS
//------------------------------------------------------------------------------

assign PADDR_key            =   APB_0_PADDR[11:0];       // 槽内偏移,12 位够 4 KB
assign PENABLE_key          =   APB_0_PENABLE;
assign PWRITE_key           =   APB_0_PWRITE;
assign PWDATA_key           =   APB_0_PWDATA;

assign PADDR_lineBuffer     =   APB_0_PADDR[11:0];
assign PENABLE_lineBuffer   =   APB_0_PENABLE;
assign PWRITE_lineBuffer    =   APB_0_PWRITE;
assign PWDATA_lineBuffer    =   APB_0_PWDATA;

//------------------------------------------------------------------------------
// 返回路径
//   每加一个槽就在上面加一个分支;最后一个 else 是兜底,必须有
//------------------------------------------------------------------------------

assign APB_0_PRDATA         =   PSEL_key     ? PRDATA_key     :
                                PSEL_lineBuffer ? PRDATA_lineBuffer :
                                32'h0;

assign APB_0_PREADY         =   PSEL_key     ? PREADY_key     :
                                PSEL_lineBuffer ? PREADY_lineBuffer :
                                1'b1;

assign APB_0_PSLVERROR      =   PSEL_key     ? PSLVERROR_key     :
                                PSEL_lineBuffer ? PSLVERROR_lineBuffer :
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
    .key_edge                   (key_edge),
    .key_irq                    (key_irq)
);

// 槽 1: 原来这里例化 APB0_DDR_checker, 现已摘除(源文件保留)
//       —— 对应地, Axi_Mux 的 S1 也不再接 checker, 让给加速器

APB0_lineBuffer # (
    .ADDR_WIDTH                 (32)
  ) u_APB0_lineBuffer (
    .clk                        (clk),
    .rst_n                      (rst_n),
    .PADDR                      (PADDR_lineBuffer),
    .PSEL                       (PSEL_lineBuffer),
    .PENABLE                    (PENABLE_lineBuffer),
    .PWRITE                     (PWRITE_lineBuffer),
    .PWDATA                     (PWDATA_lineBuffer),
    .PRDATA                     (PRDATA_lineBuffer),
    .PREADY                     (PREADY_lineBuffer),
    .PSLVERROR                  (PSLVERROR_lineBuffer),

    // 功能侧: 进(来自顶层)
    .acc_frame_done             (acc_frame_done),
    .done_clr                   (done_clr),
    .fps_acc_val                (fps_acc_val),
    .fps_cpu_val                (fps_cpu_val),

    // 功能侧: 出(接到顶层)
    .enable                     (enable),
    .split_pending              (split_pending),
    .framea_base_addr           (framea_base_addr),
    .frameb_base_addr           (frameb_base_addr),
    .frame_full_base_addr       (frame_full_base_addr),
    .done_acc                   (done_acc),
    .done_cpu                   (done_cpu),
    .fps_acc                    (fps_acc)
  );

//   bufferAPBHandler(功能层)不在这里例化 —— 它在顶层, 通过上面那两组功能侧端口连过去

endmodule
