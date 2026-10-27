//==============================================================================
//  DDRUnpacker.v —— 128bit 拍流 → 每拍 4 个 RGB888 像素
//------------------------------------------------------------------------------
//  干什么
//      上游（DDRSource 的 ping-pong 读口）吐的是"一拍 = 16 字节"的原始字节流，
//      本模块把它按 3 字节一像素切开，**每拍吐 4 个像素（12 字节）**往下游送。
//
//  为什么需要它（这条是 AXI 位宽和像素宽度打架，躲不掉）
//      16 字节 ≠ 3 字节的整数倍（16 = 5⅓ 个像素），拍边界和像素边界天然错位。
//      所以必须跨拍拼接 —— 办法是"来多少字节就按序吐多少像素"，用字节累加器兜着。
//      ★ 一行读回来一定【是整数个像素】，错位的只是"拍"（见 docs/spec/硬件_加速器.md §4）
//
//  为什么输出是 4 个像素/拍，而不是 1 个
//      一拍输入 16 字节 @100MHz = 1.6 GB/s；
//      一拍输出 1 个像素（3 字节）= 0.3 GB/s ✗ 跟不上，累加器会一直涨。
//      4 个像素 = 12 字节/拍 = 1.2 GB/s ✓ 跟得上（全屏一行 5760 字节：
//      进 360 拍 = 3.6us，出 1920/4 = 480 拍 = 4.8us，远小于 22.2us 的行周期，够用）
//
//  阈值为什么是 12（★ 不能照抄显示侧那个 8）
//      显示侧 bufferUnpacker 是"输出远慢于输入"（3 字节/拍 vs 16 字节/拍），
//      累加器长期在慢慢降，判据是 THRESH-2-OUT >= OUT → 8。
//      我们是"输出 12 / 输入 16"——累加器是【净涨】的，判据完全不同：
//          收到 THRESH 就申请，数据两拍后进兜，这期间兜里还在被吐
//          → 兜的上界 = THRESH + IN_BYTES = 28 字节
//          → 要保证"申请期间兜里始终 ≥ 一次输出(12)"，取 THRESH = 12
//      所以累加器 = 28 字节 = 224 位，拼接 case 只需 0..12（13 路）。
//
//  丢头（对齐到【源块自己的字节 0】，shift = src_off，0 <= shift <= 15）
//      读流第 i 字节 = 内存 src_aligned + i；源块的字节 0 在 src_aligned + src_off 处
//      ⇒ 出口流第 j 字节 = 读流第 j + shift 字节 ⇒ 开头丢掉 shift 个读回字节
//        （★ 丢字节，不是移位器）
//      ★ "对齐到目的 16 字节拍"【不在这里】—— 那是打包级的事（DDRPacker 行首把字节计数
//        预置成 dst_off），见 docs/spec/硬件_加速器.md §4.3。本模块只保证出口流是【整数个像素】。
//      ★ shift 恒是 3 的倍数（源地址要求像素对齐），所以丢的都是整数像素。
//
//  接口契约
//      · 标准 valid/ready（不是显示侧那种"早一拍请求"——那是 HDMI 的私有约定）
//      · 行上下文（shift）只在 line_start 那一拍有效，本模块自己锁存
//
//  参考：LineBuffer/bufferUnpacker.v（同一套累加器写法，契约和粒度按本场合改）
//        docs/spec/硬件_加速器.md §4//  上一级是谁
//      pixel/DDR/DDRSource.v —— 源① 的读回数据展开在这一级

