/* 2026/4/19 by FeelinGood
async_fifo：异步FIFO模块，采用格雷码指针同步，支持不同读写时钟域
 - 参数说明：
   - 数据位宽（WR_WIDTH）、FIFO深度（2^ADDR_WIDTH）、读数据位宽（RD_WIDTH，必须≤WR_WIDTH）
 - 内部实现：Designs
   - 使用格雷码编码读写指针，并通过两级寄存器同步到对方时钟域，避免亚稳态
   - 写满条件：写指针与同步后读指针的格雷码最高两位相反，其余位相等
   - 读空条件：读指针格雷码等于同步后写指针格雷码
 - 复位特性：
   - 两个时钟域使用独立的复位信号 rst_wr_n 和 rst_rd_n，分别复位各自时钟域寄存器
   - 复位后读写指针归零，wr_full = 0，rd_empty = 1
 - 延迟特性：
   - 写入数据到可被读取的延迟为：读指针同步延迟（2个读时钟周期）+ 组合逻辑延迟
   - 空/满标志更新延迟：跨时钟同步延迟（2个目标时钟周期）
 - 使用注意：
   - 参数 WR_WIDTH 必须能够被 RD_WIDTH 整除（内部存在 RD_MAX_CNT = WR_WIDTH/RD_WIDTH，当前未用于数据拼接）
   - 当 RD_WIDTH < WR_WIDTH 时，模块输出位宽仍为 WR_WIDTH，如需窄位宽输出需外部处理
*/
module async_fifo #(
    parameter WR_WIDTH      = 10,          // 写入数据位宽
    parameter ADDR_WIDTH    = 10,          // 地址深度（2^ADDR_WIDTH）
    parameter RD_WIDTH      = 2,           // 读取数据位宽（必须 ≤ WR_WIDTH）
    parameter USE_REGISTERS = 0            // shallow CDC queues must not consume full RAM blocks
)(
    // write domain
    input                       clk_wr,
    input                       rst_wr_n,
    input       [WR_WIDTH-1:0]  data_in,
    input                       en_wr,
    output  reg                 wr_full,
    // read domain
    input                       clk_rd,
    input                       rst_rd_n,
    output      [WR_WIDTH-1:0]  data_out,
    input                       en_rd,
    output  reg                 rd_empty
);

// localparam
localparam RD_MAX_CNT = WR_WIDTH / RD_WIDTH;

reg  [ADDR_WIDTH:0] pwr_bin;                     // 写指针（二进制，高位用于圈计数）
reg  [ADDR_WIDTH:0] pwr_grey;                    // 写指针（格雷码）
wire [ADDR_WIDTH:0] pwr_bin_next;
wire [ADDR_WIDTH:0] pwr_grey_next;
wire [ADDR_WIDTH:0] pwr_bin_inc;
wire [ADDR_WIDTH:0] pwr_grey_inc;

reg  [ADDR_WIDTH:0] prd_bin;                     // 读指针（二进制，高位用于圈计数）
reg  [ADDR_WIDTH:0] prd_grey;                    // 读指针（格雷码）
wire [ADDR_WIDTH:0] prd_bin_next;
wire [ADDR_WIDTH:0] prd_grey_next;
wire [ADDR_WIDTH:0] prd_bin_inc;
wire [ADDR_WIDTH:0] prd_grey_inc;

wire full_next;
wire empty_next;
wire write_take;
wire read_take;
wire full_hold;
wire full_inc;
wire empty_hold;
wire empty_inc;
wire [ADDR_WIDTH:0] full_compare_target;

// 预增指针和 Gray 码不依赖外部请求。空/满标志并行计算当前/递增两种
// 比较结果，再用本拍握手选择，避免 ready/enable 穿过加法和宽比较。
assign write_take   = en_wr & ~wr_full;
assign pwr_bin_inc  = pwr_bin + 1'b1;
assign pwr_grey_inc = pwr_bin_inc ^ (pwr_bin_inc >> 1);
assign pwr_bin_next = write_take ? pwr_bin_inc : pwr_bin;
assign pwr_grey_next = write_take ? pwr_grey_inc : pwr_grey;

// 同步读指针（格雷码）
(* async_reg = "true" *) reg [ADDR_WIDTH:0] prd_grey_d, prd_grey_dd;

always @(posedge clk_wr or negedge rst_wr_n) begin
    if (!rst_wr_n) begin
        prd_grey_d  <= 'b0;
        prd_grey_dd <= 'b0;
    end else begin
        prd_grey_d  <= prd_grey;
        prd_grey_dd <= prd_grey_d;
    end
end

