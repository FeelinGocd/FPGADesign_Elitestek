//==============================================================================
//  APB0_key.v  ——  按键外设的总线解析层
//------------------------------------------------------------------------------
//
//  寄存器表(槽内偏移):
//      0x00  KEY_DATA     RO     消抖后的按键状态,bit[3:0],按下 = 1
//      0x04  KEY_IRQ_EN   RW     按下沿中断使能,bit[3:0]
//      0x08  KEY_ID       RO     = 0x4B455930 ("KEY0"),用来确认外设活着
//
//  地址:APB0 窗口基址 0xf8100000 + 槽 0 偏移 0x000
//        KEY_DATA   = 0xf8100000
//        KEY_IRQ_EN = 0xf8100004
//        KEY_ID     = 0xf8100008
//
//  分层:
//        本文件 = 总线解析层,只做 APB 时序解析 + 寄存器
//        消抖逻辑在顶层例化的 key 模块里,key_state / key_edge 由它提供
//
//  中断:key_irq = |(key_edge & irq_en)，按下沿脉冲
//        ★ 目前【没接】: APB0_Interconnect 不引出它(软件轮询按键)；
//          SoC 的 userInterruptA(PLIC 16) 已专供显示帧边界 IRQ_frame
//        外设侧不设"粘住"的标志寄存器
//
//  一点差异要知道 
//        NVIC 的 pending 触发器一直在采样 IRQ 线,ISR 执行期间来的脉冲也
//        会记下;而我们的 PLIC gateway 在 "线拉高 -> 软件 complete" 这段
//        时间里是停止采样的(SoC_RISCV32.v L4241 / L6416-6419),
//        所以 ISR 执行期间按下的键会丢
//        规避办法就是常规做法:ISR 尽量短,别在里面延时
//==============================================================================

module APB0_key #(
    parameter KEY_NUM      = 4                  // 按键个数
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
    // 功能侧(接顶层例化的 key 模块)
    //   本模块只做总线解析 + 寄存器,消抖逻辑不在这里
    //------------------------------------------------------------
    input  wire [KEY_NUM-1:0]   key_state,      //(I) 消抖后的按键状态,按下 = 1
    input  wire [KEY_NUM-1:0]   key_edge,       //(I) 按下沿,单周期脉冲

    //------------------------------------------------------------
    // 中断输出 -> SoC userInterruptA
    //------------------------------------------------------------
    output wire                 key_irq
);

localparam [31:0]   KEY_ID = 32'h4B455930;      // "KEY0"

//------------------------------------------------------------------------------
// wire / reg 定义区
//------------------------------------------------------------------------------
// 总线解析
wire                trans_en;       // cpu 是否在访问本外设(ACCESS 相)
wire                write_en;       // 统一写使能,单周期
wire                read_en;        // 统一读使能,单周期

// 地址译码
wire [1:0]          reg_sel;        // 寄存器序号
wire                addr_hit;       // 地址命中(在本外设已实现的范围内)
wire                read_err;       // 读非法:在读,但地址没命中
wire                write_err;      // 写非法:在写,但地址没命中

// 寄存器
reg  [31:0]         PRDATA_r;       // 读回数据
reg  [KEY_NUM-1:0]  irq_en;         // 0x04  KEY_IRQ_EN

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
//      APB 不流水 ACCESS 相里 PADDR / PWRITE / PWDATA 全程稳定,所以
//      "锁存的地址"就是 ACCESS 相本身 —— 总线已经替你锁好了
//
//      这里要是照抄再锁一拍,读数据会晚一个周期才出来,而 PREADY=1 在同一个
//      ACCESS 相就结束了传输,SoC 桥采到的是上一拍的旧值 -> 读回全错。
//------------------------------------------------------------------------------
assign trans_en         =   PSEL     & PENABLE;
assign write_en         =   trans_en &  PWRITE;
assign read_en          =   trans_en & ~PWRITE;

assign PREADY       =   1'b1;             // 零等待,一次传输固定两拍

//------------------------------------------------------------------------------
// 地址译码与合法判断
//      addr_hit 管理访存是否命中
//      地址写错要当场报错,而不是悄悄改到别的寄存器
//------------------------------------------------------------------------------
assign reg_sel          =   PADDR[3:2];

assign addr_hit         =   (PADDR[11:4] == 8'h00)      // 落在前 16 字节内
                            & (reg_sel != 2'd3);            // 且不是保留的 0x0C

// 非法判据:读和写各有一条。两条都只在 ACCESS 相(read_en/write_en 为 1)才可能为 1,所以空闲和 SETUP 相不会误报。
assign read_err         = read_en  & ~addr_hit;
assign write_err        = write_en & ~addr_hit;
assign PRDATA           = PRDATA_r;
assign PSLVERROR        = read_err | write_err;

//------------------------------------------------------------------------------
// 中断:按下沿脉冲,使能后直出
//------------------------------------------------------------------------------
assign key_irq = |(key_edge & irq_en);

//------------------------------------------------------------------------------
// 读地址
//      读是组合的 —— ACCESS 相里 PRDATA 必须当拍有效,SoC 桥在同一个边沿就采走,所以这里不能打拍
//------------------------------------------------------------------------------

always @(*) begin
    PRDATA_r = 32'h0;
    if (read_en && !read_err) begin
        case (reg_sel)
            2'd0    : PRDATA_r = key_state;     // 0x00  KEY_DATA
            2'd1    : PRDATA_r = irq_en;        // 0x04  KEY_IRQ_EN
            2'd2    : PRDATA_r = KEY_ID;        // 0x08  KEY_ID
            default : PRDATA_r = 32'd0;
        endcase
    end
    else
        PRDATA_r = 32'd0;
end

//------------------------------------------------------------------------------
// 写地址
//------------------------------------------------------------------------------

always @(posedge clk or negedge rst_n) begin
    if (!rst_n)
        irq_en <= {KEY_NUM{1'b0}};
    else begin
        if (write_en && !write_err) begin
            case (reg_sel)
                2'd1    : irq_en <= PWDATA[KEY_NUM-1:0];    // 0x04  KEY_IRQ_EN
                default : ;
            endcase
        end
    end
end

endmodule
