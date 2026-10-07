//==============================================================================
//  PaletteSource.v —— 源②：片上素材 RAM 取索引 → 查表 → 存好一行
//------------------------------------------------------------------------------
//  定位（这是"一路源"的完整实现，和 DDRSource 并列，出口契约一模一样）
//      收行任务 → 按拍从片上素材 RAM 读索引字 → 交给 PaletteDecoder
//      （查表）→ 调色板数据流直接给 Compositor 合成
//
//  链路
//      素材 RAM ──32bit/拍(4 个索引)──▶ PaletteDecoder ──96bit/拍(4 像素)──▶ Compositor
//                                          （查表，内含 PaletteLUT；不管拍内偏移）
//
//  和 DDRSource 的关系
//      · 出口契约【一模一样】：palette_data / palette_valid / palette_ready + src_free
//        ⇒ 合成器、DDRPacker、WriteController 一行都不用改。
//      · 但内部简单得多：DDR 是"平坦字节流"，所以要拆笔 + 28 字节累加器做相位；
//        素材 RAM 是【编译期定长的字阵列】，一拍取一整字就是 4 个索引，
//        没有页边界、没有拆笔、没有"字节↔像素"错位。
//      · 所以这里没有 ping-pong：Decoder 自己的窗口寄存器已经能兜住反压。
//
//  水平镜像（flags[5] = MX）
//      CmdExec 广播过来的地址已经是【本行行尾那个字】，所以这里只做一件事：
//      每读一个字【地址 −1】（不是 +1）。取几个字（needed_bytes）一个字都不用改 ——
//      要覆盖的目的字节数没变，只是从尾部往回读。
//
//  地址怎么来（★ 不查表、不查描述表）
//      CmdExec 每行把【本行素材基址】直接给下来：
//          第 0 行 = 命令的 src 字段（素材在素材 RAM 里的字偏移）
//          以后每行 CmdExec 加一次"源行跨距"（命令的 color 字段，0 = 沿用目的跨距）
//      所以本模块只管：本行基址 + 字序号（0,1,2,...）—— 不用跨距、不用行号。
//      ★ 行任务里的 line_w 是【本次画多宽】，可以小于素材宽 ⇒ 右侧裁剪天然成立。
//
//  素材 RAM 的读口是【带 valid/ready 的】（见 PaletteRAM.v）
//      read_request / read_accept   我要读一个字，这一拍能不能受理
//      read_valid / read_ready      读出来的字有效（一直保持到被取走），这一拍被谁取走了
//      ⇒ 本模块【不再自己记】"RAM 晚一拍出数"这件事。改这里只要想清楚
//        "这一拍该不该发读请求"，不用再算延迟。
//
//  "本行读几个字"怎么定
//      写侧要的字节流长度 = ((dst_off + 3w + 15) >> 4) << 4（按 16 字节拍向上取整）。
//      给一个索引字 = 12 字节 ⇒ 累计到不少于它为止（只用移位和比较，没有除法）。
//      ★【宁可多给，不能少给】：少给的话写侧最后一拍凑不满 → 写等源、源等写（死锁）。
//        M2 当初 read_span 少 8 字节就是这么把整条链和 CPU 一起卡死的。
//        多给的那点落在行尾，由 DDRPacker 在行边界丢掉（现有约定）。
//
//  为什么 src_free 要等 Decoder 排空
//      下一行的 line_start 会把 Decoder 的窗口清掉 —— 那会把上一行还没吐出去的
//      像素直接吃掉。所以必须等到 Decoder 手里真的没货了才允许开新行。
//
//  参考：pixel/DDR/DDRSource.v（同一路源的对照实现）
//        docs/spec/硬件_加速器.md §7.3（PRAM 素材与调色板）
//==============================================================================
`timescale 1ns/100ps

module PaletteSource #(
    parameter INDEX_WIDTH           = 8,                // 索引位宽（8bpp）
    parameter COLOR_WIDTH           = 24,               // 像素颜色位宽（RGB888）
    parameter PIXELS_PER_WORD       = 4,                // 一个索引字几个索引（= 一拍几个像素）
    parameter RAM_ADDRESS_WIDTH     = 15,               // 素材 RAM 深度 2^15 字（128KB / 4 字节）
    parameter PALETTE_INIT_FILE     = "palette.hex",    // 调色板初值文件（4 份副本共用）
    parameter INDEX_INIT_FILE       = "assets.hex"      // 素材索引初值文件
)(
    //==================== 时钟 / 复位 ====================
    input  wire                                 clk,
    input  wire                                 rst_n,

    //==================== 行任务入口（接 CmdExec 的广播）====================
    input  wire                                 line_start,

    input  wire [31:0]                          line_src_addr,      //(I) 本行素材基址（RAM 内【字】偏移）
    input  wire [31:0]                          line_dst_addr,      //(I) 目的地址（只用低 4 位算"本行要覆盖多少字节"）
    input  wire [15:0]                          line_w,             //(I) 本行画多少像素（可小于素材宽 = 裁剪）
    input  wire [6:0]                           line_flags,         //(I) [5] = MX 水平镜像（倒着读字）

    //==================== 颜色键控(ck) 的键色（静态配置，不随行变）====================
    input  wire [COLOR_WIDTH-1:0]               key_color,          //(I) 键色（APB 0x2078，默认白）

    //==================== 调色板数据出口（标准 valid/ready）→ Compositor ====================
    input  wire                                 palette_ready,      //(I) Compositor 取走一拍
    output wire [PIXELS_PER_WORD*COLOR_WIDTH-1:0]   palette_data,   //(O) 一拍 = PIXELS_PER_WORD 个像素
    output wire [PIXELS_PER_WORD*3-1:0]         palette_mask,       //(O) 12 位字节掩码（颜色键控：1=写）
    output wire [PIXELS_PER_WORD*8-1:0]         palette_alpha,      //(O) 这 4 个像素的 α（旁路，只给合成器用）
    output wire                                 palette_valid,      //(O) 手上有货（保持到 palette_ready 握手）
    output wire                                 src_free            //(O) 我能接下一行
);

//==============================================================================
// 参数换算
//==============================================================================
localparam  RAM_WIDTH       = PIXELS_PER_WORD * INDEX_WIDTH;    // 素材 RAM 数据宽度（32）
localparam  BYTES_PER_PIXEL = COLOR_WIDTH / 8;                  // 一个像素几字节（3）
localparam  WORD_BYTES      = PIXELS_PER_WORD * BYTES_PER_PIXEL;// 一个索引字展开成几字节（12）

//==============================================================================
// localparam
//==============================================================================
localparam  S_IDLE = 2'd0,      // 等行任务
            S_READ = 2'd1,      // 按拍发素材 RAM 读地址
            S_WAIT = 2'd2;      // 等手上最后一个字被取走 + Decoder 排空

//==============================================================================
// 函数
//==============================================================================
// ---- 本行要覆盖多少字节（写侧口径）：((dst_off + 3w + 15) >> 4) << 4 ----
//   写侧按 16 字节拍向上取整，所以这里也得按拍向上取整；只用加减移位，没有除法。
function [31:0] needed_bytes_of;
    input [3:0]  dst_off;
    input [15:0] w;
    reg   [31:0] pixels_times_three;
    begin
        pixels_times_three = {16'd0, w} + ({16'd0, w} << 1);        // 3w
        needed_bytes_of    = (({28'd0, dst_off} + pixels_times_three + 32'd15) >> 4) << 4;
    end
endfunction

//==============================================================================
// wire / reg
//==============================================================================
//   ---- 状态机 ----
reg  [1:0]                      state;
reg  [1:0]                      next_state;

//   ---- 行上下文 ----
reg  [31:0]                     index_address_r;    // 本行当前要读的字地址
reg                             src_mx_r;           // 本行是否【倒着读字】（flags[5]）
reg  [31:0]                     needed_bytes_r;     // 本行要覆盖多少字节（写侧口径）
reg  [31:0]                     sent_bytes_r;       // 已发出的字折合多少字节

//   ---- 和素材 RAM / Decoder 之间的连线 ----
wire [RAM_WIDTH-1:0]            index_data;         // 素材 RAM 读出来的索引字
wire                            index_valid;        // = 素材 RAM 读口的 read_valid
wire                            index_take;         // 这一拍 Decoder 把手上那个字收走了
wire                            index_read_accept;  // 这一拍发读请求能不能被受理
wire                            decoder_ready;      // Decoder 还能收索引字
wire                            decoder_busy;       // Decoder 手里还有像素没吐完

//==============================================================================
// assign 区：行开始时的一次性算术
//==============================================================================
//   本行要覆盖多少字节：((dst_off + 3w + 15) >> 4) << 4
wire [31:0] needed_bytes = needed_bytes_of(line_dst_addr[3:0], line_w);

//==============================================================================
// 判断组合条件
//==============================================================================
//   这一拍真的收下行任务
//   ★ 这一行要不要我 —— 我自己看 flags 决定（模式表见 硬件_加速器.md §1.3）：
//       1 = REPLACE 且 flags[3]=1（源是 PRAM 精灵）  → 要
//       2 = ALPHA   3 = CKEY（两路同时用）           → 要（我是前景那一路）
//       0 = FILL / 未定义模式 / REPLACE 但源是 DDR   → 不要
//     ★ 同一份判断在 DDRSource（要 bit0）和 Compositor（要两位）各有一份，改表要一起改。
wire    my_line    = (line_flags[2:0] == 3'd2) | (line_flags[2:0] == 3'd3)
                   | ((line_flags[2:0] == 3'd1) & line_flags[3]);
wire    take_task  = (state == S_IDLE) & line_start & my_line;

//   还能不能再发一笔读：手上有货就先让下游取走（否则新数据会把还没取走的那拍冲掉）
//   ★ 这条规则现在由素材 RAM 读口自己给（read_accept），本模块不用再算"晚一拍"和"保持"
wire    want_more  = (sent_bytes_r < needed_bytes_r);
wire    do_read    = (state == S_READ) & want_more & index_read_accept;

//   这一拍发出去的就是本行最后一个字 ⇒ 转去等流水排空
wire    last_word  = do_read & (sent_bytes_r + WORD_BYTES >= needed_bytes_r);

//   手上没货 + Decoder 也吐完了 ⇒ 本行真的结束
wire    drained    = ~index_valid & ~decoder_busy;

//==============================================================================
// 组合输出
//==============================================================================
assign  index_take = index_valid & decoder_ready;   // 这一拍 Decoder 把手上那个字收走了
assign  src_free   = (state == S_IDLE);             // 能接下一行

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
        //   w = 0 这种退化情况直接进等待，别在 S_READ 里空转
        S_IDLE : if (take_task)  next_state = (needed_bytes == 32'd0) ? S_WAIT : S_READ;
        S_READ : if (last_word)  next_state = S_WAIT;
        S_WAIT : if (drained)    next_state = S_IDLE;
        default:                 next_state = S_IDLE;
    endcase
end

//------------------------------------------------------------------------------
// 数据通路时序逻辑
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        index_address_r <= 32'd0;
        needed_bytes_r  <= 32'd0;
        sent_bytes_r    <= 32'd0;
        src_mx_r        <= 1'b0;
    end
    else begin
        case (state)
            // ---- 收下行任务：算好这一行从哪个字开始、要覆盖多少字节 ----
            S_IDLE : if (take_task) begin
                         //   ★ MX：CmdExec 广播过来的 line_src_addr 已经是【行尾那个字】，
                         //     这里只管"每读一个字 −1"。取出器不需要知道 w。
                         index_address_r <= line_src_addr;
                         src_mx_r        <= line_flags[5];
                         needed_bytes_r  <= needed_bytes;
                         sent_bytes_r    <= 32'd0;
                     end

            // ---- 按拍发读地址：★ 素材 RAM 读口受理了（read_accept）才算数 ----
            S_READ : if (do_read) begin
                         index_address_r <= src_mx_r ? (index_address_r - 32'd1)
                                                     : (index_address_r + 32'd1);
                         sent_bytes_r    <= sent_bytes_r + WORD_BYTES;
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
// 素材 RAM：索引数据（生成 bit 阶段烧进去，运行期只读）
//------------------------------------------------------------------------------
PaletteRAM #(
    .DATA_WIDTH     (RAM_WIDTH           ),
    .ADDRESS_WIDTH  (RAM_ADDRESS_WIDTH   ),
    .INIT_FILE      (INDEX_INIT_FILE     )
) u_PaletteRAM (
    .clk            (clk),
    .rst_n          (rst_n),
    .clear          (line_start),               // 行边界：丢掉上一行可能残留的那个字
    .read_request   (do_read),                  // 一拍取一个字 = 4 个索引
    .read_accept    (index_read_accept),
    .read_address   (index_address_r[RAM_ADDRESS_WIDTH-1:0]),
    .read_data      (index_data),
    .read_valid     (index_valid),
    .read_ready     (index_take)
);

//------------------------------------------------------------------------------
// Decoder：查调色板（出口就是本模块对外的调色板数据流；相位整形已下沉到 DDRPacker）
//------------------------------------------------------------------------------
PaletteDecoder #(
    .INDEX_WIDTH        (INDEX_WIDTH       ),
    .COLOR_WIDTH        (COLOR_WIDTH       ),
    .INDEXES_PER_WORD   (PIXELS_PER_WORD   ),
    .BYTES_PER_BEAT     (WORD_BYTES        ),
    .PALETTE_INIT_FILE  (PALETTE_INIT_FILE )
) u_PaletteDecoder (
    .clk            (clk),
    .rst_n          (rst_n),
    .line_start     (line_start),

    .index_data     (index_data),
    .index_valid    (index_valid),
    .index_ready    (decoder_ready),
    .palette_data   (palette_data),
    .key_color      (key_color),            // ★ 颜色键控的键色（来自 APB 寄存器）
    .line_flags     (line_flags),           // ★ [5] = MX，Decoder 用它决定字内索引要不要反序
    .palette_mask   (palette_mask),         // ★ 掩码和颜色同一个出口契约，一起进合成器
    .palette_alpha  (palette_alpha),        // ★ α 走旁路：不并进 palette_data，只给合成器
    .palette_valid  (palette_valid),
    .palette_ready  (palette_ready),
    .busy           (decoder_busy)
);

endmodule
