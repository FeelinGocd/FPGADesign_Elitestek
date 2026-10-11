//==============================================================================
//  AXIMastersHandler.v —— 加速器的 AXI 出入口（数据面）
//------------------------------------------------------------------------------
//  干什么
//      把加速器和 DDR 之间的 AXI 通路封成一个模块：
//          对上层：cmd 方式的命令口 + 数据口
//                  —— 上层只说"往这个地址写几拍"，再按拍把数据递过来
//          对下层：AXI4 主端口（接 Axi_Mux 的从口）
//      ★ 上层因此完全不需要知道 AXI 协议（先发 AW、W 逐拍、WLAST、等 B）

//  命令口为什么按方向拆开，而不是像 AXI4_Master.v 那样合成一个
//      AXI4_Master.v 是"一个双向主 + 一个 cmd 口 + cmd_write 位"。
//      那套的问题是：cmd 口只有一份，读命令和写命令得由上层自己排队 ——
//      一个方向忙的时候另一个方向的命令发不出去，正好抵消了"拆成两个主"的意义。
//      我们是【两个平级的主】，所以命令口也平级：
//          写侧：cmd_wr_* + 数据口 wr_*
//          读侧：rd_cmd_* + 数据口 rd_*
//      ★ 名字已经按方向分好，M2 加读侧是【加端口】，不动写侧一行

//  两个方向都有
//      写：FILL 的常数色 / 合成器的输出（S3）
//      读：块拷贝的源（M2，S2）—— 读主与写主同构，端口按方向并排

//  AXI 的固定量不在这里管
//      AWSIZE / AWBURST / AWLOCK / AWCACHE / AWPROT / AWQOS / AWREGION
//      由 AXI4_Master_Write 自己驱动（常量写死在子模块里），
//      本模块只负责把端口接出去 —— 所以它没有任何逻辑，只有例化

//  接口约定（对上层）
//      1) cmd_wr_valid 拉高后 cmd_wr_addr / cmd_wr_len 必须稳定，直到 cmd_wr_ready
//      2) wr_valid 不得由 wr_ready 组合产生，payload 稳定到握手
//      3) cmd_wr_len = 突发拍数 - 1；单笔不跨 4KB、不超过 256 拍，由上层保证
//      4) cmd_wr_done 单拍，且等 B 响应回来才拉 → 它一响就代表数据已落进 DDR

//  参考：docs/参考/AXI总线解析.md（协议）+ docs/spec/设计总纲.md §8（本工程结论）
//        RTL/Peripherial/AXI4/Master/AXI4_Master.v（合并版；本模块按方向拆开）//  上一级是谁
//      GPU_2D.v（把 DDRSource / Factory 的命令与数据接到 SoC 的 AXI）

