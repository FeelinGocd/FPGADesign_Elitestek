//==============================================================================
//  DDRPacker.v —— 像素流（96 位/拍）拼成 AXI 写拍（128 位/拍）
//------------------------------------------------------------------------------
//  干什么
//      把上游每一拍 4 个像素（12 字节）按序攒起来，攒够 16 字节就往下吐一拍。
//      它是 DDRUnpacker 的镜像：那边是"AXI 读回的一拍 → 拆成像素"，
//      这边是"像素 → 拼成 AXI 的一拍"。
//
//  为什么必须跨拍拼
//      AXI 一拍 16 字节，而像素流一拍必须是整数个 RGB888 像素（12 字节 = 4 像素）；
//      16 不是 3 的倍数，两边的"拍"永远对不齐 ⇒ 只能用字节累加器攒着跨拍拼。
//
//  ★★ "拍对齐"归这里（2026-10 改，见 docs/spec/硬件_加速器.md §4.3）
//      写侧的字节流是从【目的 16 字节对齐基址】开始的，开头 dst_off = dst_addr[3:0]
//      个字节会被 WSTRB 遮掉 ⇒ 流的第 dst_off 个字节必须是本行第 0 个像素的 R，
//      也就是"流前面要补 dst_off 个哑字节"。
//      这件事原来是【每个源各自做】的（PaletteDecoder 的 36 字节窗口、DDRUnpacker 的
//      丢头/补头），结果下游拿到的"一拍 12 字节"不是整像素（dst_off 不是 3 的倍数时
//      跨 5 个像素），alpha 这种"一个像素一个"的属性配不上。
//      现在统一挪到这一级：**行首把字节计数预置成 dst_off**（那几个哑字节的掩码是 0，
//      本来就不会写进 DDR）⇒ 源侧只管"像素对齐"，打包级只管"拍对齐"。
//
//  行边界上要丢掉尾巴
//      ★ 源那一侧为了相位对齐会【多读】几个字节（最多 15 字节），
//        写侧只按"本行该有的字节数"取走该取的拍，多出来的这一小截会留在兜里。
//        下一行的 line_start 一到就把兜清空 —— 否则上一行的尾巴会拼到下一行头上，
//        后面整行的字节相位全错。
//      ★ 清兜必须是安全的：行任务来的时候，上一行该吐的拍【必须已经全被写侧取走】,
//        兜里剩的才只可能是多读的尾巴。这条由 GPU_2D 保证 ——
//        line_buffer_free = 各源 free & WriteController.wr_free：
//          · 各源 free ⟹ 它的 ping-pong 那块已被取空（数据都进了本兜）
//          · 写侧 wr_free ⟹ 上一行连 B 都回来了（该吐的拍全吐出去了）
//        两条缺一不可。少了写侧那一条，行任务会赶在本行吐完之前到，
//        这里一清就把【本行还没吐出去的正经数据】丢掉（实测：整段偏移 4 字节）。
//
//  为什么兜是 32 字节（不是"一拍输入 + 一拍输出"就够）
//      写侧一笔一笔走 AW→W→B，会被 B 响应顶住；顶住期间兜只进不出。
//      ★ 兜一旦收不下，上游（源）就跟着停 —— 源停住就换不回块，
//        CmdExec 再也发不出下一行（整条链死锁）。所以留一整个输出的余量：
//        收进最多 16 + 12 = 28 字节，兜 32 字节，怎么都不会满。
//
//  满/空怎么判
//      收：兜里放得下一整拍输入就收（cnt_byte <= OUT_BYTES）
//      吐：兜里够一整拍输出就吐（cnt_byte >= OUT_BYTES）
//   ⇒ 两者只在 cnt_byte == OUT_BYTES 这一拍重叠，净变化 = 一拍输入 - 一拍输出 = -4 字节
//
//  参考：pixel/DDR/DDRUnpacker.v（同一套字节累加器，方向相反）
//        LineBuffer/bufferUnpacker.v（最早那一版）//  上一级是谁
//      factory/Factory.v