//==============================================================================
`timescale 1ns/100ps

module DDRUnpacker #(
    parameter IN_WIDTH    = 128,                    // 输入一拍位宽（8 的整数倍）
    parameter PIXEL_BYTES   = 3,                      // 一个像素几字节（RGB888 = 3）
    parameter PIXELS_PER_OUT = 4,                      // 每拍吐几个像素
    parameter THRESH      = 12,                     // 申请阈值（字节），推导见文件头
    parameter OUT_WIDTH   = PIXELS_PER_OUT * PIXEL_BYTES * 8   // 输出位宽（派生：96）
)(
    input  wire                         clk,
    input  wire                         rst_n,

    //==================== 输入：一拍字节流（接 ping-pong 读口）  ← AXIMastersHandler ← Axi_Mux  ====================
    input  wire [IN_WIDTH-1:0]          in_data,
    input  wire                         in_valid,
    output wire                         in_ready,

    //==================== 行上下文（line_start 那一拍有效）====================
    input  wire                         line_start,
    input  wire [4:0]                   shift,      // src_off - dst_off（有符号，见文件头）
    input  wire                         flush,      //(I) 本行读完了：把兜里不足一拍的余数补零吐出来

    //==================== 输出：每拍 PIXELS_PER_OUT 个像素 ====================
    output wire [OUT_WIDTH-1:0]         out_data,
    output wire                         out_valid,
    input  wire                         out_ready,
    output wire                         empty           //(O) 兜里一个字节都没有（flush 收尾用）
);

//==============================================================================
// 参数换算
//==============================================================================
localparam  IN_BYTES    = IN_WIDTH/8;                                       // 一拍几字节（16）
localparam  OUT_BYTES   = PIXEL_BYTES * PIXELS_PER_OUT;                          // 一次吐几字节（12）
localparam  BUF_BYTES   = THRESH + IN_BYTES;                                // 累加器总长（28）
localparam  BUF_WIDTH   = BUF_BYTES * 8;                                    // 224
localparam  CNT_WIDTH   = 6;                                                // 字节计数（<=28）

//==============================================================================
// localparam
//==============================================================================
localparam  S_ALIGN = 1'b0,     // 先把 shift>0 那几个前导字节丢掉
            S_RUN   = 1'b1;     // 正常：收一拍拼进去、够一次就吐一次

//==============================================================================
// wire / reg
//==============================================================================
reg                         state;
reg  [BUF_WIDTH-1:0]        byte_buffer;        // 字节兜（低位 = 最老的字节）
reg  [CNT_WIDTH-1:0]        cnt_byte;           // 兜里有效字节数
reg  [BUF_WIDTH-1:0]        next_buffer;        // 新一拍拼进来之后的样子
reg  [4:0]                  drop_cnt_r;         // 还要丢几个前导字节
//   吐出一次之后兜里还剩几字节：★ 饱和减法，不足一次就清零 ——
//   否则 flush 时（兜里 < 一次输出）会下溢成一个巨大的数，empty 永远不为 1
wire [CNT_WIDTH-1:0]        cnt_after_take = (cnt_byte >= OUT_BYTES)
                                           ? (cnt_byte - OUT_BYTES)
                                           : {CNT_WIDTH{1'b0}};

//==============================================================================
// 判断组合条件
//==============================================================================
//   ★ 本模块的输入是"数据随 valid 直接来"（不是显示侧那种"先申请、一拍后才到"），
//     所以没有"在途"这回事 —— 只要兜里还有位置就收
//   ★ 丢头期间也要能收数：兜里没货就收一拍进来，下一拍再丢掉最老的字节
//     （收和丢不能同一拍做，会打架 —— 所以"有货就丢、没货才收"交替进行）
assign  in_ready    = (cnt_byte <= THRESH)
                    & ((state == S_RUN) | (cnt_byte == {CNT_WIDTH{1'b0}}));

//   ★ 收数的唯一凭据是【握手】：in_valid 是上游的电平，我们拉低 in_ready 时
//     上游不会前进、in_valid 会一直是 1 —— 只看 in_valid 就会把同一拍反复收进来，
//     兜里涨到溢出、in_ready 再也回不来（这个坑实测踩过：水位冲到 60，S_RUN 慢 10 倍）
wire    in_hs       = in_valid & in_ready;
//   正常：兜里够一次就吐        收尾：flush 拉高时，兜里只要还有字节就补零吐一拍
assign  out_valid   = (state == S_RUN) & ((cnt_byte >= OUT_BYTES)
                                        | (flush & (cnt_byte != {CNT_WIDTH{1'b0}})));
//   低位 = 最老的字节；flush 时不足一拍的余数自然落在高位、为 0（写侧会按行宽丢掉）
assign  out_data    = byte_buffer[OUT_WIDTH-1:0];
assign  empty       = (cnt_byte == {CNT_WIDTH{1'b0}});

//==============================================================================
// 拼接：新到的一拍接到兜里已有字节的【上面】（纯拼接，没有移位器）
//   ★ 只在 cnt_byte <= THRESH 时收，所以 cnt_byte 最大 THRESH，13 路就够
//==============================================================================
always @(*) begin
    case (cnt_byte[4:0])
        5'd0 :  next_buffer = { {(BUF_WIDTH-IN_WIDTH){1'b0}}, in_data };
        5'd1 :  next_buffer = { {(BUF_WIDTH-IN_WIDTH-8){1'b0}},  in_data, byte_buffer[  7:0] };
        5'd2 :  next_buffer = { {(BUF_WIDTH-IN_WIDTH-16){1'b0}}, in_data, byte_buffer[ 15:0] };
        5'd3 :  next_buffer = { {(BUF_WIDTH-IN_WIDTH-24){1'b0}}, in_data, byte_buffer[ 23:0] };
        5'd4 :  next_buffer = { {(BUF_WIDTH-IN_WIDTH-32){1'b0}}, in_data, byte_buffer[ 31:0] };
        5'd5 :  next_buffer = { {(BUF_WIDTH-IN_WIDTH-40){1'b0}}, in_data, byte_buffer[ 39:0] };
        5'd6 :  next_buffer = { {(BUF_WIDTH-IN_WIDTH-48){1'b0}}, in_data, byte_buffer[ 47:0] };
        5'd7 :  next_buffer = { {(BUF_WIDTH-IN_WIDTH-56){1'b0}}, in_data, byte_buffer[ 55:0] };
        5'd8 :  next_buffer = { {(BUF_WIDTH-IN_WIDTH-64){1'b0}}, in_data, byte_buffer[ 63:0] };
        5'd9 :  next_buffer = { {(BUF_WIDTH-IN_WIDTH-72){1'b0}}, in_data, byte_buffer[ 71:0] };
        5'd10:  next_buffer = { {(BUF_WIDTH-IN_WIDTH-80){1'b0}}, in_data, byte_buffer[ 79:0] };
        5'd11:  next_buffer = { {(BUF_WIDTH-IN_WIDTH-88){1'b0}}, in_data, byte_buffer[ 87:0] };
        default:next_buffer = { in_data, byte_buffer[ 95:0] };              // cnt_byte = 12
    endcase
end

//==============================================================================
// 时序逻辑
//==============================================================================
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        state       <= S_ALIGN;
        byte_buffer <= {BUF_WIDTH{1'b0}};
        cnt_byte    <= {CNT_WIDTH{1'b0}};
        drop_cnt_r  <= 5'd0;
    end
    else begin
        // ---- 行开始：按 shift 决定"丢头"还是"补头" ----
        if (line_start) begin
            byte_buffer <= {BUF_WIDTH{1'b0}};
            if (shift[4]) begin                     // shift < 0（补头）
                //  补 |shift| 个零字节：放在兜的最低位 —— 它们落在目的遮头区，
                //  值无所谓；后面收进来的真实字节会拼在它们【上面】
                cnt_byte   <= {1'b0, ~shift[3:0]} + 6'd1;
                drop_cnt_r <= 5'd0;
                state      <= S_RUN;
            end
            else if (shift[3:0] != 4'd0) begin      // shift > 0（丢头）
                cnt_byte   <= {CNT_WIDTH{1'b0}};
                drop_cnt_r <= {1'b0, shift[3:0]};
                state      <= S_ALIGN;
            end
            else begin                              // shift = 0
                cnt_byte   <= {CNT_WIDTH{1'b0}};
                drop_cnt_r <= 5'd0;
                state      <= S_RUN;
            end
        end
        else begin
            case (state)
                // ---- 丢头：每拍丢一个最老的字节，丢完就走 ----
                S_ALIGN : if (drop_cnt_r == 5'd0) begin
                              state <= S_RUN;                               // 丢够了，转正常
                          end
                          else if (cnt_byte != {CNT_WIDTH{1'b0}}) begin
                              byte_buffer <= byte_buffer >> 8;              // 兜里有货 → 丢掉最老的
                              cnt_byte    <= cnt_byte - 6'd1;
                              drop_cnt_r  <= drop_cnt_r - 5'd1;
                          end
                          else if (in_hs) begin
                              byte_buffer <= next_buffer;                   // 兜里空 → 先收一拍
                              cnt_byte    <= cnt_byte + IN_BYTES;
                          end

                // ---- 正常：数据回来就拼；够一次就吐一次 ----
                S_RUN : begin
                            if (in_hs) begin
                                if (out_valid & out_ready) begin
                                    byte_buffer <= next_buffer >> (OUT_BYTES*8);    // 拼完顺手吐
                                    cnt_byte    <= cnt_after_take + IN_BYTES;
                                end
                                else begin
                                    byte_buffer <= next_buffer;                     // 只收下
                                    cnt_byte    <= cnt_byte + IN_BYTES;
                                end
                            end
                            else if (out_valid & out_ready) begin
                                byte_buffer <= byte_buffer >> (OUT_BYTES*8);        // 只吐
                                cnt_byte    <= cnt_after_take;                      // ★ 饱和，不下溢
                            end
                        end

                default: ;
            endcase
        end
    end
end

endmodule
