// GPU2D_MemoryCDC.v -- GPU commands/data across the accelerator/memory clocks.
// GPU_2D places this before AXIMastersHandler, keeping all AXI channels at 100MHz.
// Command, write data and write completion use independent ordered FIFOs.
// Read last travels WITH its data; completion occurs only when GPU accepts it.
// A write completes only after the memory master receives B, preserving FENCE.
// Reference: docs/spec/硬件_RTL规范.md and AXIMastersHandler interface contract.
module GPU2D_MemoryCDC #(
    parameter ADDR_WIDTH = 32,
    parameter DATA_WIDTH = 128,
    parameter LEN_WIDTH = 8,
    parameter STRB_WIDTH = DATA_WIDTH/8
)(
    input wire clk_acc, input wire rst_acc_n,
    input wire clk_mem, input wire rst_mem_n,
    input wire cmd_wr_valid, output wire cmd_wr_ready,
    input wire [ADDR_WIDTH-1:0] cmd_wr_addr,
    input wire [LEN_WIDTH-1:0] cmd_wr_len,
    output wire cmd_wr_done, output wire cmd_wr_err,
    input wire [DATA_WIDTH-1:0] wr_data,
    input wire [STRB_WIDTH-1:0] wr_strb,
    input wire wr_valid, output wire wr_ready,
    input wire rd_cmd_valid, output wire rd_cmd_ready,
    input wire [ADDR_WIDTH-1:0] rd_cmd_addr,
    input wire [LEN_WIDTH-1:0] rd_cmd_len,
    output wire rd_cmd_done,
    output wire [DATA_WIDTH-1:0] rd_data,
    output wire rd_valid, input wire rd_ready,
    output wire mem_cmd_wr_valid, input wire mem_cmd_wr_ready,
    output wire [ADDR_WIDTH-1:0] mem_cmd_wr_addr,
    output wire [LEN_WIDTH-1:0] mem_cmd_wr_len,
    input wire mem_cmd_wr_done, input wire mem_cmd_wr_err,
    output wire [DATA_WIDTH-1:0] mem_wr_data,
    output wire [STRB_WIDTH-1:0] mem_wr_strb,
    output wire mem_wr_valid, input wire mem_wr_ready,
    output wire mem_rd_cmd_valid, input wire mem_rd_cmd_ready,
    output wire [ADDR_WIDTH-1:0] mem_rd_cmd_addr,
    output wire [LEN_WIDTH-1:0] mem_rd_cmd_len,
    input wire [DATA_WIDTH-1:0] mem_rd_data,
    input wire mem_rd_valid, input wire mem_rd_last,
    output wire mem_rd_ready
);
wire rd_last;
wire wr_response_ready;
assign rd_cmd_done = rd_valid & rd_ready & rd_last;
CdcStream #(.WIDTH(ADDR_WIDTH+LEN_WIDTH), .ADDR_WIDTH(2)) u_write_cmd (
    .src_clk(clk_acc), .src_rst_n(rst_acc_n),
    .src_data({cmd_wr_addr,cmd_wr_len}), .src_valid(cmd_wr_valid), .src_ready(cmd_wr_ready),
    .dst_clk(clk_mem), .dst_rst_n(rst_mem_n),
    .dst_data({mem_cmd_wr_addr,mem_cmd_wr_len}), .dst_valid(mem_cmd_wr_valid), .dst_ready(mem_cmd_wr_ready)
);
CdcStream #(.WIDTH(DATA_WIDTH+STRB_WIDTH), .ADDR_WIDTH(3)) u_write_data (
    .src_clk(clk_acc), .src_rst_n(rst_acc_n),
    .src_data({wr_data,wr_strb}), .src_valid(wr_valid), .src_ready(wr_ready),
    .dst_clk(clk_mem), .dst_rst_n(rst_mem_n),
    .dst_data({mem_wr_data,mem_wr_strb}), .dst_valid(mem_wr_valid), .dst_ready(mem_wr_ready)
);
// Factory has a single outstanding write, so a four-entry response queue cannot
// overflow: it cannot launch its next write until this response is consumed.
CdcStream #(.WIDTH(1), .ADDR_WIDTH(2)) u_write_response (
    .src_clk(clk_mem), .src_rst_n(rst_mem_n),
    .src_data(mem_cmd_wr_err), .src_valid(mem_cmd_wr_done), .src_ready(wr_response_ready),
    .dst_clk(clk_acc), .dst_rst_n(rst_acc_n),
    .dst_data(cmd_wr_err), .dst_valid(cmd_wr_done), .dst_ready(1'b1)
);
CdcStream #(.WIDTH(ADDR_WIDTH+LEN_WIDTH), .ADDR_WIDTH(2)) u_read_cmd (
    .src_clk(clk_acc), .src_rst_n(rst_acc_n),
    .src_data({rd_cmd_addr,rd_cmd_len}), .src_valid(rd_cmd_valid), .src_ready(rd_cmd_ready),
    .dst_clk(clk_mem), .dst_rst_n(rst_mem_n),
    .dst_data({mem_rd_cmd_addr,mem_rd_cmd_len}), .dst_valid(mem_rd_cmd_valid), .dst_ready(mem_rd_cmd_ready)
);
CdcStream #(.WIDTH(DATA_WIDTH+1), .ADDR_WIDTH(3)) u_read_data (
    .src_clk(clk_mem), .src_rst_n(rst_mem_n),
    .src_data({mem_rd_last,mem_rd_data}), .src_valid(mem_rd_valid), .src_ready(mem_rd_ready),
    .dst_clk(clk_acc), .dst_rst_n(rst_acc_n),
    .dst_data({rd_last,rd_data}), .dst_valid(rd_valid), .dst_ready(rd_ready)
);
endmodule