//==============================================================================
`timescale 1ns/100ps

module DDRPacker #(
    parameter IN_WIDTH  = 96,                       // 上游一拍（4 个 RGB888 像素）
    parameter OUT_WIDTH = 128                       // 下游一拍（AXI 16 字节）
)(
    input  wire                     clk,
    input  wire                     rst_n,

    //==================== 行边界（CmdExec 广播，各下游各取所需）  ====================
    input  wire                     line_start,     //(I) 单拍脉冲：清掉上一行多读回来的尾巴
    input  wire [3:0]               line_dst_off,   //(I) 目的地址低 4 位：行首预置几个哑字节

    //==================== 数据入口（接 Compositor）====================
    input  wire [IN_WIDTH-1:0]      pixel_data,
    input  wire [IN_WIDTH/8-1:0]    pixel_mask,        //(I) 每字节要不要写（ColorKey 给的，1=写）

    input  wire                     pixel_valid,
    output wire                     pixel_ready,

    //==================== 数据出口（接 WriteController）====================
    output wire [OUT_WIDTH-1:0]     packer_data,
    output wire [OUT_WIDTH/8-1:0]   packer_mask,       //(O) 拼好的一拍掩码（→ wr_strb 的一个与项）
    output wire                     packer_valid,
    input  wire                     packer_ready
);

//==============================================================================
// 参数换算
//==============================================================================
localparam  IN_BYTES    = IN_WIDTH / 8;                 // 一拍输入几字节（12）
localparam  OUT_BYTES   = OUT_WIDTH / 8;                // 一拍输出几字节（16）
localparam  BUF_BYTES   = 2 * OUT_BYTES;                // 字节兜总长（32）
localparam  BUF_WIDTH   = BUF_BYTES * 8;                // 256
localparam  CNT_WIDTH   = 6;                            // 字节计数（<= 28）

//==============================================================================
// wire / reg
//==============================================================================
reg  [BUF_WIDTH-1:0]    byte_buffer;                    // 字节兜（低位 = 最老的字节）
reg  [BUF_BYTES-1:0]    mask_buffer;                    // 掩码兜：1 位/字节，和数据兜【同进同出】

reg  [CNT_WIDTH-1:0]    cnt_byte;                       // 兜里有效字节数
reg  [BUF_WIDTH-1:0]    next_buffer;                    // 新一拍拼进来之后的样子
reg  [BUF_BYTES-1:0]    next_mask;                      // 新一拍掩码拼进来之后的样子

//==============================================================================
// 判断组合条件
//==============================================================================
wire    accept;                                         // 兜里放得下一整拍输入
wire    in_hs;                                          // 这一拍像素收下
wire    emit;                                           // 兜里够一拍输出
wire    do_emit;                                        // 这一拍真的吐出去了（★ 必须握手）

assign  accept  = (cnt_byte <= OUT_BYTES[CNT_WIDTH-1:0]);
assign  in_hs   = pixel_valid & accept;

assign  emit    = (cnt_byte >= OUT_BYTES[CNT_WIDTH-1:0]);
assign  do_emit = emit & packer_ready;

//==============================================================================
// 组合输出
//==============================================================================
assign  pixel_ready  = accept;
assign  packer_data  = byte_buffer[OUT_WIDTH-1:0];         // 低位 = 最老的字节
//   ★ 掩码走【同一套】移位：同一个 cnt_byte、同一个 line_start 清空、同一个 >>16 字节。
//     数据和掩码在物理上就不可能错位 —— 这是整个 ck 最容易错的地方，所以不另算一套。
assign  packer_mask  = mask_buffer[OUT_BYTES-1:0];

assign  packer_valid = emit;

//==============================================================================
// 拼接：新到的一拍接到兜里已有字节的【上面】（纯拼接，没有移位器）
//   ★ 只在 cnt_byte <= 16 时收，所以 cnt_byte 最大 16，17 路就够
//==============================================================================
//   ★ 掩码那一行是数据那一行的【逐字节镜像】：数据补 0 的地方掩码补 1（那些字节
//     还没攒够，永远不会被吐出去），移位量、切片宽度一模一样。
always @(*) begin
    case (cnt_byte)
6'd0 :  begin next_buffer = { {160{1'b0}}, pixel_data };
              next_mask   = { { 20{1'b1}}, pixel_mask }; end
6'd1 :  begin next_buffer = { {152{1'b0}}, pixel_data, byte_buffer[  7:0] };
              next_mask   = { { 19{1'b1}}, pixel_mask, mask_buffer[ 0:0] }; end
6'd2 :  begin next_buffer = { {144{1'b0}}, pixel_data, byte_buffer[ 15:0] };
              next_mask   = { { 18{1'b1}}, pixel_mask, mask_buffer[ 1:0] }; end
6'd3 :  begin next_buffer = { {136{1'b0}}, pixel_data, byte_buffer[ 23:0] };
              next_mask   = { { 17{1'b1}}, pixel_mask, mask_buffer[ 2:0] }; end
6'd4 :  begin next_buffer = { {128{1'b0}}, pixel_data, byte_buffer[ 31:0] };
              next_mask   = { { 16{1'b1}}, pixel_mask, mask_buffer[ 3:0] }; end
6'd5 :  begin next_buffer = { {120{1'b0}}, pixel_data, byte_buffer[ 39:0] };
              next_mask   = { { 15{1'b1}}, pixel_mask, mask_buffer[ 4:0] }; end
6'd6 :  begin next_buffer = { {112{1'b0}}, pixel_data, byte_buffer[ 47:0] };
              next_mask   = { { 14{1'b1}}, pixel_mask, mask_buffer[ 5:0] }; end
6'd7 :  begin next_buffer = { {104{1'b0}}, pixel_data, byte_buffer[ 55:0] };
              next_mask   = { { 13{1'b1}}, pixel_mask, mask_buffer[ 6:0] }; end
6'd8 :  begin next_buffer = { { 96{1'b0}}, pixel_data, byte_buffer[ 63:0] };
              next_mask   = { { 12{1'b1}}, pixel_mask, mask_buffer[ 7:0] }; end
6'd9 :  begin next_buffer = { { 88{1'b0}}, pixel_data, byte_buffer[ 71:0] };
              next_mask   = { { 11{1'b1}}, pixel_mask, mask_buffer[ 8:0] }; end
6'd10:  begin next_buffer = { { 80{1'b0}}, pixel_data, byte_buffer[ 79:0] };
              next_mask   = { { 10{1'b1}}, pixel_mask, mask_buffer[ 9:0] }; end
6'd11:  begin next_buffer = { { 72{1'b0}}, pixel_data, byte_buffer[ 87:0] };
              next_mask   = { {  9{1'b1}}, pixel_mask, mask_buffer[10:0] }; end
6'd12:  begin next_buffer = { { 64{1'b0}}, pixel_data, byte_buffer[ 95:0] };
              next_mask   = { {  8{1'b1}}, pixel_mask, mask_buffer[11:0] }; end
6'd13:  begin next_buffer = { { 56{1'b0}}, pixel_data, byte_buffer[103:0] };
              next_mask   = { {  7{1'b1}}, pixel_mask, mask_buffer[12:0] }; end
6'd14:  begin next_buffer = { { 48{1'b0}}, pixel_data, byte_buffer[111:0] };
              next_mask   = { {  6{1'b1}}, pixel_mask, mask_buffer[13:0] }; end
6'd15:  begin next_buffer = { { 40{1'b0}}, pixel_data, byte_buffer[119:0] };
              next_mask   = { {  5{1'b1}}, pixel_mask, mask_buffer[14:0] }; end
default:begin next_buffer = { { 32{1'b0}}, pixel_data, byte_buffer[127:0] };
              next_mask   = { {  4{1'b1}}, pixel_mask, mask_buffer[15:0] }; end    // cnt_byte = 16
    endcase
end

//==============================================================================
// 时序逻辑
//==============================================================================
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        byte_buffer <= {BUF_WIDTH{1'b0}};
        mask_buffer <= {BUF_BYTES{1'b0}};
        cnt_byte    <= {CNT_WIDTH{1'b0}};

    end
    else begin
        // ---- 行边界：丢掉上一行多读回来的尾巴 ----
        //   ★ 行任务来的时候上一行该吐的拍已经全被写侧取走了（见文件头），
        //     所以这里直接清，不会丢有效数据
        if (line_start) begin
            byte_buffer <= {BUF_WIDTH{1'b0}};
            mask_buffer <= {BUF_BYTES{1'b0}};              // 和数据兜同拍清
            //   ★★ 预置 dst_off：等于"流前面已经有 dst_off 个哑字节"——
            //      它们的掩码是 0（上面刚清零），所以一个字节都不会写进 DDR；
            //      数据值也无所谓（原来是 0）。这就是"补 dst_off 个哑字节"，
            //      只是不真塞字节，而是把计数摆过去 —— 和 DDRUnpacker 补头同一个手法。
            cnt_byte    <= {1'b0, line_dst_off};

        end
        else begin
            if (in_hs & do_emit) begin
                byte_buffer <= next_buffer >> (OUT_BYTES*8);                // 拼完顺手吐
                mask_buffer <= next_mask   >> OUT_BYTES;
                cnt_byte    <= cnt_byte + IN_BYTES[CNT_WIDTH-1:0]
                                       - OUT_BYTES[CNT_WIDTH-1:0];          // 净减 4
            end
            else if (in_hs) begin
                byte_buffer <= next_buffer;                                 // 只收
                mask_buffer <= next_mask;
                cnt_byte    <= cnt_byte + IN_BYTES[CNT_WIDTH-1:0];
            end
            else if (do_emit) begin
                byte_buffer <= byte_buffer >> (OUT_BYTES*8);                // 只吐
                mask_buffer <= mask_buffer >> OUT_BYTES;
                cnt_byte    <= cnt_byte - OUT_BYTES[CNT_WIDTH-1:0];
            end
        end
    end
end

endmodule
