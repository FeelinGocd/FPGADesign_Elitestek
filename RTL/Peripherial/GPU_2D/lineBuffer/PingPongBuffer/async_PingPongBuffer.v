/* 2026/4/20 by FeelinGood
   rev 2026-09-19：拆双时钟域 + 读侧控制全局块切换

 结构：一根请求 + 一根应答（四相握手），决策只在读侧
   写域：写满一块 → buffer_wr_block_done → wr_block_done = 1（请求，保持住）
         收到读侧的切换应答 → 跟随翻 bank，并撤销请求
   读域：本块读空 且 写侧已交一块（wr_block_done 同步过来）→ 翻 rd_block_r，并翻 rd_switch_tgl（应答）
         ★ 全局块切换的唯一决策点在这里；写侧不做任何判断，只跟随

 两边 bank 永远互补：复位时 wr=0 / rd=1，之后每来一次应答两边各翻一次。
   因此：写侧永远不会碰到读侧正在读的那块；读侧切过去的那块永远是写侧刚交完的那块。

 跨域只有两根线（各 1 bit）
   ① 写→读  wr_block_done   电平：保持到被消费。跨域脉冲会漏采，所以用状态电平
   ② 读→写  rd_switch_flip   toggle：每切一次翻一次 + seen。天然不受脉冲宽度限制

 加固（两条都是实测踩出来的）
   - fifoX_wr_en 带 ~wr_block_done：一块交完后立刻停收数，
     否则下一块的头几拍会灌进上一块的尾巴，两边块计数就永久错开
   - buffer_wr_block_ready 带 rst_wr_n：本域复位期间不发"能收"。
     否则复位释放那两拍上游照发、DUT 写指针还被按住 → 收下的数被丢掉，上游计数却走了
*/
module async_PingPongBuffer #(
    parameter DATA_WIDTH = 16,
    parameter DEPTH      = 512
)(
    input  wire                     clk_wr,             // 写时钟域（AXI 侧，100MHz）
    input  wire                     clk_rd,             // 读时钟域（像素侧，148.5MHz）
    input  wire                     rst_n,              // 外部复位

    // ---- 写侧 ----
    input  wire                     buffer_wr_en,
    input  wire [DATA_WIDTH-1:0]    buffer_wr_data,
    input  wire                     buffer_wr_block_done,
    output wire                     buffer_wr_block_ready,

    // ---- 读侧 ----
    input  wire                     buffer_rd_en,
    output wire [DATA_WIDTH-1:0]    buffer_rd_data,
    output wire                     buffer_rd_valid,
    output wire                     buffer_rd_block_ready,

    output wire                     buf_select          // 调试：当前读 bank

);

localparam ADDR_WIDTH = $clog2(DEPTH);          // 地址宽度

//==============================================================================
// FIFO 接口
//==============================================================================
wire                    fifo0_wr_en;
wire                    fifo0_wr_full;
wire                    fifo0_rd_en;
wire                    fifo0_rd_empty;
wire                    fifo0_rd_valid;
wire [DATA_WIDTH-1:0]   fifo0_buffer_rd_data;

wire                    fifo1_wr_en;
wire                    fifo1_wr_full;
wire                    fifo1_rd_en;
wire                    fifo1_rd_empty;
wire                    fifo1_rd_valid;
wire [DATA_WIDTH-1:0]   fifo1_buffer_rd_data;

wire                    rd_valid;               // 当前读出数据是否有效
reg                     buffer_rd_valid_r;      // 同步数据输出 读数据有效

// 复位
reg                     wr_rst_sync_d;
reg                     wr_rst_sync_dd;
reg                     rd_rst_sync_d;
reg                     rd_rst_sync_dd;
wire                    rst_wr_n;
wire                    rst_rd_n;

// 写域
reg                     wr_block_r;             // 当前写 bank
reg                     wr_block_done;          // 本块已交完（请求，保持到读侧应答）
reg                     rd_switch_flip_d;        // [写域] 读侧应答 同步第 1 级
reg                     rd_switch_flip_dd;       // [写域] 读侧应答 同步第 2 级
reg                     wr_rd_switch;           // [写域] 已消费到的应答值

wire                    rd_switch_pending;

// 读域
wire                    rd_wr_block_done;
wire                    rd_block_empty;

// 跨域同步
reg                     rd_block_r;             // 当前读 bank
reg                     rd_switch_flip;          // 读侧换块应答（每切一次翻一次）
reg                     wr_block_done_d;        // [读域] 写侧请求 同步第 1 级
reg                     wr_block_done_dd;       // [读域] 写侧请求 同步第 2 级

//==============================================================================
// 复位同步：外部 rst_n 单源、异步断言；两个域各自同步撤销
//==============================================================================
assign  rst_wr_n    = wr_rst_sync_dd;
assign  rst_rd_n    = rd_rst_sync_dd;

always @(posedge clk_wr or negedge rst_n) begin
    if (!rst_n) begin
        wr_rst_sync_d   <= 1'b0;
        wr_rst_sync_dd  <= 1'b0;
    end
    else begin
        wr_rst_sync_d   <= 1'b1;
        wr_rst_sync_dd  <= wr_rst_sync_d;
    end
end

always @(posedge clk_rd or negedge rst_n) begin
    if (!rst_n) begin
        rd_rst_sync_d   <= 1'b0;
        rd_rst_sync_dd  <= 1'b0;
    end
    else begin
        rd_rst_sync_d   <= 1'b1;
        rd_rst_sync_dd  <= rd_rst_sync_d;
    end
end


//==============================================================================
// 写域 clk_wr（从：只跟随，不做判断）
//==============================================================================

assign  rd_switch_pending   = (rd_switch_flip_dd != wr_rd_switch);