// 写指针更新
always @(posedge clk_wr or negedge rst_wr_n) begin
    if (!rst_wr_n) begin
        pwr_bin  <= 'b0;
        pwr_grey <= 'b0;
    end else begin
        pwr_bin  <= pwr_bin_next;
        pwr_grey <= pwr_grey_next;
    end
end

// 生成写满标志（格雷码最高两位取反比较）

assign full_compare_target = {~prd_grey_dd[ADDR_WIDTH:ADDR_WIDTH-1],
                              prd_grey_dd[ADDR_WIDTH-2:0]};
// hold 分支也必须重新比较：没有写入时，对端读指针仍会跨域更新。
assign full_hold = (pwr_grey == full_compare_target);
assign full_inc  = (pwr_grey_inc == full_compare_target);
assign full_next = write_take ? full_inc : full_hold;

always @(posedge clk_wr or negedge rst_wr_n) begin
    if (!rst_wr_n)
        wr_full <= 1'b0;
    else
        wr_full <= full_next;
end

// 读时钟域：与写侧一样，先预增再在末端选择，不改变指针/标志周期。
assign read_take    = en_rd & ~rd_empty;
assign prd_bin_inc  = prd_bin + 1'b1;
assign prd_grey_inc = prd_bin_inc ^ (prd_bin_inc >> 1);
assign prd_bin_next = read_take ? prd_bin_inc : prd_bin;
assign prd_grey_next = read_take ? prd_grey_inc : prd_grey;

// 同步写指针（格雷码）
(* async_reg = "true" *) reg [ADDR_WIDTH:0] pwr_grey_d, pwr_grey_dd;

always @(posedge clk_rd or negedge rst_rd_n) begin
    if (!rst_rd_n) begin
        pwr_grey_d  <= 'b0;
        pwr_grey_dd <= 'b0;
    end else begin
        pwr_grey_d  <= pwr_grey;
        pwr_grey_dd <= pwr_grey_d;
    end
end

// 读指针更新
always @(posedge clk_rd or negedge rst_rd_n) begin
    if (!rst_rd_n) begin
        prd_bin  <= 'b0;
        prd_grey <= 'b0;
    end else begin
        prd_bin  <= prd_bin_next;
        prd_grey <= prd_grey_next;
    end
end

// 生成读空标志（格雷码相等比较）
// 没有读取时仍重新比较，使新写入同步后能够解除 rd_empty。
assign empty_hold = (prd_grey == pwr_grey_dd);
assign empty_inc  = (prd_grey_inc == pwr_grey_dd);
assign empty_next = read_take ? empty_inc : empty_hold;

always @(posedge clk_rd or negedge rst_rd_n) begin
    if (!rst_rd_n)
        rd_empty <= 1'b1;
    else
        rd_empty <= empty_next;
end

// BRAM 接口信号
wire                    bram_wr_en;
wire [ADDR_WIDTH-1:0]   bram_wr_addr;
wire                    bram_rd_en;
wire [ADDR_WIDTH-1:0]   bram_rd_addr;

assign bram_wr_en   = (en_wr && !wr_full) ? 1'b1 : 1'b0;
assign bram_wr_addr = pwr_bin[ADDR_WIDTH-1:0];
assign bram_rd_en   = (en_rd && !rd_empty) ? 1'b1 : 1'b0;
assign bram_rd_addr = prd_bin[ADDR_WIDTH-1:0];

// Small command/beat queues use registers; display and CI queues keep BRAM.
generate if (USE_REGISTERS) begin : g_register_memory
    (* syn_ramstyle = "registers" *) reg [WR_WIDTH-1:0] memory_r [0:(1<<ADDR_WIDTH)-1];
    reg [WR_WIDTH-1:0] read_r;
    assign data_out = read_r;
    always @(posedge clk_wr) begin
        if (bram_wr_en) memory_r[bram_wr_addr] <= data_in;
    end
    always @(posedge clk_rd) begin
        if (bram_rd_en) read_r <= memory_r[bram_rd_addr];
    end
end else begin : g_block_memory
// 实例化 BRAM 模块
BlockRAM # (
.DATA_WIDTH         (WR_WIDTH),
.ADDR_WIDTH         (ADDR_WIDTH),
.USE_BYTE_EN        (0),
.INIT_FILE_PATH     ()
)
u_BlockRAM (
    .bram_clk_wr        (clk_wr),
    .bram_data_in       (data_in),
    .bram_en_wr         (bram_wr_en),
    .bram_addr_wr       (bram_wr_addr),
    .bram_byte_en_wr    (),
    .bram_clk_rd        (clk_rd),
    .bram_data_out      (data_out),
    .bram_en_rd         (bram_rd_en),
    .bram_addr_rd       (bram_rd_addr)
);
end endgenerate
endmodule
