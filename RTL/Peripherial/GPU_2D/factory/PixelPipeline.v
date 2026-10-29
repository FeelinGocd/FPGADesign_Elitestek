// PixelPipeline.v -- one elastic pixel register between compositor and packer.
// Factory owns this stage. Data and byte mask move on the same handshake.
// line_done discards unused tail pixels; no new data is accepted until line_start.
// Splits image/tint arithmetic from the byte-packing mux in the core clock domain.
// Reference: docs/spec/硬件_RTL规范.md, ready/valid and line-boundary contracts.
module PixelPipeline #(
    parameter WIDTH = 96
)(
    input wire clk, input wire rst_n,
    input wire line_start, input wire line_done,
    input wire [WIDTH-1:0] pixel_data,
    input wire [WIDTH/8-1:0] pixel_mask,
    input wire pixel_valid, output wire pixel_ready,
    output wire [WIDTH-1:0] stage_data,
    output wire [WIDTH/8-1:0] stage_mask,
    output wire stage_valid, input wire stage_ready,
    output wire stage_free
);
reg [WIDTH-1:0] data_r;
reg [WIDTH/8-1:0] mask_r;
reg valid_r, row_done_r;
// line_start/line_done synchronously discard the buffered tail below. Input
// ready can consume another unused tail on that edge, while stage_valid still
// prevents it from reaching the packer. Keep writer completion off input ready.
assign pixel_ready = (~valid_r | stage_ready) & ~row_done_r;
assign stage_data = data_r;
assign stage_mask = mask_r;
assign stage_valid = valid_r & ~line_start & ~line_done;
assign stage_free = ~valid_r;
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        data_r <= 0;
        mask_r <= 0;
        valid_r <= 1'b0;
        row_done_r <= 1'b1;
    end else if (line_start || line_done) begin
        valid_r <= 1'b0;
        row_done_r <= ~line_start;
    end else if (pixel_ready) begin
        valid_r <= pixel_valid;
        if (pixel_valid) begin
            data_r <= pixel_data;
            mask_r <= pixel_mask;
        end
    end
end
endmodule