// ---- ② 读→写：读侧换块（toggle，每切一次翻一次）----
always @(posedge clk_wr or negedge rst_wr_n) begin
    if (!rst_wr_n) begin
        rd_switch_flip_d     <= 1'b0;
        rd_switch_flip_dd    <= 1'b0;
    end
    else begin
        rd_switch_flip_d     <= rd_switch_flip;
        rd_switch_flip_dd    <= rd_switch_flip_d;
    end
end

always @(posedge clk_wr or negedge rst_wr_n) begin
    if (!rst_wr_n) begin
        wr_block_r      <= 1'b0;        // 复位后先写 FIFO0（和读侧的 bank1 互补）
        wr_block_done   <= 1'b0;
        wr_rd_switch    <= 1'b0;
    end
    else begin
        if (buffer_wr_block_done) begin
            wr_block_done   <= 1'b1;    // 本块交完 → 发请求
        end
        if (rd_switch_pending) begin
            wr_block_r      <= ~wr_block_r;
            wr_block_done   <= 1'b0;
            wr_rd_switch    <= rd_switch_flip_dd;
        end
    end
end

//==============================================================================
// 读域 clk_rd（主：全局块切换的唯一决策点）
//==============================================================================

assign  rd_wr_block_done    = wr_block_done_dd;     // [读域] 写侧已交一块;
assign  rd_block_empty      = (rd_block_r == 1'b0) ? fifo0_rd_empty : fifo1_rd_empty;

// ---- ① 写→读：本块写完（电平请求，保持到写侧跟随才撤销）----
always @(posedge clk_rd or negedge rst_rd_n) begin
    if (!rst_rd_n) begin
        wr_block_done_d     <= 1'b0;
        wr_block_done_dd    <= 1'b0;
    end
    else begin
        wr_block_done_d     <= wr_block_done;
        wr_block_done_dd    <= wr_block_done_d;
    end
end

always @(posedge clk_rd or negedge rst_rd_n) begin
    if (!rst_rd_n) begin
        rd_block_r          <= 1'b1;        // 开局停在 FIFO1（空的），等写侧第一块
        rd_switch_flip      <= 1'b0;
    end
    else if (rd_block_empty && rd_wr_block_done) begin
        rd_block_r          <= ~rd_block_r;
        rd_switch_flip      <= ~rd_switch_flip;      // 应答写侧
    end
end

//==============================================================================
// 组合条件
//==============================================================================
assign  fifo0_wr_en             = buffer_wr_en && !fifo0_wr_full  && (wr_block_r == 1'b0) && ~wr_block_done && rst_wr_n;
assign  fifo1_wr_en             = buffer_wr_en && !fifo1_wr_full  && (wr_block_r == 1'b1) && ~wr_block_done && rst_wr_n;

assign  fifo0_rd_en             = buffer_rd_en && !fifo0_rd_empty && (rd_block_r == 1'b0);
assign  fifo1_rd_en             = buffer_rd_en && !fifo1_rd_empty && (rd_block_r == 1'b1);
assign  fifo0_rd_valid          = buffer_rd_en && !fifo0_rd_empty;
assign  fifo1_rd_valid          = buffer_rd_en && !fifo1_rd_empty;

// 有效数据输出选择
assign  buffer_rd_data          = (rd_block_r == 1'b0) ? fifo0_buffer_rd_data : fifo1_buffer_rd_data;
assign  buffer_rd_valid         = buffer_rd_valid_r;
assign  rd_valid                = (rd_block_r == 1'b0) ? fifo0_rd_valid : fifo1_rd_valid;
assign  buffer_rd_block_ready   = (rd_block_r == 1'b0) ? !fifo0_rd_empty : !fifo1_rd_empty;
assign  buffer_wr_block_ready   = rst_wr_n && ~wr_block_done && ((wr_block_r == 1'b0) ? !fifo0_wr_full : !fifo1_wr_full);
assign  buf_select              = rd_block_r;

// buffer_rd_valid 延后一个时钟（BRAM 是时序输出）
always @(posedge clk_rd or negedge rst_rd_n) begin
    if (!rst_rd_n)
        buffer_rd_valid_r <= 1'b0;
    else
        buffer_rd_valid_r <= rd_valid;
end

//==============================================================================
// 异步 FIFO
//==============================================================================
async_fifo #(
    .WR_WIDTH   (DATA_WIDTH),
    .ADDR_WIDTH (ADDR_WIDTH),
    .RD_WIDTH   (DATA_WIDTH)
) u_fifo0 (
    .clk_wr     (clk_wr),
    .rst_wr_n   (rst_wr_n),
    .data_in    (buffer_wr_data),
    .en_wr      (fifo0_wr_en),
    .wr_full    (fifo0_wr_full),
    .clk_rd     (clk_rd),
    .rst_rd_n   (rst_rd_n),
    .data_out   (fifo0_buffer_rd_data),
    .en_rd      (fifo0_rd_en),
    .rd_empty   (fifo0_rd_empty)
);

async_fifo #(
    .WR_WIDTH   (DATA_WIDTH),
    .ADDR_WIDTH (ADDR_WIDTH),
    .RD_WIDTH   (DATA_WIDTH)
) u_fifo1 (
    .clk_wr     (clk_wr),
    .rst_wr_n   (rst_wr_n),
    .data_in    (buffer_wr_data),
    .en_wr      (fifo1_wr_en),
    .wr_full    (fifo1_wr_full),
    .clk_rd     (clk_rd),
    .rst_rd_n   (rst_rd_n),
    .data_out   (fifo1_buffer_rd_data),
    .en_rd      (fifo1_rd_en),
    .rd_empty   (fifo1_rd_empty)
);

endmodule
