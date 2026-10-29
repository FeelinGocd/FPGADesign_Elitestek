// ResetSync.v -- asynchronous assertion, two-edge release in the receiving domain.
// Used by GamingPlayUnit_Top for the CPU/GPU and DDR/display clock domains.
// Both ends of a CDC queue must share the same reset assertion source.
// Reference: docs/spec/硬件_RTL规范.md, reset/clock-domain contract.
module ResetSync (
    input wire clk,
    input wire async_rst_n,
    output wire rst_n
);
(* async_reg = "true" *) reg [1:0] release_r;
assign rst_n = release_r[1];
always @(posedge clk or negedge async_rst_n) begin
    if (!async_rst_n) release_r <= 2'b00;
    else release_r <= {release_r[0], 1'b1};
end
endmodule
