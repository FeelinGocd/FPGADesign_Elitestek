//==============================================================================
//  DDRSource.v —— 源①：DDR 读回来 → 拆成像素 → 存好一行
//------------------------------------------------------------------------------
//  定位（这是"一路源"的完整实现，不是"读通路"）
//      收行任务 → 拆笔发 AXI 读 → 把读回的字节流拆成像素（只对齐到源块自己的字节 0）
//      → 写进【自己的】ping-pong（一行像素）→ 交给 Compositor 合成
//
//  链路
//      AXI 读 ──128bit/拍──▶ DDRUnpacker ──96bit/拍(4 像素)──▶ ping-pong ──▶ Compositor
//                              （拆像素 + 丢掉源块前面的 src_off 个字节）
//
//  为什么格式是"拍 → 像素"
//      AXI 一拍 16 字节 = 5⅓ 个 RGB888 像素，拍边界和像素边界天然错位，
//      必须跨拍拼接 —— 这件事交给 DDRUnpacker（它内部是字节累加器）。
//      本模块只负责"什么时候该收几拍、什么时候该收尾"。
//
//  对齐交给谁（★ 只有一件事：对齐到【源块自己的字节 0】）
//      shift = row_addr[3:0] = 源行起始字节在拍内的偏移，丢给 DDRUnpacker 丢头用。
//      ★ "对齐到目的 16 字节拍"【不在这里】—— 那是打包级的事（DDRPacker 行首
//        cnt_byte <= dst_off），见 docs/spec/硬件_加速器.md §4.3。
//      ★ 本模块因此【不再需要】原来那套字节级重排（keep_mask / pre / phase）—— 全删了。
//
//  两根往外/往回的线
//      line_src0_free ：本行已经交完、而且 ping-pong 已经把块换回来 → 可以让 CmdExec 发下一行
//      （合成器按"本行用到哪几路源"相与后，汇成 buf_free 送给 CmdExec）
//
//  参考：docs/spec/硬件_加速器.md §4、LineBuffer/bufferUnpacker.v（同一套累加器写法）//  上一级是谁
//      GPU_2D.v —— 顶层把它的像素流接到 Factory 的源①口

