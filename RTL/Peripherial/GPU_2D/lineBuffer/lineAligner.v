//==============================================================================
//  lineAligner.v —— 显示行首非 16B 对齐时的“滑窗”对齐器
//------------------------------------------------------------------------------
//  为什么需要它
//      大房间相机把显示基址写成 FB + cam_y*ROW_STRIDE + cam_x*3。
//      cam_x*3 不一定是 16 的倍数，而 lineBuffer 从 DDR 读一行是按 16B 拍读的。
//      直接把这个地址给 AXI 会读到“对齐后的前 16B”，像素相位就错了。
//
//  它做什么
//      全屏: 输入一行 241 拍（241*16 = 3856B），skip = 行首在 16B 拍内的字节偏移(0..15)。
//      丢掉前 skip 个字节，输出连续 240 拍（240*16 = 3840B = 1280 像素）。
//      等价于：out[k] = in_bytes[skip + 16k .. skip + 16k + 15]。
//      ★ 分屏也走这里：一行只有 240 拍、skip=0 —— 不丢字节，纯直通，块尾同样
//        由 in_block_done 收尾。所以 controller 的两种模式共用这一条通路，
//        下游永远看到"240 拍 + 一次块尾"。
//
//  放在哪
//      bufferControler 之后、async_PingPongBuffer 写口之前（100MHz 写域）。
//      PingPong 读侧仍然只看到 240 拍/行，bufferUnpacker 一行都不用改。
//
//  握手
//      in_valid/in_ready  : 与 controller 的写口握手
//      out_valid/out_ready: 与 PingPong 的写口握手（PingPong 满时反压）
//      in_block_done      : 本行最后一拍（全屏第 241 拍 / 分屏第 240 拍）
//      out_block_done     : 第 240 拍输出，给 PingPong 做块结束
//
//  实现
//      内部只需要“上一拍 16B + 当前拍 16B”的 32B 窗口和一个 16B 桶形移位。
//      skip=0 时当前拍就是输出；skip>0 时输出 = {prev[skip..15], cur[0..skip-1]}。
//      skip>0 的第一拍只存 prev，不出数；因此 241 拍输入正好出 240 拍。
//      skip=0 时 241 拍中的最后一拍是多余拍，丢掉。
//
//  时序
//      100MHz 写域，一行 241 拍 = 2.41us；行周期 22.2us，余量很大。
//      没有 BRAM，没有跨时钟域。
//==============================================================================
`timescale 1ns/100ps

module lineAligner #(
    parameter DIN_WIDTH = 128,      // 一拍多少位（本工程 128 = 16B）
    parameter OUT_BEATS = 240       // 1280 像素 / 4 像素每 16B? 实际 16B = 5⅓ 像素，240 拍 = 3840B
)(
    input  wire                     clk,
    input  wire                     rst_n,
    input  wire                     enable,         // 0 = 普通房间: 持续清空内部状态

    input  wire [3:0]               skip,           // 行首字节偏移 0..15
    input  wire                     in_valid,
    output wire                     in_ready,
    input  wire [DIN_WIDTH-1:0]     in_data,
    input  wire                     in_block_done,  // 输入第 241 拍

    output wire                     out_valid,
    input  wire                     out_ready,
    output wire [DIN_WIDTH-1:0]     out_data,
    output wire                     out_block_done // 输出第 240 拍
);

localparam  CNT_W   = 9;            // 0..240

reg  [DIN_WIDTH-1:0]    prev_data_r;
reg                     have_prev_r;
reg  [CNT_W-1:0]        out_cnt_r;
reg                     out_valid_r;
reg  [DIN_WIDTH-1:0]    out_data_r;
reg                     out_last_r;

//----------------------------------------------------------------------
// 组合：32B 窗口右移 skip 字节
//   {in_data, prev_data_r} 的低 16B = prev，高 16B = cur
//   右移 skip*8 位后，低 16B = prev[skip..15] + cur[0..skip-1]
//----------------------------------------------------------------------
wire [2*DIN_WIDTH-1:0]  join_data   = {in_data, prev_data_r};
wire [2*DIN_WIDTH-1:0]  shifted     = join_data >> {skip, 3'b000};  // 7 位移位量，避免 4 位左移截断
wire [DIN_WIDTH-1:0]    cand_data   = (skip == 4'd0) ? in_data : shifted[DIN_WIDTH-1:0];
wire                    cand_valid  = (skip == 4'd0)
                                    ? (out_cnt_r < OUT_BEATS)
                                    : (have_prev_r && (out_cnt_r < OUT_BEATS));
wire                    cand_last   = (out_cnt_r == (OUT_BEATS - 1));

// 输出槽空（或本拍被取走）才收下一拍
assign  in_ready        = ~out_valid_r | out_ready;
assign  out_valid       = out_valid_r;
assign  out_data        = out_data_r;
assign  out_block_done  = out_valid_r & out_last_r & out_ready;  // 只在实际写入 PingPong 那一拍拉高

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        prev_data_r  <= {DIN_WIDTH{1'b0}};
        have_prev_r  <= 1'b0;
        out_cnt_r    <= {CNT_W{1'b0}};
        out_valid_r  <= 1'b0;
        out_data_r   <= {DIN_WIDTH{1'b0}};
        out_last_r   <= 1'b0;
    end
    else if (!enable) begin
        // 普通房间不走这条通路：把跨拍状态清干净，切入大房间时第一行就是满 240 拍
        prev_data_r  <= {DIN_WIDTH{1'b0}};
        have_prev_r  <= 1'b0;
        out_cnt_r    <= {CNT_W{1'b0}};
        out_valid_r  <= 1'b0;
        out_data_r   <= {DIN_WIDTH{1'b0}};
        out_last_r   <= 1'b0;
    end
    else begin
        // 输出被取走：默认清 valid；若本拍又算出新输出，下面的赋值会覆盖
        if (out_valid_r && out_ready)
            out_valid_r <= 1'b0;

        if (in_valid && in_ready) begin
            // 当前拍进入 prev，供下一拍做跨拍拼接
            prev_data_r <= in_data;
            have_prev_r <= 1'b1;

            if (cand_valid) begin
                out_data_r  <= cand_data;
                out_last_r  <= cand_last;
                out_valid_r <= 1'b1;
            end

            // 第 241 拍：本行结束，清跨拍状态；输出槽里的最后一拍保留到被取走
            if (in_block_done) begin
                have_prev_r <= 1'b0;
                out_cnt_r   <= {CNT_W{1'b0}};
            end
            else if (cand_valid) begin
                out_cnt_r <= out_cnt_r + 1'b1;
            end
        end
    end
end

endmodule
