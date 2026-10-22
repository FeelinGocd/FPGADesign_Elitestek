//==============================================================================
//  PaletteSource.v —— 源②：片上素材 RAM 取索引 → 查表 → 存好一行
//------------------------------------------------------------------------------
//  定位（这是"一路源"的完整实现，和 DDRSource 并列，出口契约一模一样）
//      收行任务 → 按拍从片上素材 RAM 读索引字 → 交给 PaletteDecoder（查表）
//      → 调色板像素流出去（→ imageCombine → Compositor）
//
//  干什么
//      · 从【片上 PRAM】（编译期烧进去的索引素材）取一行像素，全链不读 DDR；
//      · 自己数"本行要覆盖多少字节"，读够就换下一格（顺序不用别人编排）；
//      · 一行的【两趟】：第 1 趟读图一 → 发一拍 line_image_next → 第 2 趟读图二；
//      · 90° 旋转也在这里做（按列取样、攒够 4 个重新打成一拍），见下面"三条路"；
//      · 给下游报 line_src1_free（两趟都排空才算）。
//      · 不做的事：不管拍内相位（那是 DDRPacker 的活）、不管"本行几拍"（写侧算）。
//
//  上一级是谁
//      GPU_2D.v —— 顶层把它的像素流接到 Factory 的源②口（Compositor 的 src1_*）。
//
//  接口契约（握手/时序上必须守的）
//      · 行任务的字段只在 line_start 那一拍有效 ⇒ 两趟的地址都要在那一拍锁存
//        （CmdExec 发完就把 line_src_r 推到下一行了，第 2 趟再看线上就是下一行的值）；
//      · line_src1_free 要等【两趟都排空】才拉高（下一行会重装上下文）；
//      · 出口 valid 不看下游 ready（看了就是组合环，整条链一个拍都不动）。
//
//  三条路（按 flags 自己判，同一个出口）
//      ① 普通两趟（图一 + 图二）：S_IDLE → S_READ → S_WAIT →（switched）S_READ → S_WAIT → S_IDLE
//      ② 90° 旋转（flags[7]=ROT90 且是 PRAM 源、且不是纯色层）：S_IDLE → R_REQ → R_WAIT ⇄ R_OUT → S_WAIT
//         旋转时【一条行任务 = 输出一行 = 素材的一整列】：地址高 2 位 addr[31:30] 是"列号"，
//         每取一个像素只抽 1 个字节（一拍只取 1 个索引 ⇒ 吞吐约 1/4，适合小图如子弹）。
//      ③ 第 2 趟是【纯色层】（flags[2:0]=3，颜色合成）：本模块【一个字节都不读】——
//         上层颜色由 Compositor 造；但仍然要发那一拍 line_image_next（合成级靠它进 S_COMBINE）。
//
//  为什么这么做（踩过的坑 ★）
//      · ★★ 第 2 层是纯色层时【不能】去读素材：拿颜色值当字地址读一遍垃圾，然后卡在 S_WAIT
//        等一个永远不来的 palette_ready ⇒ line_src1_free 永不拉高 ⇒ CmdExec 发不出下一行
//        ⇒ 板上就是"撞一下就整屏卡死"（2026-10 抓到，见 docs/spec/硬件_加速器.md §5.6）。
//      · ★★ 选"这一趟算第几格"不能只看 pass_r：pass_r 是收行任务那拍才更新的，而 line_start
//        那一拍 Decoder 就把 mx_r 锁走了 ⇒ 第一行会选到空的第 2 格（MX 首行不做字内反序）。
//        正确写法：in_pass2 = pass_r & (state != S_IDLE)（下面 dec_flags 就是这么用的）。
//      · ★ MX 的"行尾字"由 CmdExec 算好，本模块只负责每读一个字 −1；字内反序在 PaletteDecoder。
//      · ★ "本行读几个字"必须【宁可多给、不能少给】：少给 ⇒ 写侧最后一拍凑不满 ⇒ 写等源、源等写
//        （死锁）。多给的那点落在行尾，由 DDRPacker 在行边界丢掉。
//
//  参考：docs/spec/硬件_加速器.md §5.5 / §5.6 / §7.3、docs/spec/硬件_RTL规范.md
//        pixel/DDR/DDRSource.v（同一路源的对照实现）
//==============================================================================
`timescale 1ns/100ps

module PaletteSource #(
    parameter INDEX_WIDTH           = 8,                // 索引位宽（8bpp）
    parameter COLOR_WIDTH           = 24,               // 像素颜色位宽（RGB888）
    parameter PIXELS_PER_WORD       = 4,                // 一个索引字几个索引（= 一拍几个像素）
    parameter RAM_ADDRESS_WIDTH     = 15,               // 素材 RAM 深度 2^15 字（128KB / 4 字节）
    parameter PALETTE_INIT_FILE     = "palette.hex",    // 调色板初值文件（4 份副本共用）
    parameter INDEX_INIT_FILE       = "assets.hex",     // 素材索引初值文件
    parameter IMAGE_NUM             = 2                 // 一条命令几张图（★ 与 CmdExec 一致）
)(
    //==================== 时钟 / 复位 ====================
    input  wire                                         clk,
    input  wire                                         rst_n,

    //==================== 行任务入口（接 CmdExec 的广播）  ====================
    input  wire                                         line_start,
    input  wire [32*IMAGE_NUM-1:0]                      line_src_addr,      //(I) 每图一格：本行素材基址（RAM 内【字】偏移）
    input  wire [31:0]                                  line_dst_addr,      //(I) 目的地址（只用低 4 位算"本行要覆盖多少字节"）
    input  wire [15:0]                                  line_w,             //(I) 本行画多少像素（可小于素材宽 = 裁剪）
    input  wire                                         line_fx_en,         //(I) 本行有没有第二张图（有就再读一趟）
    input  wire [8*IMAGE_NUM-1:0]                       line_flags,         //(I) 每图一格：8 位（[2:0] 模式 / [3] PRAM / [4] ck / [5] MX / [6] MY / [7] ROT90）
    input  wire [24*IMAGE_NUM-1:0]                      line_param,         //(I) 每图一格：[11:0] = 源行跨距（字）

    //==================== 颜色键控(ck) 的键色（静态配置，不随行变）====================
    input  wire [COLOR_WIDTH-1:0]                       key_color,          //(I) 键色（APB 0x2078，默认白）

    //==================== 调色板数据出口（标准 valid/ready）→ Compositor ====================
    input  wire                                         palette_ready,      //(I) Compositor 取走一拍
    output wire [PIXELS_PER_WORD*COLOR_WIDTH-1:0]       palette_data,       //(O) 一拍 = PIXELS_PER_WORD 个像素
    output wire [PIXELS_PER_WORD*3-1:0]                 palette_mask,       //(O) 12 位字节掩码（颜色键控：1=写）
    output wire [PIXELS_PER_WORD*8-1:0]                 palette_alpha,      //(O) 这 4 个像素的 α（旁路，只给合成器用）
    output wire                                         palette_valid,      //(O) 手上有货（保持到 palette_ready 握手）
    output wire                                         line_src1_free,     //(O) 我能接下一行（两趟都做完才算）
    output wire                                         line_image_next     //(O) 单拍：图一的流到此为止，后面是图二
);
//==============================================================================
// 参数换算
//==============================================================================
localparam  RAM_WIDTH               = PIXELS_PER_WORD*INDEX_WIDTH;          // 素材 RAM 数据宽度（32）
localparam  WORD_BYTES              = PIXELS_PER_WORD*(COLOR_WIDTH/8);      // 一个索引字展开成几字节（12）

//==============================================================================
// localparam：状态编码
//==============================================================================
localparam  S_IDLE = 4'd0,     // 等行任务
            S_READ = 4'd1,     // 【普通路】按拍发素材 RAM 读地址
            S_WAIT = 4'd2,     // 等手上最后一个字被取走 + Decoder 排空（两趟之间的切换也在这）
            R_REQ  = 4'd3,     // 【旋转路】发"这一列当前落在的那个字"
            R_WAIT = 4'd4,     // 【旋转路】收字 → 只抽 1 个索引 → 攒够 4 个成一拍
            R_OUT  = 4'd5;     // 【旋转路】把攒好的一拍喂给 Decoder

//==============================================================================
// 函数
//==============================================================================
//   ---- 本行要覆盖多少字节（写侧口径）：((dst_off + 3w + 15) >> 4) << 4 ----
//     写侧按 16 字节拍向上取整，所以这里也得按拍向上取整；只用加减移位，没有除法。
function [31:0] needed_bytes_of;
    input [3:0]             dst_off;
    input [15:0]            w;
    reg   [31:0]            pixels_times_three;
    begin
        pixels_times_three = {16'd0, w} + ({16'd0, w} << 1);        // 3w
        needed_bytes_of    = (({28'd0, dst_off} + pixels_times_three + 32'd15) >> 4) << 4;
    end
endfunction

//   ---- 这一格那张图，这一行要不要我（模式表见 docs/spec/硬件_加速器.md §1.3）----
//       1 = REPLACE 且 flags[3]=1（源是 PRAM 精灵）  → 要
//       2 = ALPHA / 3/4 = 纯色层（两路同时用）       → 要（我是前景那一路）
//       0 = FILL / 未定义模式 / REPLACE 但源是 DDR   → 不要
//     ★ 同一份判断在 DDRSource（要 bit0）和 Compositor（要两位）各有一份，改表要一起改。
function lane_wanted;
    input [7:0]             f;
    begin
        lane_wanted = (f[2:0] == 3'd2) || (f[2:0] == 3'd3) || (f[2:0] == 3'd4) ||
                      ((f[2:0] == 3'd1) && f[3]);
    end
endfunction

//==============================================================================
// wire / reg（按"和谁之间"分组）
//==============================================================================
//   ---- 状态机 ----
reg  [3:0]                                          state;
reg  [3:0]                                          next_state;

//   ---- 行上下文（每一趟各装一次）----
reg  [31:0]                                         index_address_r;        // 【普通路】本趟当前要读的字地址
//   ★★ 两张图的行首字地址必须【在收行任务那一拍锁存】—— 不能到第 2 趟才去读广播线：
//      CmdExec 发完第 y 行任务那一拍就把 line_src_r 递推到第 y+1 行了，而本模块第 2 趟
//      要等第 1 趟排空才开始 ⇒ 那时候线上已经是下一行的地址 ⇒ 图二整帧下移一行
//      （2026-10 用逐像素端到端测试抓到：不开 ck 的合成结果 = 图二第 y+1 行，512 像素 0 错地吻合）
reg  [31:0]                                         row_src_addr_0_r;       // 第 1 趟（图一）的行首字地址（锁存）
reg  [31:0]                                         row_src_addr_1_r;       // 第 2 趟（图二）的行首字地址（锁存）
reg  [31:0]                                         needed_bytes_r;         // 本行要覆盖多少字节（写侧口径，两趟一样）
reg  [31:0]                                         sent_bytes_r;           // 本趟已发出的字折合多少字节
reg  [7:0]                                          flags_r;                // 本趟的模式位（8 位：MX 在 [5]、ROT90 在 [7]）
reg  [23:0]                                         param_r;                // 本趟的参数（[11:0] = 源行跨距）
reg                                                 pass_r;                 // 0 = 第 1 趟（图一），1 = 第 2 趟（图二）

//   ---- 旋转路径的上下文（只有 ROT90 那一趟用）----
reg                                                 rot_active_r;           // 本趟走不走旋转路
reg  [31:0]                                         rot_row_addr_r;         // 这一列当前读到的【行】地址（每取一个像素 ± 跨距）
reg  [1:0]                                          rot_lane_r;             // 取字里的第几个字节（= 列号 0~3）
reg  [15:0]                                         rot_stride_r;           // 源行跨距（走一行加一次）
reg  [15:0]                                         rot_valid_count_r;      // 本行已经取到几个有效像素（对 line_w）
reg  [15:0]                                         rot_beat_count_r;       // 本行已经吐出几拍（对 rot_total_beats_r）
reg  [15:0]                                         rot_total_beats_r;      // 本行一共要吐几拍
reg  [1:0]                                          rot_pack_count_r;       // 这一拍已经攒了几个像素（0~3）
reg  [31:0]                                         rot_pack_r;             // 攒拍用的移位寄存器

//   ---- 和素材 RAM / Decoder 之间的连线 ----
wire [RAM_WIDTH-1:0]                                index_data;             // 素材 RAM 读出来的索引字
wire                                                index_valid;            // = 素材 RAM 读口的 read_valid
wire                                                index_read_accept;      // 这一拍发读请求能不能被受理
wire                                                decoder_ready;          // Decoder 还能收索引字
wire                                                decoder_busy;           // Decoder 手里还有像素没吐完

//==============================================================================
// assign 区：行开始时的一次性算术
//==============================================================================
//   本行要覆盖多少字节：((dst_off + 3w + 15) >> 4) << 4   （两趟一样：都覆盖同一块矩形）
wire [31:0]                                         needed_bytes;
assign needed_bytes          = needed_bytes_of(line_dst_addr[3:0], line_w);

//   旋转路：本行要吐几拍（一拍 = WORD_BYTES = 12 字节 = 4 个像素）
wire [31:0]                                         rot_total_beats_calc;
assign rot_total_beats_calc  = (needed_bytes + 32'd11) / WORD_BYTES;

//==============================================================================
// assign 区：每张图那一格切出来（第 1 格 = 图一，最后一格 = 图二）
//==============================================================================
wire [31:0]                                         src_addr_0;
assign src_addr_0            = line_src_addr[31:0];
wire [31:0]                                         src_addr_1;
assign src_addr_1            = line_src_addr[32*(IMAGE_NUM-1)+:32];
wire [7:0]                                          flags_0;
assign flags_0               = line_flags[7:0];
wire [7:0]                                          flags_1;
assign flags_1               = line_flags[8*(IMAGE_NUM-1)+:8];
wire [23:0]                                         param_0;
assign param_0               = line_param[23:0];
wire [23:0]                                         param_1;
assign param_1               = line_param[24*(IMAGE_NUM-1)+:24];

//==============================================================================
// assign 区：判断组合条件
//==============================================================================
//   这一格那张图这一行要不要我（先判这个，后面 take_task 要用）
wire                                                my_line_0;
assign my_line_0             = lane_wanted(flags_0);
wire                                                my_line_1;              // 图二那一趟要不要我【读素材】
//   ★★ 第 2 层是【纯色层】（flags[2:0]=3 或 4）时，这一趟本模块【一个字节都不读】：上层颜色由
//      Compositor 自己造。以前少了这个条件 ⇒ 拿"颜色值当字地址"读一遍垃圾、然后卡在 S_WAIT
//      等一个永远不来的 palette_ready ⇒ line_src1_free 永不拉高 ⇒ CmdExec 发不出下一行
//      ⇒ 板上"撞一下就整屏卡死"（2026-10 抓到）。
//      ★ 注意 switched（下面那个）【不能】跟着 gate 掉：合成级要靠那一拍进 S_COMBINE。
assign my_line_1             = line_fx_en & lane_wanted(flags_1) &
                               (flags_1[2:0] != 3'd3) & (flags_1[2:0] != 3'd4);

//   这一拍真的收下行任务
wire                                                take_task;
assign take_task             = (state == S_IDLE) & line_start & my_line_0;

//   第 2 趟那一格要不要旋转（与 task_rot 同款判断，但看的是 flags_1）
wire                                                task_rot_1;
assign task_rot_1            = (flags_1[7] & flags_1[3] &
                                (flags_1[2:0] != 3'd3) & (flags_1[2:0] != 3'd4));
//   本趟要不要走旋转路：ROT90（flags[7]）+ PRAM 源（flags[3]）+ 不是纯色层（[2:0]!=3/4）
//   ★ 纯色层必须排除：那一格的 param[23] 是"色调 tone"的最高位，不是旋转位（字段按模式复用）。
wire                                                task_rot;
assign task_rot              = (flags_0[7] & flags_0[3] &
                                (flags_0[2:0] != 3'd3) & (flags_0[2:0] != 3'd4));

//   ★★ 选"这一趟算第几格"不能只看 pass_r：pass_r 是收行任务那拍才更新的，而 line_start
//      那一拍 Decoder 就把 mx_r 锁走了 ⇒ 那一拍 pass_r 还是【上一条命令】留下的值，
//      第一行会选到空的第 2 格（症状：MX 命令第一行不做字内反序、顶行几个像素错位）。
wire                                                in_pass2;
assign in_pass2              = pass_r & (state != S_IDLE);

//   ★ 给 Decoder 的模式位：组合按趟选。【旋转生效时屏蔽 MX（清 bit5）】：旋转已经用
//     rot_row_addr_r 决定了列内方向，再让 Decoder 翻一次就打架。
wire [7:0]                                          dec_flags;
assign dec_flags             = (in_pass2 ? flags_1 : flags_0) &
                               ((in_pass2 ? rot_active_r : task_rot) ? 8'hdf : 8'hff);

//   还能不能再发一笔读（普通路）：手上有货就先让下游取走
wire [31:0]                                         normal_needed;
assign normal_needed         = needed_bytes_r;
wire                                                want_more;
assign want_more             = (sent_bytes_r < normal_needed);
wire                                                normal_do_read;
assign normal_do_read        = (state == S_READ) & want_more & index_read_accept;
wire                                                normal_last;
assign normal_last           = normal_do_read & (sent_bytes_r + WORD_BYTES >= normal_needed);

//   普通路的"取走一拍" / 旋转路的"收到一个字"
wire                                                normal_index_take;
assign normal_index_take     = index_valid & decoder_ready;
wire                                                rot_sample_take;
assign rot_sample_take       = (state == R_WAIT) & index_valid;

//   ★★ 送给 Decoder 的那一拍：普通路是素材 RAM 出来的字；旋转路是我们攒好的字
//      ⇒ 旋转【绕开】素材 RAM 的正常出口，Decoder 完全不知道有旋转这回事。
wire                                                decoder_input_valid;
// 旋转路在 R_WAIT 只消费 index_data 中的一个字节，不能把整字 RAM
// 字同时送给 Decoder；否则每次列采样都会额外产生一拍错误的四像素。
// 普通路仍保留 index_valid 的收尾行为，因为最后一次 RAM 读可能在
// S_READ -> S_WAIT 的状态切换后才返回。
assign decoder_input_valid   = rot_active_r ? (state == R_OUT) : index_valid;
wire [RAM_WIDTH-1:0]                                decoder_input_data;
assign decoder_input_data    = (state == R_OUT) ? rot_pack_r : index_data;

//   手上没货 + Decoder 也吐完了 ⇒ 本行真的结束
wire                                                drained;
assign drained               = ~decoder_input_valid & ~decoder_busy;

//   第 1 趟排空之后，这一拍换到第 2 趟（同时也是给下游的 line_image_next 脉冲）
//   ★ 有第 2 层就发（第 2 层是纯色时本模块不读它，但合成级要靠这一拍进 S_COMBINE）
wire                                                switched;
assign switched              = (state == S_WAIT) & drained & ~pass_r & line_fx_en;

//==============================================================================
// assign 区：组合输出
//==============================================================================
assign line_image_next       = switched;
//   ★ 两趟都排空才回 S_IDLE ⇒ 才报"能接下一行"（下一行会重装上下文）
assign line_src1_free        = (state == S_IDLE);

//   素材 RAM 读口的两个握手：普通路发读请求 / 旋转路（R_REQ 发、R_WAIT 收）
wire                                                ram_read_request;
assign ram_read_request      = normal_do_read | ((state == R_REQ) & index_read_accept);
wire                                                ram_read_ready;
assign ram_read_ready        = normal_index_take | (state == R_WAIT);

//==============================================================================
// 状态机（三段式：状态寄存器 / 次态组合逻辑 / 数据通路时序逻辑）
//==============================================================================

//------------------------------------------------------------------------------
// 第一段：状态寄存器
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n)
        state <= S_IDLE;
    else
        state <= next_state;
end

//------------------------------------------------------------------------------
// 第二段：次态组合逻辑
//------------------------------------------------------------------------------
always @(*) begin
    next_state = state;
    case (state)
        //   w = 0 这种退化情况直接进等待，别在 S_READ 里空转
        //   ★ 旋转那一趟不回 S_READ，直接进 R_REQ
        S_IDLE : if (take_task)  next_state = (needed_bytes == 32'd0) ? S_WAIT : (task_rot ? R_REQ : S_READ);
        S_READ : if (normal_last) next_state = S_WAIT;
        //   ---- 旋转路：一个字一个字地"沿列往下"取样，攒够 4 个就出一拍 ----
        R_REQ  : if (index_read_accept) next_state = R_WAIT;
        R_WAIT : if (rot_sample_take) begin
                     if ((rot_pack_count_r == 2'd3) || (rot_valid_count_r + 16'd1 >= line_w))
                         next_state = R_OUT;
                     else
                         next_state = R_REQ;
                 end
        R_OUT  : if (decoder_ready) begin
                     if (rot_beat_count_r + 16'd1 >= rot_total_beats_r) next_state = S_WAIT;
                     else if (rot_valid_count_r >= line_w)              next_state = R_OUT;
                     else                                              next_state = R_REQ;
                 end
        //   ---- 排空后：这一行还有图二 ⇒ 接着读第 2 趟（要读；旋转同理）；否则这一行完了 ----
        S_WAIT : if (drained)    next_state = switched
                                             ? (my_line_1 ? (task_rot_1 ? R_REQ : S_READ) : S_IDLE)
                                             : S_IDLE;
        default:                 next_state = S_IDLE;
    endcase
end

//------------------------------------------------------------------------------
// 第三段：数据通路时序逻辑
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        index_address_r     <= 32'd0;
        row_src_addr_0_r    <= 32'd0;
        row_src_addr_1_r    <= 32'd0;
        needed_bytes_r      <= 32'd0;
        sent_bytes_r        <= 32'd0;
        flags_r             <= 8'd0;
        param_r             <= 24'd0;
        pass_r              <= 1'b0;
        rot_active_r        <= 1'b0;
        rot_row_addr_r      <= 32'd0;
        rot_lane_r          <= 2'd0;
        rot_stride_r        <= 16'd0;
        rot_valid_count_r   <= 16'd0;
        rot_beat_count_r    <= 16'd0;
        rot_total_beats_r   <= 16'd0;
        rot_pack_count_r    <= 2'd0;
        rot_pack_r          <= 32'd0;
    end
    else begin
        case (state)
            // ---- 收下行任务：装【第 1 趟】的上下文 ----
            S_IDLE : if (take_task) begin
                         //   ★ MX：CmdExec 广播过来的 line_src_addr 已经是【行尾那个字】，
                         //     这里只管"每读一个字 −1"。旋转时那一格是"列号+字地址"。
                         //   ★ 两趟的地址都在【这一拍】锁存：第 2 趟开始时线上早就是下一行的值了
                         index_address_r     <= src_addr_0;
                         row_src_addr_0_r    <= src_addr_0;
                         row_src_addr_1_r    <= src_addr_1;
                         flags_r             <= flags_0;
                         param_r             <= param_0;
                         needed_bytes_r      <= needed_bytes;
                         sent_bytes_r        <= 32'd0;
                         pass_r              <= 1'b0;
                         rot_active_r        <= task_rot;
                         rot_lane_r          <= src_addr_0[31:30];        // 列号
                         rot_stride_r        <= param_0[11:0];
                         rot_valid_count_r   <= 16'd0;
                         rot_beat_count_r    <= 16'd0;
                         rot_total_beats_r   <= rot_total_beats_calc[15:0];
                         rot_pack_count_r    <= 2'd0;
                         rot_pack_r          <= 32'd0;
                         //   旋转路的起点：MX 时从【这一列的下端】往上取（= 转出来的图上下翻）
                         if (task_rot)
                             rot_row_addr_r <= flags_0[5]
                                             ? ({2'd0, src_addr_0[29:0]} + ({16'd0, line_w - 16'd1} * param_0[11:0]))
                                             : {2'd0, src_addr_0[29:0]};
                     end

            // ---- 普通路：按拍发读地址（★ 素材 RAM 读口受理了 read_accept 才算数）----
            S_READ : if (normal_do_read) begin
                         index_address_r <= flags_r[5] ? (index_address_r - 32'd1)
                                                       : (index_address_r + 32'd1);
                         sent_bytes_r    <= sent_bytes_r + WORD_BYTES;
                     end

            // ---- 旋转路：收一个字 ⇒ 只抽 1 个字节（这一列当前那个像素）⇒ 攒 4 个成一拍 ----
            R_WAIT : if (rot_sample_take) begin
                         case (rot_pack_count_r)
                             2'd0    : rot_pack_r <= {24'd0, index_data[(rot_lane_r*8)+:8]};
                             2'd1    : rot_pack_r <= {16'd0, index_data[(rot_lane_r*8)+:8], rot_pack_r[7:0]};
                             2'd2    : rot_pack_r <= {8'd0,  index_data[(rot_lane_r*8)+:8], rot_pack_r[15:0]};
                             default : rot_pack_r <= {       index_data[(rot_lane_r*8)+:8], rot_pack_r[23:0]};
                         endcase
                         rot_valid_count_r <= rot_valid_count_r + 16'd1;
                         if (rot_pack_count_r == 2'd3) rot_pack_count_r <= 2'd0;
                         else                          rot_pack_count_r <= rot_pack_count_r + 1'b1;
                         //   ★ 沿【列】往下走一行：这正是"旋转"和"普通按行走"的分界
                         rot_row_addr_r <= flags_r[5] ? (rot_row_addr_r - rot_stride_r)
                                                      : (rot_row_addr_r + rot_stride_r);
                     end

            // ---- 旋转路：交出一拍给 Decoder ----
            R_OUT : if (decoder_ready) begin
                        if (rot_beat_count_r + 16'd1 < rot_total_beats_r) begin
                            rot_beat_count_r <= rot_beat_count_r + 1'b1;
                            rot_pack_count_r <= 2'd0;
                            //   已经取够 line_w 个 ⇒ 后面几拍补 0（行尾多出来的那些会被写侧丢掉）
                            if (rot_valid_count_r >= line_w) rot_pack_r <= 32'd0;
                        end
                    end

            // ---- 第 1 趟排空：有第 2 趟就装【第 2 趟】的上下文（普通路继续按行走 / 旋转路按列走）----
            S_WAIT : if (switched) begin
                         index_address_r     <= row_src_addr_1_r;      // ★ 用锁存值，不看线上
                         flags_r             <= flags_1;
                         param_r             <= param_1;
                         needed_bytes_r      <= needed_bytes;
                         sent_bytes_r        <= 32'd0;
                         pass_r              <= 1'b1;
                         rot_active_r        <= task_rot_1;
                         rot_lane_r          <= src_addr_1[31:30];
                         rot_stride_r        <= param_1[11:0];
                         rot_valid_count_r   <= 16'd0;
                         rot_beat_count_r    <= 16'd0;
                         rot_total_beats_r   <= rot_total_beats_calc[15:0];
                         rot_pack_count_r    <= 2'd0;
                         rot_pack_r          <= 32'd0;
                         if (task_rot_1)
                             rot_row_addr_r <= flags_1[5]
                                             ? ({2'd0, src_addr_1[29:0]} + ({16'd0, line_w - 16'd1} * param_1[11:0]))
                                             : {2'd0, src_addr_1[29:0]};
                     end
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
    .DATA_WIDTH                                 (RAM_WIDTH          ),
    .ADDRESS_WIDTH                              (RAM_ADDRESS_WIDTH  ),
    .INIT_FILE                                  (INDEX_INIT_FILE    )
) u_PaletteRAM (
    .clk                                        (clk),
    .rst_n                                      (rst_n),
    .clear                                      (line_start),               // 行边界：丢掉上一行可能残留的那个字
    .read_request                               (ram_read_request),
    .read_accept                                (index_read_accept),
    .read_address                               ((state == R_REQ) || (state == R_WAIT)
                                                 ? rot_row_addr_r[RAM_ADDRESS_WIDTH-1:0]
                                                 : index_address_r[RAM_ADDRESS_WIDTH-1:0]),
    .read_data                                  (index_data),
    .read_valid                                 (index_valid),
    .read_ready                                 (ram_read_ready)
);

//------------------------------------------------------------------------------
// Decoder：查调色板（出口就是本模块对外的调色板数据流；相位整形已下沉到 DDRPacker）
//   ★ 旋转路送进来的是我们攒好的字，Decoder 不用知道区别（decoder_input_* 已经选好）
//------------------------------------------------------------------------------
PaletteDecoder #(
    .INDEX_WIDTH                                (INDEX_WIDTH        ),
    .COLOR_WIDTH                                (COLOR_WIDTH        ),
    .INDEXES_PER_WORD                           (PIXELS_PER_WORD    ),
    .BYTES_PER_BEAT                             (WORD_BYTES         ),
    .PALETTE_INIT_FILE                          (PALETTE_INIT_FILE  )
) u_PaletteDecoder (
    .clk                                        (clk),
    .rst_n                                      (rst_n),
    .line_start                                 (line_start),
    .index_data                                 (decoder_input_data),
    .index_valid                                (decoder_input_valid),
    .index_ready                                (decoder_ready),
    .palette_data                               (palette_data),
    .key_color                                  (key_color),                // ★ ck 键色（来自 APB 寄存器）
    .line_flags                                 (dec_flags),                // ★ 本趟的模式位（组合按趟选，旋转时屏蔽 MX）
    .palette_mask                               (palette_mask),             // ★ 掩码和颜色同一个出口契约，一起进合成器
    .palette_alpha                              (palette_alpha),            // ★ α 走旁路：不并进 palette_data
    .palette_valid                              (palette_valid),
    .palette_ready                              (palette_ready),
    .busy                                       (decoder_busy)
);

endmodule