//==============================================================================
`timescale 1ns/100ps

module AXIMastersHandler #(
    parameter ADDR_WIDTH = 32,
    parameter DATA_WIDTH = 128,
    parameter STRB_WIDTH = DATA_WIDTH/8,        // 16 字节使能位
    parameter ID_WIDTH   = 8,                   // 与 Axi_Mux 的 SLAVE_ID_WIDTH 一致
    parameter LEN_WIDTH  = 8,                   // 突发拍数-1（最大 256 拍 = 4KB）
    parameter AXI_ID     = {ID_WIDTH{1'b0}}     // 全链路 ID = 0
)(
    //==================== 时钟 / 复位 ====================
    input  wire                     clk,
    input  wire                     rst_n,          //(I) 异步复位，低有效

    //==================== 写命令口（对上层：写引擎）  ← Factory ====================
    input  wire                     cmd_wr_valid,   //(I) 提交一笔写
    output wire                     cmd_wr_ready,   //(O) 可接收（空闲且不在复位）
    input  wire [ADDR_WIDTH-1:0]    cmd_wr_addr,    //(I) 写起始字节地址
    input  wire [LEN_WIDTH-1:0]     cmd_wr_len,     //(I) 突发拍数 - 1
    output wire                     cmd_wr_done,    //(O) 整笔完成脉冲（1 拍，含 B 响应）
    output wire                     cmd_wr_err,     //(O) 恒 0（Axi_Mux 不透传 BRESP）

    //==================== 写数据口（对上层：数据源）====================
    input  wire [DATA_WIDTH-1:0]    wr_data,        //(I) 写数据
    input  wire [STRB_WIDTH-1:0]    wr_strb,        //(I) 字节使能（首尾遮罩）
    input  wire                     wr_valid,       //(I) 数据有效
    output wire                     wr_ready,       //(O) 能收这一拍

    //==================== AXI4 主端口 → Axi_Mux S3（写）====================
    // ---- AW ----
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
    // ---- W ----
    output wire [DATA_WIDTH-1:0]    m_AXI_WDATA,
    output wire [STRB_WIDTH-1:0]    m_AXI_WSTRB,
    output wire                     m_AXI_WLAST,
    output wire                     m_AXI_WVALID,
    input  wire                     m_AXI_WREADY,
    // ---- B ----
    input  wire [ID_WIDTH-1:0]      m_AXI_BID,
    input  wire [1:0]               m_AXI_BRESP,
    input  wire                     m_AXI_BVALID,
    output wire                     m_AXI_BREADY,

    //==================== 读命令口（对上层：读引擎）  ← DDRSource ====================
    input  wire                     rd_cmd_valid,   //(I) 提交一笔读
    output wire                     rd_cmd_ready,   //(O) 可接收（空闲且不在复位）
    input  wire [ADDR_WIDTH-1:0]    rd_cmd_addr,    //(I) 读起始字节地址
    input  wire [LEN_WIDTH-1:0]     rd_cmd_len,     //(I) 突发拍数 - 1
    output wire                     rd_cmd_done,    //(O) 整笔完成脉冲（1 拍）
    output wire                     rd_cmd_err,     //(O) 整笔期间有过非 OKAY（粘滞）

    //==================== 读数据口（对上层：送下游）====================
    output wire [DATA_WIDTH-1:0]    rd_data,        //(O) 读回的一拍
    output wire                     rd_valid,       //(O) 这一拍有效
    output wire                     rd_last,        //(O) 本笔最后一拍
    input  wire                     rd_ready,       //(I) 下游收下了

    //==================== AXI4 主端口 → Axi_Mux S2（读）====================
    // ---- AR ----
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
    // ---- R ----
    input  wire [ID_WIDTH-1:0]      m_AXI_RID,
    input  wire [DATA_WIDTH-1:0]    m_AXI_RDATA,
    input  wire [1:0]               m_AXI_RRESP,
    input  wire                     m_AXI_RLAST,
    input  wire                     m_AXI_RVALID,
    output wire                     m_AXI_RREADY
);

//==============================================================================
// 例化
//==============================================================================
//------------------------------------------------------------------------------
// 写通路（v1 只有它）
//------------------------------------------------------------------------------
AXI4_Master_Write #(
    .ADDR_WIDTH     (ADDR_WIDTH),
    .DATA_WIDTH     (DATA_WIDTH),
    .STRB_WIDTH     (STRB_WIDTH),
    .ID_WIDTH       (ID_WIDTH),
    .LEN_WIDTH      (LEN_WIDTH),
    .AXI_ID         (AXI_ID)
) u_AXI4_Master_Write (
    .clk            (clk),
    .rst_n          (rst_n),

    // 命令口：把上层那套 cmd_wr_* 接到子模块的 cmd_*
    .cmd_valid      (cmd_wr_valid),
    .cmd_ready      (cmd_wr_ready),
    .cmd_addr       (cmd_wr_addr),
    .cmd_len        (cmd_wr_len),
    .cmd_done       (cmd_wr_done),
    .cmd_err        (cmd_wr_err),

    // 数据口：原样透传
    .wr_data        (wr_data),
    .wr_strb        (wr_strb),
    .wr_valid       (wr_valid),
    .wr_ready       (wr_ready),

    // AXI 主口：原样透传
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
    .m_AXI_BREADY   (m_AXI_BREADY)
);

//------------------------------------------------------------------------------
// 读通路
//------------------------------------------------------------------------------
AXI4_Master_Read #(
    .ADDR_WIDTH     (ADDR_WIDTH),
    .DATA_WIDTH     (DATA_WIDTH),
    .ID_WIDTH       (ID_WIDTH),
    .LEN_WIDTH      (LEN_WIDTH),
    .AXI_ID         (AXI_ID)
) u_AXI4_Master_Read (
    .clk            (clk),
    .rst_n          (rst_n),

    // 命令口：原样接出去
    .cmd_valid      (rd_cmd_valid),
    .cmd_ready      (rd_cmd_ready),
    .cmd_addr       (rd_cmd_addr),
    .cmd_len        (rd_cmd_len),
    .cmd_done       (rd_cmd_done),
    .cmd_err        (rd_cmd_err),

    // 数据口：原样接出去
    .rd_data        (rd_data),
    .rd_valid       (rd_valid),
    .rd_last        (rd_last),
    .rd_ready       (rd_ready),

    // AXI 主口：原样透传
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
    .m_AXI_ARREADY  (m_AXI_ARREADY),

    .m_AXI_RID      (m_AXI_RID),
    .m_AXI_RDATA    (m_AXI_RDATA),
    .m_AXI_RRESP    (m_AXI_RRESP),
    .m_AXI_RLAST    (m_AXI_RLAST),
    .m_AXI_RVALID   (m_AXI_RVALID),
    .m_AXI_RREADY   (m_AXI_RREADY)
);

endmodule
