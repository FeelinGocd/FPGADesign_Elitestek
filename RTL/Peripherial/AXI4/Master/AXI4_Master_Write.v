//////////////////////////////////////////////////////////////////////////////
// AXI4_Master_Write —— AXI4 写通路
//   一笔写事务 = 1×AW + N×W + 1×B。
//   先发 AW，握手后再从 wr_* 逐拍取数发 W（WLAST 由 cmd_len 生成），
//   收到 B 即整笔结束。
//
// 接口组：
//   clk / rst_n     时钟、复位（顶层已过滤系统门控）
//   cmd_*           命令接口：提交一笔写（去哪 / 多长）
//   wr_*            写数据接口：从数据源拉数
//   m_AXI_AW_*      写地址通道
//   m_AXI_W_*       写数据通道
//   m_AXI_B_*       写响应通道
//
// 约束：
//   1. cmd_valid 拉高后 cmd_addr / cmd_len 必须稳定，直到 cmd_ready
//   2. wr_valid 不得由 wr_ready 组合产生，且 payload 稳定到握手
//   cmd_err 恒为 0（Axi_Mux 不透传 BRESP，写错误靠读回比对发现）
//
// 参考：docs/参考/AXI总线解析.md
//////////////////////////////////////////////////////////////////////////////
`timescale 1ns/100ps

module AXI4_Master_Write #(
    parameter ADDR_WIDTH = 32,
    parameter DATA_WIDTH = 128,
    parameter STRB_WIDTH = DATA_WIDTH/8,
    parameter ID_WIDTH   = 8,
    parameter LEN_WIDTH  = 8,
    parameter AXI_ID     = {ID_WIDTH{1'b0}}
)(
    //==================== 时钟 / 复位（顶层已过滤）====================
    input  wire                     clk,
    input  wire                     rst_n,

    //==================== 命令接口 ====================
    input  wire                     cmd_valid,      //(I) 命令有效
    output wire                     cmd_ready,      //(O) 可接收命令（空闲且不在复位）
    input  wire [ADDR_WIDTH-1:0]    cmd_addr,       //(I) 写起始字节地址
    input  wire [LEN_WIDTH-1:0]     cmd_len,        //(I) 突发拍数 - 1
    output reg                      cmd_done,       //(O) 命令完成脉冲（1 拍）
    output wire                     cmd_err,        //(O) 恒 0（BRESP 拿不到）

    //==================== 写数据接口（数据源供货）====================
    input  wire [DATA_WIDTH-1:0]    wr_data,
    input  wire [STRB_WIDTH-1:0]    wr_strb,
    input  wire                     wr_valid,
    output wire                     wr_ready,

    //==================== AW ====================
    output wire [ID_WIDTH-1:0]      m_AXI_AWID,
    output wire [ADDR_WIDTH-1:0]    m_AXI_AWADDR,
    output wire [LEN_WIDTH-1:0]     m_AXI_AWLEN,
    output wire [2:0]               m_AXI_AWSIZE,
    output wire [1:0]               m_AXI_AWBURST,
    output wire                     m_AXI_AWLOCK,
    output wire [3:0]               m_AXI_AWCACHE,
    output wire [2:0]               m_AXI_AWPROT,
    output wire [3:0]               m_AXI_AWQOS,
    output wire [3:0]               m_AXI_AWREGION,
    output wire                     m_AXI_AWVALID,
    input  wire                     m_AXI_AWREADY,

    //==================== W ====================
    output wire [DATA_WIDTH-1:0]    m_AXI_WDATA,
    output wire [STRB_WIDTH-1:0]    m_AXI_WSTRB,
    output wire                     m_AXI_WLAST,
    output wire                     m_AXI_WVALID,
    input  wire                     m_AXI_WREADY,

    //==================== B ====================
    input  wire [ID_WIDTH-1:0]      m_AXI_BID,
    input  wire [1:0]               m_AXI_BRESP,
    input  wire                     m_AXI_BVALID,
    output wire                     m_AXI_BREADY
);

//------------------------------------------------------------------------------
// 状态编码
//------------------------------------------------------------------------------
localparam  S_IDLE = 2'd0,      // 空闲：等命令
            S_AW   = 2'd1,      // 发写地址
            S_W    = 2'd2,      // 发写数据
            S_B    = 2'd3;      // 等写响应

reg  [1:0]              state;
reg  [1:0]              next_state;

//------------------------------------------------------------------------------
// 数据寄存器
//------------------------------------------------------------------------------
reg  [ADDR_WIDTH-1:0]   addr_r;     // 命令被接受时锁存，握手前不变
reg  [LEN_WIDTH-1:0]    len_r;
reg  [LEN_WIDTH-1:0]    wcnt;       // 已发出的写数据拍数

//------------------------------------------------------------------------------
// 组合条件
//------------------------------------------------------------------------------
// rst_n 已含系统门控（见顶层）：复位期间不接受命令，
// 否则调用方会看到 cmd_ready=1 却丢命令
assign  cmd_ready   = (state == S_IDLE) && rst_n;

wire    cmd_accept  = cmd_valid && cmd_ready;
wire    aw_hs       = m_AXI_AWVALID && m_AXI_AWREADY;
wire    w_hs        = m_AXI_WVALID  && m_AXI_WREADY;
wire    b_hs        = m_AXI_BVALID  && m_AXI_BREADY;
wire    w_last      = (wcnt == len_r);

//------------------------------------------------------------------------------
// 第一段：状态寄存器
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n)
        state <= S_IDLE;
    else
        state <= next_state;
end

//------------------------------------------------------------------------------
// 第二段：次态组合逻辑
//------------------------------------------------------------------------------
always @(*) begin
    next_state = state;
    case (state)
        S_IDLE:     if (cmd_accept)     next_state = S_AW;
        S_AW:       if (aw_hs)          next_state = S_W;
        S_W:        if (w_hs && w_last) next_state = S_B;
        S_B:        if (b_hs)           next_state = S_IDLE;
        default:                        next_state = S_IDLE;
    endcase
end

//------------------------------------------------------------------------------
// 第三段：时序逻辑（数据通路 + 寄存器输出）
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        addr_r   <= {ADDR_WIDTH{1'b0}};
        len_r    <= {LEN_WIDTH{1'b0}};
        wcnt     <= {LEN_WIDTH{1'b0}};
        cmd_done <= 1'b0;
    end
    else begin
        cmd_done <= 1'b0;               // 默认：完成脉冲只打一拍
        case (state)
            S_IDLE: begin
                if (cmd_accept) begin   // 锁存命令
                    addr_r <= cmd_addr;
                    len_r  <= cmd_len;
                    wcnt   <= {LEN_WIDTH{1'b0}};
                end
            end
            S_AW:   ;
            S_W: begin
                if (w_hs && !w_last)
                    wcnt <= wcnt + 1'b1;
            end
            S_B: begin
                if (b_hs)
                    cmd_done <= 1'b1;
            end
            default: ;
        endcase
    end
end

//------------------------------------------------------------------------------
// 组合输出
//------------------------------------------------------------------------------
// AW：awvalid 只由 state 决定 —— 不含 awready（AXI 硬规则）
assign  m_AXI_AWID      = AXI_ID;
assign  m_AXI_AWADDR    = addr_r;
assign  m_AXI_AWLEN     = len_r;
assign  m_AXI_AWSIZE    = 3'b100;       // 2^4 = 16 字节/拍 = 128bit 满宽
assign  m_AXI_AWBURST   = 2'b01;        // INCR
assign  m_AXI_AWLOCK    = 1'b0;
assign  m_AXI_AWCACHE   = 4'b0000;      // 访问 DDR，无需缓存属性
assign  m_AXI_AWPROT    = 3'b000;
assign  m_AXI_AWQOS     = 4'b0000;
assign  m_AXI_AWREGION  = 4'b0000;      // 接 Axi_Mux 时无此信号，留空
assign  m_AXI_AWVALID   = (state == S_AW);

// W：wvalid 只由 (state, wr_valid) 决定 —— 不含 wready（AXI 硬规则）
//    wlast 由 wcnt 与 len_r 比较得到，严格对应 awlen
assign  m_AXI_WDATA     = wr_data;
assign  m_AXI_WSTRB     = wr_strb;
assign  m_AXI_WVALID    = (state == S_W) && wr_valid;
assign  m_AXI_WLAST     = (state == S_W) && w_last;
assign  wr_ready        = (state == S_W) && m_AXI_WREADY;

// B
assign  m_AXI_BREADY    = (state == S_B);

// cmd_err：Axi_Mux 不透传 BRESP，写响应码拿不到，故恒为 0。
//          写是否成功只能靠"写后读回比对"验证。
assign  cmd_err         = 1'b0;

endmodule