//==============================================================================
`timescale 1ns/100ps

module DDRSource #(
    parameter ADDR_WIDTH = 32,
    parameter AXI_DATA_WIDTH = 128,                     // AXI 一拍
    parameter PIXEL_BYTES  = 3,                       // RGB888
    parameter PIXELS_PER_WORD  = 4,                       // ping-pong 一个字装 4 个像素
    parameter BANK_WIDTH     = PIXEL_BYTES * PIXELS_PER_WORD * 8,   // 96
    parameter BANK_DEPTH = 512,                     // 一块的深度（像素字）
    parameter LEN_WIDTH  = 8
)(
    //==================== 时钟 / 复位 ====================
    input  wire                     clk,
    input  wire                     rst_n,

    //==================== 行任务入口（接 CmdExec 的广播）  ====================
    input  wire                     line_start,
    input  wire [31:0]              line_src_addr,
    input  wire [31:0]              line_dst_addr,  //(I) 目的行地址（算相位；alpha 时它就是读数地址）
    input  wire [7:0]               line_flags,     //(I) 合成模式（整个命令期间稳定）—— 我自己判断要不要动
    input  wire [15:0]              line_w,

    //==================== 像素流出口（标准 valid/ready）→ Compositor ====================
    //   ★ 对外的契约是【标准 valid/ready】：valid 拉高就得一直保持到握手。
    //     ping-pong 读口那种"早一拍请求"的私有约定关在本模块内部（见下面的 skid）
    input  wire                     pixel_rd_en,      //(I) Compositor 取走一拍像素
    output wire [BANK_WIDTH-1:0]        pixel_rd_data,    //(O) 一拍 = PIXELS_PER_WORD 个像素
    output wire                     pixel_rd_valid,   //(O) 手上有货（保持到 pixel_rd_en 握手）
    output wire                     line_src0_free,       //(O) 我能接下一行

    //==================== AXI 读命令口（接 AXIMastersHandler）  ====================
    output wire                     rd_cmd_valid,
    input  wire                     rd_cmd_ready,
    output wire [ADDR_WIDTH-1:0]    rd_cmd_addr,
    output wire [LEN_WIDTH-1:0]     rd_cmd_len,     // 拍数 - 1
    input  wire                     rd_cmd_done,    //(I) 本笔收完
    input  wire [AXI_DATA_WIDTH-1:0]    rd_data,
    input  wire                     rd_valid,
    output wire                     rd_ready
);

//==============================================================================
// localparam
//==============================================================================
localparam  OUT_BYTES = PIXEL_BYTES * PIXELS_PER_WORD;      // 一拍像素几字节（12）

localparam  S_IDLE = 2'd0,      // 等行任务
            S_CMD  = 2'd1,      // 发一笔读命令
            S_RUN  = 2'd2,      // 收这一笔 / 持续喂 unpacker
            S_WAIT = 2'd3;      // 收尾：flush + 交块 + 等 ping-pong 换回

//==============================================================================
// 函数
//==============================================================================
// ---- 拆笔：纯加法/移位/比较，没有乘法器 ----
//   ★ addr 的低 4 位必须先清掉再算页内余量：不清的话页尾不足 16 字节时整除得 0，
//     本笔 0 拍。写侧那次 0 拍让 cmd_len 下溢成 255，把 Axi_Mux 的 W 通道锁死（§9.1）
function [13:0] burst_of;
    input [31:0] addr;
    input [13:0] left;
    reg   [13:0] to_page;
    begin
        to_page = 14'd4096 - {2'b0, addr[11:4], 4'b0000};
        to_page = to_page >> 4;
        if (to_page > 14'd256) to_page = 14'd256;
        if (left < to_page)    to_page = left;
        burst_of = to_page;
    end
endfunction

//==============================================================================
// wire / reg
//==============================================================================
// ---- 状态机 ----
reg  [1:0]      state;
reg  [1:0]      next_state;

// ---- 拆笔进度 ----
reg  [31:0]     rd_addr_r;                     // 本笔起始地址（命令期间必须稳定）
reg  [13:0]     rd_burst_r;                    // 本笔拍数（命令期间必须稳定）
reg  [13:0]     rd_left_r;                   // 这一行还剩几笔没发

// ---- 收数 / 喂 unpacker ----
reg  [13:0]     rd_beats_r;                     // 本行一共要收几拍
reg  [13:0]     rd_cnt_r;                       // 已收几拍
reg             unpack_flush_r;                        // 收尾：让 unpacker 把余数吐出来

// ---- ping-pong 交块 ----
//   ★ 不数"该写几拍"：读了 flush 之后，unpacker 兜里吐空（empty）就说明这一行完了。
//     这样既不需要常量除法，也不会把尾部那几个有效像素漏掉。
reg             bank_hand_r;                         // 本行的块已经交出去了（电平，给状态机判出口）
reg             bank_done_r;                         // ★ 交块那一拍的单拍脉冲（给 ping-pong 的 done）
reg             bank_low_r;                      // 已经看到 ping-pong 换块（wr_ready 掉过）

// ---- 读口一级 skid（把 ping-pong 的"早一拍请求"变成标准 valid/ready）----
//   ★ async_PingPongBuffer 的读口是【早一拍请求】约定：buffer_rd_en 拉高，
//     【下一拍】才给 buffer_rd_valid + 数据（BRAM 时序输出 + valid 也延了一拍）。
//     而下游（合成器/DDRPacker）用的是标准 valid/ready —— 它只在"兜里放得下"时才收。
//     两者直接对接：一旦下游反压，那一拍请求回来的数据没人接，就【永久丢了】。
//     （实测：每 4 个字丢 1 个 —— 12 入/16 出刚好 48 字节一圈）
//   ⇒ 在这里兜一拍：手上没货就提前请求，请求回来的先存住，下游什么时候要什么时候给。
//     于是本模块对外的 pixel_rd_valid/pixel_rd_en 就是【标准 valid/ready】。
reg  [BANK_WIDTH-1:0]       pixel_hold_data;              // 兜住的那一拍像素
reg                     pixel_hold_r;                 // 手上有货

// ---- 子模块之间的连线 ----
wire                    unpack_in_ready;               // unpacker 还能收一拍
wire [BANK_WIDTH-1:0]       unpack_out_data;               // unpacker 吐出的像素拍
wire                    unpack_out_valid;
wire                    unpack_out_ready;
wire                    unpack_empty;                  // unpacker 兜里空了
wire                    bank_wr_ready;                // ping-pong：本块还能收
wire [BANK_WIDTH-1:0]       bank_rd_data;                 // ping-pong：读口
wire                    bank_rd_valid;                // ping-pong：请求回来的数据到了
wire                    bank_rd_en;                   // ping-pong：这一拍提前请求
wire                    bank_rd_block_ready;          // ping-pong：当前读块非空
wire                    bank_has_data;                // = 上面那根（本模块内部起的名）
wire                    pixel_take;                     // 下游这一拍真的取走了
wire                    pixel_hold_next;                // 这一拍结束时兜里还有没有货

// ---- 判断组合条件 ----
wire            take_task;
wire            last_burst;
wire            unpack_take;                      // 这一拍读回数据要收下
wire [4:0]      shift;

//==============================================================================
// assign 区：行开始时的一次性算术
//==============================================================================
//   相位：读流第 i 字节 ↔ 内存 src_aligned + i
//      ★ 这里【只把读流对齐到源块自己的字节 0】(丢掉 src_off 个前导字节)。
//        "对齐到目的 16 字节拍"是【打包的事】，已经下沉到 DDRPacker
//        （行首把字节计数预置成 dst_off）—— 见 docs/spec/硬件_加速器.md §4.3。
//      ⇒ shift = src_off。原来是 src_off − dst_off，那把"源对齐"和"拍对齐"
//        两件事混在一起了：下游拿到的"一拍 12 字节"因此不是整像素（dst_off 不是
//        3 的倍数时跨 5 个像素），alpha 这种"一个像素一个"的属性就配不上。
//   ★ 这一行要不要我 —— 我自己看 flags 决定（模式表见 硬件_加速器.md §1.3）：
//       1 = REPLACE 且 flags[3]=0（源是 DDR 帧）      → 要
//       2 = ALPHA（两路同时用）                       → 要（我是背景那一路）
//       4 = TINT_ALPHA（纯色层 tint + AB）             → 要（我是背景那一路）
//       3 = 纯色层（颜色合成的上层，见 §1.3.1）        → 不要（用不到背景读回）
//       0 = FILL / 未定义模式 / REPLACE 但源是 PRAM    → 不要
//     ★ 同一份判断在 PaletteSource（要 bit1）和 Compositor（要两位）各有一份，
//       改模式表要一起改。
wire        my_line;
assign      my_line  = (line_flags[2:0] == 3'd2)                      // ALPHA：要背景（读回目的自己）
                     | (line_flags[2:0] == 3'd4)                      // TINT_ALPHA：也要背景
                     | ((line_flags[2:0] == 3'd1) & ~line_flags[3]);  // REPLACE 且源是 DDR 帧：也要
//   ★ mode 3 = CK 精灵 / 纯色层：**不要**读回目的行。
//     它是"直写"模式，读回只会让每个精灵多一趟 DDR 读，帧内写不完就撕裂。
//   ★ mode 4 是独立的新模式：只要纯色层 tint + AB 这两件事同时要，才读回背景。
//   ★ 模式表改动要三处一起改：本处 / PaletteSource（要 bit1）/ Compositor（要两位）。
//   ★ 读哪一行：
//       普通 BLIT = line_src_addr（命令给的源行）
//       alpha     = line_dst_addr（【目的自己】：读回现有像素去混合）
//       tint_alpha = line_dst_addr（同上，tint 后再和背景 AB）
//     两路的相位算法完全一样，所以只在这一处换地址，后面一个字都不用改。
wire        bg_rd    = (line_flags[2:0] == 3'd2) | (line_flags[2:0] == 3'd4);
wire [31:0] row_addr = bg_rd ? line_dst_addr : line_src_addr;

assign shift = row_addr[3:0];

//   本行要读回几拍、要写几拍像素
//     读：要够【源块那 3w 个字节】⇒ 从 src_aligned 起得读 src_off + 3w 个字节
//     写：目的从 dst_aligned 起，占 dst_off + 3w 个字节
wire [17:0] line_bytes  = {2'b0, line_w} + ({2'b0, line_w} << 1);
//   ★ +16（多读一拍）保留，但现在【结构上不会死锁】了：
//     源侧吐出的字节 = 读回字节 − src_off ≥ 3w + 16 > dst_off + 3w（dst_off ≤ 15）✔
//     （以前 shift 里含着 −dst_off，两边的向上取整会打架：实测 src_off=8 / dst_off=0 /
//      w=600 时，写侧要 1808 字节只给 1800，第 113 拍卡死、AXI 被占住。）
wire [18:0] read_span   = {14'b0, row_addr[3:0]} + {1'b0, line_bytes} + 19'd16;

//==============================================================================
// 判断组合条件
//==============================================================================
assign  take_task   = (state == S_IDLE) & line_start & my_line;
assign  last_burst  = (rd_left_r == rd_burst_r);

//   读数据：只有本行还该收、且 unpacker 还能收，才握手（反压会顶回读主）
assign  unpack_take   = (rd_cnt_r < rd_beats_r);
assign  rd_ready    = unpack_take & unpack_in_ready;

//   能接下一行：本行块已交出去，且 ping-pong 已经把块换回来
assign  line_src0_free    = (state == S_IDLE);

//==============================================================================
// 组合输出
//==============================================================================
// ---- 读命令：只在 S_CMD 那一拍有效 ----
assign  rd_cmd_valid = (state == S_CMD);
assign  rd_cmd_addr  = rd_addr_r;
assign  rd_cmd_len   = rd_burst_r[7:0] - 8'd1;

// ---- 像素流出口：标准 valid/ready（skid 见上面的说明）----
//   手上的货优先（更老），请求回来的那拍紧跟其后
assign  pixel_rd_data  = pixel_hold_r ? pixel_hold_data : bank_rd_data;
assign  pixel_rd_valid = pixel_hold_r | bank_rd_valid;
assign  pixel_take     = pixel_rd_valid & pixel_rd_en;    // 下游收下了

//   ★ 提前请求的判据必须是"这一拍【结束时】兜里空不空"，不能看"这一拍开始时"：
//     数据正好在这一拍到，用开始时的状态判断会再发一次请求，
//     下一拍回来的字就把刚到、还没被取走的那个【覆盖掉】（实测：w3 被 w4 盖掉）。
assign  bank_has_data  = bank_rd_block_ready;
assign  pixel_hold_next  = (pixel_hold_r & ~pixel_take)                  // 手上没被取走 → 还是它
                       | (bank_rd_valid & (pixel_hold_r | ~pixel_take)); // 刚到的不被直接取走 → 存住
assign  bank_rd_en     = bank_has_data & ~pixel_hold_next;

//==============================================================================
// 状态机
//==============================================================================

//------------------------------------------------------------------------------
// 状态寄存器
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n)
        state <= S_IDLE;
    else
        state <= next_state;
end

//------------------------------------------------------------------------------
// 次态组合逻辑
//------------------------------------------------------------------------------
always @(*) begin
    next_state = state;
    case (state)
        S_IDLE : if (take_task)             next_state = S_CMD;
        S_CMD  : if (rd_cmd_ready)          next_state = S_RUN;
        //   ★ 还有下一笔就回 S_CMD —— 这条必须在【组合】次态里判，
        //     数据通路里只能改寄存器，不能碰 next_state（双驱动）
        S_RUN  : if (rd_cmd_done)           next_state = last_burst ? S_WAIT : S_CMD;
        S_WAIT : if (bank_hand_r & bank_low_r & bank_wr_ready) next_state = S_IDLE;
        default:                            next_state = S_IDLE;
    endcase
end

//------------------------------------------------------------------------------
// 数据通路时序逻辑
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        rd_addr_r   <= 32'd0;
        rd_burst_r  <= 14'd0;
        rd_left_r <= 14'd0;
        rd_beats_r   <= 14'd0;
        rd_cnt_r     <= 14'd0;
        unpack_flush_r     <= 1'b0;
        bank_hand_r        <= 1'b0;
        bank_low_r         <= 1'b0;
        bank_done_r        <= 1'b0;
        pixel_hold_r         <= 1'b0;
        pixel_hold_data      <= {BANK_WIDTH{1'b0}};
    end
    else begin
        bank_done_r <= 1'b0;                        // 默认：交块脉冲只打一拍

        // ---- 读口 skid ----
        //   兜里下一拍的状态全由 pixel_hold_next 算好（含"被取走就腾位"），
        //   这里不再单独写"取走就清零"—— 会被下面这行覆盖，是死代码
        pixel_hold_r <= pixel_hold_next;
        //   请求回来的这一拍要【存住】——除了"它当拍就被下游直接取走"那种情况
        //   （手上空 且 当拍取走 = 取的就是它本人，再存一遍就重复了 —— 实测踩过）
        if (bank_rd_valid & (pixel_hold_r | ~pixel_take))
            pixel_hold_data <= bank_rd_data;

        // ---- 收数：写进 unpacker ----
        if (rd_valid & rd_ready)
            rd_cnt_r <= rd_cnt_r + 14'd1;

        // ---- 本行收尾：flush 拉过之后，unpacker 吐空就是这一行写完了 ----
        //   ★ 交块是【事件】不是电平：async_PingPongBuffer 的 buffer_wr_block_done
        //     如果一直挂着，它每次收到读侧应答后会立刻又认为"已交完"，块疯狂乱换。
        //     所以置电平给状态机判出口，同时打一拍脉冲给 ping-pong。
        if (unpack_flush_r & unpack_empty & ~bank_hand_r) begin
            bank_hand_r <= 1'b1;
            bank_done_r <= 1'b1;
        end

        // ---- 等 ping-pong 换回块（wr_ready 掉一次再起来）----
        if (bank_hand_r) begin
            if (!bank_wr_ready) bank_low_r <= 1'b1;
        end

        case (state)
            // ---- 收下行任务：算好这一行的参数、装载首笔 ----
            S_IDLE : if (take_task) begin
                         rd_cnt_r     <= 14'd0;
                         bank_hand_r       <= 1'b0;
                         bank_low_r    <= 1'b0;
                         unpack_flush_r      <= 1'b0;

                         rd_beats_r   <= (read_span + 19'd15) >> 4;

                         rd_addr_r   <= {row_addr[31:4], 4'b0000};
                         rd_left_r <= (read_span + 19'd15) >> 4;
                         rd_burst_r  <= burst_of({row_addr[31:4], 4'b0000},
                                                  (read_span + 19'd15) >> 4);
                     end

            S_CMD  : ;

            // ---- 收完这一笔：还有剩就递推下一笔 ----
            S_RUN  : if (rd_cmd_done) begin
                         if (!last_burst) begin
                             rd_addr_r   <= rd_addr_r + (rd_burst_r << 4);
                             rd_left_r <= rd_left_r - rd_burst_r;
                             rd_burst_r  <= burst_of(rd_addr_r + (rd_burst_r << 4),
                                                      rd_left_r - rd_burst_r);
                         end
                         else begin
                             unpack_flush_r <= 1'b1;       // 收尾：让 unpacker 把余数吐完
                         end
                     end

            S_WAIT : ;
            default: ;
        endcase
    end
end

//==============================================================================
// 例化
//==============================================================================
//------------------------------------------------------------------------------
// 拆像素 + 相位对齐：128bit 拍 → 每拍 4 个像素
//------------------------------------------------------------------------------
DDRUnpacker #(
    .IN_WIDTH    (AXI_DATA_WIDTH),
    .PIXEL_BYTES   (PIXEL_BYTES),
    .PIXELS_PER_OUT (PIXELS_PER_WORD)
) u_DDRUnpacker (
    .clk        (clk),
    .rst_n      (rst_n),
    .in_data    (rd_data),
    .in_valid   (rd_valid & unpack_take),
    .in_ready   (unpack_in_ready),
    .line_start (take_task),
    .shift      (shift),
    .flush      (unpack_flush_r),
    .out_data   (unpack_out_data),
    .out_valid  (unpack_out_valid),
    .out_ready  (unpack_out_ready),
    .empty      (unpack_empty)
);

//   收不下就顶回去（unpacker 满时连读主一起停）
assign unpack_out_ready = bank_wr_ready;

//------------------------------------------------------------------------------
// 一行像素的 ping-pong（现成的模块）
//------------------------------------------------------------------------------
async_PingPongBuffer #(
    .DATA_WIDTH (BANK_WIDTH),
    .DEPTH      (BANK_DEPTH)
) u_bank (
    .clk_wr                 (clk),
    .clk_rd                 (clk),              // 同域：两边接同一个钟
    .rst_n                  (rst_n),

    //   交块之后本块不再收数（ping-pong 内部也有这道，这里再挡一道更直观）
    // async_PingPongBuffer gates each selected FIFO with its own wr_full.
    // Do not repeat bank_wr_ready here: that mux depends on both FIFO full
    // flags and creates a same-clock full0 <-> full1 timing loop at 300 MHz.
    .buffer_wr_en           (unpack_out_valid & ~bank_hand_r),
    .buffer_wr_data         (unpack_out_data),
    .buffer_wr_block_done   (bank_done_r),       // ★ 单拍脉冲
    .buffer_wr_block_ready  (bank_wr_ready),

    .buffer_rd_en           (bank_rd_en),
    .buffer_rd_data         (bank_rd_data),
    .buffer_rd_valid        (bank_rd_valid),
    .buffer_rd_block_ready  (bank_rd_block_ready),

    .buf_select             ()
);

endmodule
