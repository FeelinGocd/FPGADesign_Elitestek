//==============================================================================
//  APB0_Interconnect.v  ——  APB0 口的外设互连
//------------------------------------------------------------------------------
//  三件事:
//    1) 例化 APB0_Decoder,把地址翻成片选
//    2) 前向分发:把 PSEL/PENABLE/PWRITE/PADDR/PWDATA 送到被选中的外设
//       (用 PSEL 圈定,所以从机看到的还是标准的 SETUP -> ACCESS 两相)
//    3) 回程选择:把被选中外设的 PRDATA/PREADY/PSLVERROR 送回 SoC
//
//  注意:APB 是单主(只有 CPU),所以这里没有仲裁,也不需要像 AHB 那样寄存
//        片选。APB 的地址在整个 ACCESS 相里都是稳的,回程可以直接组合选。
//
//  !! 回程的兜底非常关键 !!  PREADY 必须始终有值,否则 SoC 桥会永远卡在
//     ACCESS 态,CPU 总线死锁。下面每个选择都带了默认值。
//==============================================================================

module APB0_Interconnect (
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
    input  wire [3:0]   key_pin,      // 板级按键,按下 = 0
    output wire         key_irq       // -> SoC userInterruptA
);

//------------------------------------------------------------
// wire inst
//------------------------------------------------------------
// key
wire [11:0]             PADDR_key;
wire                    PSEL_key;
wire                    PENABLE_key;
wire                    PWRITE_key;
wire [31:0]             PWDATA_key;
wire [31:0]             PRDATA_key;
wire                    PREADY_key;
wire                    PSLVERROR_key;

//------------------------------------------------------------------------------
// 地址译码
//------------------------------------------------------------------------------

APB0_Decoder u_APB0_Decoder (
    .APB_0_PADDR                (APB_0_PADDR),
    .APB_0_PSEL                 (APB_0_PSEL),
    .PSEL_key                   (PSEL_key)
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

//------------------------------------------------------------------------------
// 返回路径
//------------------------------------------------------------------------------

assign APB_0_PRDATA         =   PSEL_key ? PRDATA_key :
                                32'h0;

assign APB_0_PREADY         =   PSEL_key ? PREADY_key :
                                1'b1;

assign APB_0_PSLVERROR      =   PSEL_key ? PSLVERROR_key :
                                1'b1;

APB0_key #(
    .KEY_NUM                    (4),
    .DEBOUNCE_CNT               (1_000_000)
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

    .key_pin                    (key_pin),
    .key_irq                    (key_irq)
);

endmodule
