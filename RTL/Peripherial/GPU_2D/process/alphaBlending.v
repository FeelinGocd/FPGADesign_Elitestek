//==============================================================================
//  alphaBlending.v —— 带反压的四级前景/背景 alpha 流水
//------------------------------------------------------------------------------
//  一拍处理 4 个 RGB888 像素，保持原来的精确公式：
//
//      out = bg + (fg - bg) * alpha / 255
//      q   = (abs(diff) * alpha + 1 + ((abs(diff) * alpha) >> 8)) >> 8
//
//  差值、乘法、/255 和加回背景分别落在寄存器之间，避免 300 MHz
//  时钟下把整条算术链放在一个组合路径中。四级都是 elastic register，
//  下游停顿时数据、掩码和符号一起停，不丢像素。
//
//  上一级：factory/Compositor.v。参考：docs/spec/硬件_RTL规范.md §4、
//  docs/spec/硬件_加速器.md §5.3
//==============================================================================
`timescale 1ns/100ps

module alphaBlending #(
    parameter PIXEL_WIDTH       = 96,
    parameter PIXELS            = 4,
    parameter BYTES_PER_PIXEL   = 3,
    parameter ALPHA_WIDTH       = 8
)(
    input  wire                                  clk,
    input  wire                                  rst_n,
    input  wire                                  line_start,
    input  wire                                  line_done,
    input  wire [PIXEL_WIDTH-1:0]                fg_data,
    input  wire [PIXELS*ALPHA_WIDTH-1:0]         fg_alpha,
    input  wire [PIXEL_WIDTH-1:0]                bg_data,
    input  wire [PIXEL_WIDTH/8-1:0]              fg_mask,
    input  wire                                  in_valid,
    output wire                                  in_ready,
    output wire [PIXEL_WIDTH-1:0]                blend_data,
    output wire [PIXELS*BYTES_PER_PIXEL-1:0]     blend_mask,
    output wire                                  out_valid,
    input  wire                                  out_ready,
    output wire                                  pipeline_free
);

localparam PIXEL_BYTES = PIXELS * BYTES_PER_PIXEL;
localparam FIFO_DEPTH = 4;
localparam FIFO_PTR_W = 2;
localparam FIFO_CNT_W = 3;

// stage 0: 差值 / 符号 / 绝对值
reg [PIXEL_BYTES*9-1:0]  diff_abs_r;
reg [PIXEL_BYTES-1:0]     diff_neg_r;
reg [PIXEL_BYTES*8-1:0]   alpha_byte_r;
reg [PIXEL_BYTES*8-1:0]   bg_r;
reg [PIXEL_BYTES-1:0]     mask_r;
reg                       v0_r;

// stage 1: DSP 乘法
reg [PIXEL_BYTES*17-1:0]  product_r;
reg [PIXEL_BYTES-1:0]     neg1_r;
reg [PIXEL_BYTES*8-1:0]   bg1_r;
reg [PIXEL_BYTES-1:0]     mask1_r;
reg [PIXEL_BYTES-1:0]     alpha_nz1_r;
reg                       v1_r;

// stage 2: 精确 /255
reg [PIXEL_BYTES*9-1:0]   quotient_r;
reg [PIXEL_BYTES-1:0]     neg2_r;
reg [PIXEL_BYTES*8-1:0]   bg2_r;
reg [PIXEL_BYTES-1:0]     mask2_r;
reg                       v2_r;

// stage 3: 加回背景，输出寄存器
reg [PIXEL_WIDTH-1:0]     data3_r;
reg [PIXEL_BYTES-1:0]     mask3_r;
reg                       v3_r;

// A small registered output FIFO keeps DDRPacker's ready signal out of the
// arithmetic/input ready chain. Its occupancy changes only on a clock edge.
reg [PIXEL_WIDTH-1:0] fifo_data_r [0:FIFO_DEPTH-1];
reg [PIXEL_BYTES-1:0] fifo_mask_r [0:FIFO_DEPTH-1];
reg [FIFO_PTR_W-1:0] fifo_wr_ptr_r;
reg [FIFO_PTR_W-1:0] fifo_rd_ptr_r;
reg [FIFO_CNT_W-1:0] fifo_count_r;
wire fifo_in_ready;
wire fifo_push;
wire fifo_pop;
wire ready3;
wire ready2;
wire ready1;
wire ready0;
integer i;

assign fifo_in_ready = (fifo_count_r < FIFO_DEPTH);
assign fifo_push = v3_r & fifo_in_ready;
assign fifo_pop = (fifo_count_r != 0) & out_ready;
assign ready3 = ~v3_r | fifo_in_ready;
assign ready2 = ~v2_r | ready3;
assign ready1 = ~v1_r | ready2;
assign ready0 = ~v0_r | ready1;

// Boundary flush has priority at the clock edge. A paired input consumed on
// that edge is an unused tail and is discarded; do not put the writer's
// combinational line_done path back into the source/FIFO ready chain.
assign in_ready      = ready0;
assign out_valid     = (fifo_count_r != 0) & ~line_start & ~line_done;
assign blend_data    = fifo_data_r[fifo_rd_ptr_r];
assign blend_mask    = fifo_mask_r[fifo_rd_ptr_r];
assign pipeline_free = ~v0_r & ~v1_r & ~v2_r & ~v3_r & (fifo_count_r == 0);

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        v0_r <= 1'b0;
        v1_r <= 1'b0;
        v2_r <= 1'b0;
        v3_r <= 1'b0;
        fifo_wr_ptr_r <= {FIFO_PTR_W{1'b0}};
        fifo_rd_ptr_r <= {FIFO_PTR_W{1'b0}};
        fifo_count_r <= {FIFO_CNT_W{1'b0}};
    end
    else if (line_start || line_done) begin
        v0_r <= 1'b0;
        v1_r <= 1'b0;
        v2_r <= 1'b0;
        v3_r <= 1'b0;
        fifo_wr_ptr_r <= {FIFO_PTR_W{1'b0}};
        fifo_rd_ptr_r <= {FIFO_PTR_W{1'b0}};
        fifo_count_r <= {FIFO_CNT_W{1'b0}};
    end
    else begin
        // The output FIFO is deliberately clocked separately from its ready
        // consumer; this breaks the long DDRPacker -> source ready path.
        if (fifo_push) begin
            fifo_data_r[fifo_wr_ptr_r] <= data3_r;
            fifo_mask_r[fifo_wr_ptr_r] <= mask3_r;
            fifo_wr_ptr_r <= fifo_wr_ptr_r + 1'b1;
        end
        if (fifo_pop)
            fifo_rd_ptr_r <= fifo_rd_ptr_r + 1'b1;
        case ({fifo_push, fifo_pop})
            2'b10: fifo_count_r <= fifo_count_r + 1'b1;
            2'b01: fifo_count_r <= fifo_count_r - 1'b1;
            default: fifo_count_r <= fifo_count_r;
        endcase

        // stage 3: quotient + sign + background -> final RGB byte
        if (ready3) begin
            v3_r <= v2_r;
            if (v2_r) begin
                for (i = 0; i < PIXEL_BYTES; i = i + 1) begin
                    if (neg2_r[i])
                        data3_r[i*8 +: 8] <= {1'b0, bg2_r[i*8 +: 8]}
                                             - quotient_r[i*9 +: 9];
                    else
                        data3_r[i*8 +: 8] <= {1'b0, bg2_r[i*8 +: 8]}
                                             + quotient_r[i*9 +: 9];
                end
                mask3_r <= mask2_r;
            end
        end

        // stage 2: exact division by 255
        if (ready2) begin
            v2_r <= v1_r;
            if (v1_r) begin
                for (i = 0; i < PIXEL_BYTES; i = i + 1) begin
                    quotient_r[i*9 +: 9] <=
                        (product_r[i*17 +: 17] + 17'd1
                         + (product_r[i*17 +: 17] >> 8)) >> 8;
                end
                neg2_r  <= neg1_r;
                bg2_r   <= bg1_r;
                mask2_r <= mask1_r & alpha_nz1_r;
            end
        end

        // stage 1: one DSP product per RGB byte
        if (ready1) begin
            v1_r <= v0_r;
            if (v0_r) begin
                for (i = 0; i < PIXEL_BYTES; i = i + 1) begin
                    product_r[i*17 +: 17] <=
                        diff_abs_r[i*9 +: 9] * alpha_byte_r[i*8 +: 8];
                end
                neg1_r  <= diff_neg_r;
                bg1_r   <= bg_r;
                mask1_r <= mask_r;
                for (i = 0; i < PIXEL_BYTES; i = i + 1)
                    alpha_nz1_r[i] <= |alpha_byte_r[i*8 +: 8];
            end
        end

        // stage 0: capture one complete paired input; all sideband travels with it
        if (ready0) begin
            v0_r <= in_valid;
            if (in_valid) begin
                for (i = 0; i < PIXEL_BYTES; i = i + 1) begin
                    diff_neg_r[i] <= fg_data[i*8 +: 8] < bg_data[i*8 +: 8];
                    diff_abs_r[i*9 +: 9] <=
                        (fg_data[i*8 +: 8] >= bg_data[i*8 +: 8])
                        ? ({1'b0, fg_data[i*8 +: 8]} - {1'b0, bg_data[i*8 +: 8]})
                        : ({1'b0, bg_data[i*8 +: 8]} - {1'b0, fg_data[i*8 +: 8]});
                    alpha_byte_r[i*8 +: 8] <=
                        fg_alpha[(i/BYTES_PER_PIXEL)*ALPHA_WIDTH +: ALPHA_WIDTH];
                    bg_r[i*8 +: 8] <= bg_data[i*8 +: 8];
                end
                mask_r <= fg_mask;
            end
        end
    end
end

endmodule
