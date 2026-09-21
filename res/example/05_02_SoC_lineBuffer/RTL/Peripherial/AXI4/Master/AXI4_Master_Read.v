//////////////////////////////////////////////////////////////////////////////
// AXI4_Master_Read —— AXI4 读通路
//   一笔读事务 = 1×AR + N×R。
//   发 AR 后从机逐拍回数据，转送到 rd_*；末拍由从机的 RLAST 标记，
//   RRESP 非 OKAY 时置 cmd_err。
//
// 接口组：
//   clk / rst_n     时钟、复位（顶层已过滤系统门控）
//   cmd_*           命令接口：提交一笔读（去哪 / 多长）
//   rd_*            读数据接口：数据交给下游（推式）
//   m_AXI_AR_*      读地址通道
//   m_AXI_R_*       读数据通道
//
// 约束：
//   1. cmd_valid 拉高后 cmd_addr / cmd_len 必须稳定，直到 cmd_ready
//   2. rd_valid 不得由 rd_ready 组合产生（合法方向只有 rd_ready 依赖 rd_valid）
//   结束以从机的 RLAST 为准，不自己数拍。
//
// 参考：docs/spec/AXI4FULL解析.md §5 / §6 / §9 / §15
//////////////////////////////////////////////////////////////////////////////
`timescale 1ns/100ps

module AXI4_Master_Read #(
    parameter ADDR_WIDTH = 32,
    parameter DATA_WIDTH = 128,
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
    input  wire [ADDR_WIDTH-1:0]    cmd_addr,       //(I) 读起始字节地址
    input  wire [LEN_WIDTH-1:0]     cmd_len,        //(I) 突发拍数 - 1
    output reg                      cmd_done,       //(O) 命令完成脉冲（1 拍）
    output reg                      cmd_err,        //(O) 响应非 OKAY（粘滞到下条命令）

    //==================== 读数据接口（送下游）====================
    output wire [DATA_WIDTH-1:0]    rd_data,
    output wire                     rd_valid,
    output wire                     rd_last,
    input  wire                     rd_ready,

    //==================== AR ====================
    output wire [ID_WIDTH-1:0]      m_AXI_ARID,
    output wire [ADDR_WIDTH-1:0]    m_AXI_ARADDR,
    output wire [LEN_WIDTH-1:0]     m_AXI_ARLEN,
    output wire [2:0]               m_AXI_ARSIZE,
    output wire [1:0]               m_AXI_ARBURST,
    output wire                     m_AXI_ARLOCK,
    output wire [3:0]               m_AXI_ARCACHE,
    output wire [2:0]               m_AXI_ARPROT,
    output wire [3:0]               m_AXI_ARQOS,
    output wire [3:0]               m_AXI_ARREGION,
    output wire                     m_AXI_ARVALID,
    input  wire                     m_AXI_ARREADY,

    //==================== R ====================
    input  wire [ID_WIDTH-1:0]      m_AXI_RID,
    input  wire [DATA_WIDTH-1:0]    m_AXI_RDATA,
    input  wire [1:0]               m_AXI_RRESP,
    input  wire                     m_AXI_RLAST,
    input  wire                     m_AXI_RVALID,
    output wire                     m_AXI_RREADY
);

//------------------------------------------------------------------------------
// 状态编码
//------------------------------------------------------------------------------
localparam  S_IDLE = 2'd0,      // 空闲：等命令
            S_AR   = 2'd1,      // 发读地址
            S_R    = 2'd2;      // 收读数据

reg  [1:0]              state;
reg  [1:0]              next_state;

//------------------------------------------------------------------------------
// 数据寄存器
//------------------------------------------------------------------------------
reg  [ADDR_WIDTH-1:0]   addr_r;     // 命令被接受时锁存，握手前不变
reg  [LEN_WIDTH-1:0]    len_r;      // 只用于 ARLEN；R 侧以 RLAST 结束

//------------------------------------------------------------------------------
// 组合条件
//------------------------------------------------------------------------------
// rst_n 已含系统门控（见顶层）：复位期间不接受命令
assign  cmd_ready   = (state == S_IDLE) && rst_n;

wire    cmd_accept  = cmd_valid && cmd_ready;
wire    ar_hs       = m_AXI_ARVALID && m_AXI_ARREADY;
wire    r_hs        = m_AXI_RVALID  && m_AXI_RREADY;
wire    r_done      = r_hs && m_AXI_RLAST;      // 最后一拍握手 = 整笔结束

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
        S_IDLE:     if (cmd_accept)     next_state = S_AR;
        S_AR:       if (ar_hs)          next_state = S_R;
        S_R:        if (r_done)         next_state = S_IDLE;
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
        cmd_done <= 1'b0;
        cmd_err  <= 1'b0;
    end
    else begin
        cmd_done <= 1'b0;               // 默认：完成脉冲只打一拍

        // 错误标志：整笔期间只要有一拍响应非 OKAY 就置位，
        //           粘滞到下一条命令被接受（便于状态寄存器读取）
        if (cmd_accept)
            cmd_err <= 1'b0;
        else if (r_hs && (m_AXI_RRESP != 2'b00))
            cmd_err <= 1'b1;

        case (state)
            S_IDLE: begin
                if (cmd_accept) begin   // 锁存命令
                    addr_r <= cmd_addr;
                    len_r  <= cmd_len;
                end
            end
            S_AR:   ;
            S_R: begin
                if (r_done)
                    cmd_done <= 1'b1;
            end
            default: ;
        endcase
    end
end

//------------------------------------------------------------------------------
// 组合输出
//------------------------------------------------------------------------------
// AR：arvalid 只由 state 决定 —— 不含 arready（AXI 硬规则）
assign  m_AXI_ARID      = AXI_ID;
assign  m_AXI_ARADDR    = addr_r;
assign  m_AXI_ARLEN     = len_r;
assign  m_AXI_ARSIZE    = 3'b100;       // 2^4 = 16 字节/拍 = 128bit 满宽
assign  m_AXI_ARBURST   = 2'b01;        // INCR
assign  m_AXI_ARLOCK    = 1'b0;
assign  m_AXI_ARCACHE   = 4'b0000;
assign  m_AXI_ARPROT    = 3'b000;
assign  m_AXI_ARQOS     = 4'b0000;
assign  m_AXI_ARREGION  = 4'b0000;      // 接 Axi_Mux 时无此信号，留空
assign  m_AXI_ARVALID   = (state == S_AR);

// R：rd_valid 只由 (state, m_AXI_RVALID) 决定 —— 不含 rd_ready（AXI 硬规则）
//    rd_last 只在数据有效的那一拍拉高
assign  rd_data         = m_AXI_RDATA;
assign  rd_valid        = (state == S_R) && m_AXI_RVALID;
assign  rd_last         = (state == S_R) && m_AXI_RVALID && m_AXI_RLAST;
assign  m_AXI_RREADY    = (state == S_R) && rd_ready;

endmodule
