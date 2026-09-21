//==============================================================================
//  APB0_DDR_checker.v  ——  DDR 自检的总线解析层
//------------------------------------------------------------------------------
//
//  寄存器表(槽内偏移):
//      0x00  CTRL       RW   bit0 = start,写 1 启动自检,硬件自动清
//      0x04  STATUS     RO   bit0=busy  bit1=done  bit2=pass  bit[15:8]=errcode
//      0x08  ERR_ADDR   RO   第一个不一致的地址
//      0x0C  ERR_EXP    RO   期望值(低 32 位)
//      0x10  ERR_ACT    RO   实际值(低 32 位)
//      0x14  TEST_ID    RO   = 0x43484B30 ("CHK0"),用来确认外设活着
//
//  地址:APB0 窗口基址 0xf8100000 + 槽 1 偏移 0x1000
//        CTRL     = 0xf8101000
//        STATUS   = 0xf8101004
//        ERR_ADDR = 0xf8101008
//        ERR_EXP  = 0xf810100C
//        ERR_ACT  = 0xf8101010
//        TEST_ID  = 0xf8101014
//
//  errcode(来自 DDR_checker):
//      0 = 无错误        1 = 读回不一致
//      2 = 响应非 OKAY   3 = 超时
//      4 = 图案未加载
//
//  分层:本文件 = 总线解析层,只做"APB 时序 + 寄存器"
//        自检逻辑全在 DDR_checker 里(在顶层例化,自带 AXI4_Master)
//        两者之间的连线:checker_run / checker_busy / ... 共 8 根
//==============================================================================

module APB0_DDR_checker #(
    parameter ADDR_WIDTH = 32
)(
    input  wire                 clk,
    input  wire                 rst_n,

    //------------------------------------------------------------
    // APB3 从机侧(接 APB0_Interconnect)
    //------------------------------------------------------------
    input  wire [11:0]          PADDR,
    input  wire                 PSEL,
    input  wire                 PENABLE,
    input  wire                 PWRITE,
    input  wire [31:0]          PWDATA,
    output wire [31:0]          PRDATA,
    output wire                 PREADY,
    output wire                 PSLVERROR,

    //------------------------------------------------------------
    // 功能侧(接 DDR_Checker)
    //------------------------------------------------------------
    output wire                 checker_run,        // 启动脉冲,单周期
    input  wire                 checker_busy,
    input  wire                 checker_done,
    input  wire                 checker_pass,
    input  wire [7:0]           checker_errcode,
    input  wire [ADDR_WIDTH-1:0] checker_err_addr,
    input  wire [31:0]          checker_err_exp,
    input  wire [31:0]          checker_err_act
);

localparam [31:0]   TEST_ID = 32'h43484B30;     // "CHK0"

//------------------------------------------------------------------------------
// wire / reg 定义区
//------------------------------------------------------------------------------
// 总线解析
wire                trans_en;       // cpu 是否在访问本外设(ACCESS 相)
wire                write_en;       // 统一写使能,单周期
wire                read_en;        // 统一读使能,单周期

// 地址译码
wire [2:0]          reg_sel;        // 寄存器序号
wire                addr_hit;       // 地址命中(在本外设已实现的范围内)
wire                read_err;       // 读非法:在读,但地址没命中
wire                write_err;      // 写非法:在写,但地址没命中

// 寄存器
reg  [31:0]         PRDATA_r;       // 读回数据
reg                 ctrl_start_r;   // 0x00  CTRL.start

//------------------------------------------------------------------------------
// APB3 总线解析
//------------------------------------------------------------------------------
// APB3 是两相:
//      SETUP  : PSEL=1, PENABLE=0      地址已经有效
//      ACCESS : PSEL=1, PENABLE=1      传输在这里完成
//
// APB 不流水,ACCESS 相里 PADDR / PWRITE / PWDATA 全程稳定,所以
// "锁存的地址"就是 ACCESS 相本身 —— 总线已经替你锁好了。
// 这里要是照抄 AHB 再锁一拍,读数据会晚一个周期出来,
// 而 PREADY=1 在同一个 ACCESS 相就结束了传输 -> 读回全错。
//------------------------------------------------------------------------------
assign trans_en         =   PSEL     & PENABLE;
assign write_en         =   trans_en &  PWRITE;
assign read_en          =   trans_en & ~PWRITE;

assign PREADY       =   1'b1;             // 零等待,一次传输固定两拍

//------------------------------------------------------------------------------
// 地址译码与合法判断
//      本槽 4 KB,只实现前 32 字节(8 个寄存器槽)
//------------------------------------------------------------------------------
assign reg_sel          =   PADDR[4:2];                     // 8 个寄存器槽

assign addr_hit         =   (PADDR[11:5] == 7'h00)          // 落在前 32 字节内
                            & (reg_sel <= 3'd5);            // 且不是保留的 0x18/0x1C

// 非法判据:读和写各有一条。两条都只在 ACCESS 相(read_en/write_en 为 1)才可能为 1,所以空闲和 SETUP 相不会误报。
assign read_err         = read_en  & ~addr_hit;
assign write_err        = write_en & ~addr_hit;
assign PRDATA           = PRDATA_r;
assign PSLVERROR        = read_err | write_err;

//------------------------------------------------------------------------------
// 启动脉冲 -> 功能模块
//------------------------------------------------------------------------------
assign checker_run = ctrl_start_r;

//------------------------------------------------------------------------------
// 读寄存器
//      读是组合的 —— ACCESS 相里 PRDATA 必须当拍有效,SoC 桥在同一个边沿就采走,所以这里不能打拍
//------------------------------------------------------------------------------
always @(*) begin
    PRDATA_r = 32'h0;
    if (read_en && !read_err) begin
        case (reg_sel)
            3'd0    : PRDATA_r = {31'd0, ctrl_start_r};                         // 0x00 CTRL
            3'd1    : PRDATA_r = {16'd0, checker_errcode, 5'd0,
                                  checker_pass, checker_done, checker_busy};    // 0x04 STATUS
            3'd2    : PRDATA_r = checker_err_addr;                              // 0x08 ERR_ADDR
            3'd3    : PRDATA_r = checker_err_exp;                               // 0x0C ERR_EXP
            3'd4    : PRDATA_r = checker_err_act;                               // 0x10 ERR_ACT
            3'd5    : PRDATA_r = TEST_ID;                                       // 0x14 TEST_ID
            default : PRDATA_r = 32'd0;
        endcase
    end
    else
        PRDATA_r = 32'h0;
end

//------------------------------------------------------------------------------
// 写寄存器
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n)
        ctrl_start_r <= 1'b0;
    else begin
        ctrl_start_r <= 1'b0;                   // 默认:start 是单周期脉冲
        if (write_en && !write_err) begin
            case (reg_sel)
                3'd0    : ctrl_start_r <= PWDATA[0];    // 0x00 CTRL.start
                default : ;
            endcase
        end
    end
end

endmodule
