// CdcStream.v -- ready/valid wrapper around the existing Gray-pointer async FIFO.
// Used by GPU2D_MemoryCDC; transfers the complete payload and all sidebands.
// The synchronous RAM output is prefetched into one held ready/valid slot.
// Both resets must assert together; each may release on its own clock edge.
// Reference: GPU_2D/cmd/CmdBuffer.v (the same FIFO prefetch contract).
module CdcStream #(
    parameter WIDTH = 32,
    parameter ADDR_WIDTH = 4
)(
    input wire src_clk,
    input wire src_rst_n,
    input wire [WIDTH-1:0] src_data,
    input wire src_valid,
    output wire src_ready,
    input wire dst_clk,
    input wire dst_rst_n,
    output wire [WIDTH-1:0] dst_data,
    output wire dst_valid,
    input wire dst_ready
);
wire fifo_full;
wire fifo_empty;
wire prefetch;
reg valid_r;
assign src_ready = src_rst_n & ~fifo_full;
assign prefetch = dst_rst_n & ~fifo_empty & (~valid_r | dst_ready);
assign dst_valid = dst_rst_n & valid_r;
always @(posedge dst_clk or negedge dst_rst_n) begin
    if (!dst_rst_n) valid_r <= 1'b0;
    else if (prefetch) valid_r <= 1'b1;
    else if (dst_ready) valid_r <= 1'b0;
end
async_fifo #(.WR_WIDTH(WIDTH), .RD_WIDTH(WIDTH), .ADDR_WIDTH(ADDR_WIDTH), .USE_REGISTERS(1)) u_fifo (
    .clk_wr(src_clk), .rst_wr_n(src_rst_n),
    .data_in(src_data), .en_wr(src_valid & src_ready), .wr_full(fifo_full),
    .clk_rd(dst_clk), .rst_rd_n(dst_rst_n),
    .data_out(dst_data), .en_rd(prefetch), .rd_empty(fifo_empty)
);
endmodule
