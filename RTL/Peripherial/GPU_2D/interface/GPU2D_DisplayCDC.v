// GPU2D_DisplayCDC.v -- frame-boundary request/reply between memory and GPU.
// The raw HDMI frame event requests a flip in the GPU/APB domain. One edge
// AFTER the flip, latch its mode/address and acknowledge the request. The
// memory domain accepts this held bundle before releasing first-line prefetch.
// At 720p there are 607us of blanking; the handshake consumes under 0.2us.
// Resets assert together; held payload stays unchanged until the next frame.
// Reference: lineBuffer/bufferControler.v, frame-start/base-address contract.
module GPU2D_DisplayCDC #(
    parameter ADDR_WIDTH = 32
)(
    input wire clk_acc, input wire rst_acc_n,
    input wire clk_mem, input wire rst_mem_n,
    input wire mem_frame_start,
    output wire acc_frame_start,
    input wire acc_enable,
    input wire acc_mode,
    input wire [2*ADDR_WIDTH-1:0] acc_addr,
    output wire mem_enable,
    output wire mem_mode,
    output wire [2*ADDR_WIDTH-1:0] mem_addr,
    output wire mem_frame_commit
);
reg request_r;
(* async_reg = "true" *) reg request_meta_r, request_sync_r;
reg request_seen_r;
reg capture_r;
reg acknowledge_r;
reg [2*ADDR_WIDTH:0] payload_r;
(* async_reg = "true" *) reg ack_meta_r, ack_sync_r;
reg ack_seen_r;
(* async_reg = "true" *) reg enable_meta_r, enable_sync_r;
reg [2*ADDR_WIDTH:0] config_r;
reg commit_r;
assign acc_frame_start = request_sync_r ^ request_seen_r;
assign mem_enable = enable_sync_r;
assign {mem_mode,mem_addr} = config_r;
assign mem_frame_commit = commit_r;
always @(posedge clk_mem or negedge rst_mem_n) begin
    if (!rst_mem_n) begin
        request_r <= 1'b0;
        ack_meta_r <= 1'b0;
        ack_sync_r <= 1'b0;
        ack_seen_r <= 1'b0;
        enable_meta_r <= 1'b0;
        enable_sync_r <= 1'b0;
        config_r <= 0;
        commit_r <= 1'b0;
    end else begin
        if (mem_frame_start) request_r <= ~request_r;
        ack_meta_r <= acknowledge_r;
        ack_sync_r <= ack_meta_r;
        enable_meta_r <= acc_enable;
        enable_sync_r <= enable_meta_r;
        commit_r <= 1'b0;
        if (ack_sync_r != ack_seen_r) begin
            config_r <= payload_r;
            ack_seen_r <= ack_sync_r;
            commit_r <= 1'b1;
        end
    end
end
always @(posedge clk_acc or negedge rst_acc_n) begin
    if (!rst_acc_n) begin
        request_meta_r <= 1'b0;
        request_sync_r <= 1'b0;
        request_seen_r <= 1'b0;
        capture_r <= 1'b0;
        acknowledge_r <= 1'b0;
        payload_r <= 0;
    end else begin
        request_meta_r <= request_r;
        request_sync_r <= request_meta_r;
        request_seen_r <= request_sync_r;
        capture_r <= acc_frame_start;
        if (capture_r) begin
            payload_r <= {acc_mode,acc_addr};
            acknowledge_r <= request_seen_r;
        end
    end
end
endmodule
