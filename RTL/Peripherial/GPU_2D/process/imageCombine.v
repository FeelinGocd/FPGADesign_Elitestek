//==============================================================================
//  imageCombine.v —— 图片合成级：把两张图合成一张
//------------------------------------------------------------------------------
//  干什么（★ 就两件事，别的一概不管）
//      ① 缓存【第一张图】那一行：素材源先把图一那一行流进来，写进片上行缓冲
//      ② 合成：见到 line_image_next（"图一的流到此为止"）之后，图二那一行边来边合
//      ★ 它不管 AB、不管 ck 判定、不管行任务调度、不管打包 —— 那些各有其人
//      ★ 它不认识 DDR、不认识 PRAM、不认识命令：只认"这一拍的 4 个像素"

//  一条命令两张图（素材都在同一个 PRAM 里、读口只有一个）⇒ 两行只能【串行】读：
//      素材源：第 1 趟读图一 → 第 2 趟读图二，中间给一拍 line_image_next
//      本模块：line_image_next 之前 = 存缓冲；之后 = 读缓冲 + 图二逐拍合成
//      ⇒ 所以它必须缓存一行（不放 DDR：那是你最紧的带宽，多写多读一次不划算）

//  为什么不用自己数 "这一行多少像素"
//      那是上游的事：素材源读够它自己的 needed_bytes 就发 line_image_next —— 边界由它划，
//      本模块只管"脉冲之前写、脉冲之后读"。两边不用各算一套，也就不会算岔。

//  三态
//      S_EMPTY  ：缓冲空（还没开始 / 这一行已经合完）—— line_imgRow_free = 1
//      S_FILL   ：在写缓冲（这一段 imgCombine_valid 恒 0，下游什么也拿不到）
//      S_COMBINE：在合成出流（图二每来一拍，就从缓冲取一拍合一下）
//      ★ 旁路（line_fx_en = 0，单图 / FILL 命令）：in 直通 out，缓冲一个字节都不碰
//        ⇒ 老路径的行为和时序【一位不差】

//  掩码（★ 按"合成后整张图"给，不是按某一路给）
//      字节级：图二在这一字节要写 ⇒ 用图二的颜色；否则用图一的颜色
//      imgCombine_mask = 图二 mask | 图一 mask        （任一张要写 ⇒ 这一字节要写）
//      imgCombine_alpha= 这个像素在图二那边要写 ⇒ 用图二的 α；否则用图一的 α
//      ★ ck 开关（line_colorKey_en）在本模块【一处】gate：不开就把两张图的掩码都当全 1
//        ⇒ 上面那张整张盖住下面那张；开了才按源给的真实掩码选图层。
//      ★ 于是"原图透明、特效盖住的地方"会被写（特效把它填上了）✔
//      ★ 特效什么都没产出（mask=0，比如强度 0）⇒ 掩码全看图一 ⇒ 原图的透明洞还在 ✔

//  AB 那一级在哪
//      在本模块【之后】（Compositor 里的 alphaBlending）：本模块出的是"合成后的前景"，
//      背景是 DDR 读回的目的那一行 —— 和现在的 alpha 一模一样，那一级一行都不用改。

//  行缓冲：480 拍 × 140 位 ≈ 67 Kb ≈ 7 块 RAM10（一拍 4 个像素，data+mask+α 一起存）
//  上一级是谁
//      factory/Compositor.v —— 源② 那条流（素材源）先进本模块，合成结果再交给它去和源① 混合。
//
//  参考：docs/spec/硬件_加速器.md §4.1 / §4.2 / §5.5
//==============================================================================
`timescale 1ns/100ps

module imageCombine #(
    parameter PIXEL_WIDTH   = 96,       // 一拍几个像素 × 24 位（4 个 RGB888 像素）
    parameter PIXEL_BYTES   = 12,       // 一拍多少字节（掩码位宽）
    parameter PIXELS        = 4,        // 一拍几个像素
    parameter ALPHA_WIDTH   = 8,        // 每个像素的 α 位宽
    parameter ROW_PIXELS    = 1920      // 一行最多多少像素（行缓冲深度按它算）
)(
    //==================== 时钟 / 复位 ====================
    input  wire                                 clk,
    input  wire                                 rst_n,

    //==================== 行任务（CmdExec 广播里它要的那几根）  ====================
    input  wire                                 line_start,         //(I) 单拍：新的一行开始（清指针/状态）← CmdExec
    input  wire                                 line_fx_en,         //(I) 本命令有没有第二张图；0 = 旁路  ← CmdExec
    input  wire                                 line_colorKey_en,   //(I) 命令级 ck 开关（最后一格的 flags[4]）← Compositor.flags_r[4]

    //==================== 和素材源之间"这一行换图了"  ← PaletteSource  ====================
    input  wire                                 line_image_next,    //(I) 单拍：图一的流到此为止 ← PaletteSource
    input  wire                                 line_src_free,      //(I) 素材源空闲（两趟都排空）← PaletteSource.line_src1_free

    //==================== 像素流入口（素材源：先图一、后图二）  ← PaletteSource  ====================
    input  wire [PIXEL_WIDTH-1:0]               source_data,        //(I) 一拍 PIXELS 个像素 ← PaletteSource.palette_data
    input  wire [PIXEL_BYTES-1:0]               source_mask,        //(I) 字节掩码（1=写）  ← PaletteSource.palette_mask
    input  wire [PIXELS*ALPHA_WIDTH-1:0]        source_alpha,       //(I) 每像素的 α        ← PaletteSource.palette_alpha
    input  wire                                 source_valid,       //(I) 这一拍有效        ← PaletteSource.palette_valid
    output wire                                 source_ready,       //(O) 取走一拍          → PaletteSource.palette_ready

    //==================== 像素流出口（合成结果：data + mask + α，和入口同形）  → DDRPacker（再往 WriteController）  ====================
    output wire [PIXEL_WIDTH-1:0]               imgCombine_data,    //(O) 合成后的 PIXELS 个像素 → Compositor
    output wire [PIXEL_BYTES-1:0]               imgCombine_mask,    //(O) 合成后整张图的掩码   → Compositor
    output wire [PIXELS*ALPHA_WIDTH-1:0]        imgCombine_alpha,   //(O) 合成后的 α            → Compositor
    output wire                                 imgCombine_valid,   //(O) 这一拍有效            → Compositor
    input  wire                                 imgCombine_ready,   //(I) 下游收下了            ← Compositor

    //==================== 节奏：能不能接下一行 ====================
    output wire                                 line_imgRow_free    //(O) 缓冲空着（下一行可以进来）→ Compositor.line_imgRow_free
);

//==============================================================================
// localparam / 参数换算
//==============================================================================
localparam  BYTES_PER_PIXEL = PIXEL_BYTES/PIXELS;                               // 一个像素几字节（3）
localparam  BEAT_W          = PIXEL_WIDTH+PIXEL_BYTES+PIXELS*ALPHA_WIDTH;       // 缓冲里一拍多宽（140）
localparam  ROW_BEATS       = (ROW_PIXELS+PIXELS-1)/PIXELS;                     // 一行最多几拍（480）
localparam  ADDR_W          = 10;                                               // 480 拍 ⇒ 10 位地址够

//   状态
localparam  S_EMPTY         = 2'd0;       // 缓冲空（等 line_start / 这一行已合完）
localparam  S_FILL          = 2'd1;       // 在写缓冲（图一那一趟）
localparam  S_COMBINE       = 2'd2;       // 在合成出流（图二那一趟）

//==============================================================================
// wire / reg
//==============================================================================
// ---- 状态机 ----
reg  [1:0]                                      state;
reg  [1:0]                                      next_state;

// ---- 行缓冲（一拍一条：data + mask + alpha 一起存）----
reg  [BEAT_W-1:0]                               row_buf [0:ROW_BEATS-1];
reg  [ADDR_W-1:0]                               wr_ptr_r;       // 写指针（图一那一趟自增）
reg  [ADDR_W-1:0]                               rd_ptr_r;       // 读指针（图二那一趟自增）
reg  [BEAT_W-1:0]                               rd_beat_r;      // 缓冲读出来的"底图那一拍"（同步读，下一拍到）

// ---- 判断组合条件 ----
wire                                            comb_on;        // 这一行走不走合成（不然就是旁路）
wire                                            fill_take;      // 写缓冲那一拍
wire                                            take_in;        // 这一拍真的从上游取走一拍
wire [BEAT_W-1:0]                               cur_beat;       // 上游这一拍（打包成一条）
wire [BEAT_W-1:0]                               mix_beat;       // 合成后的一条
wire [BEAT_W-1:0]                               base_beat;      // 从缓冲里取出来的"图一那一拍"
wire [ADDR_W-1:0]                               rd_addr_go;     // 这一拍要发的读地址
wire                                            rd_en_go;       // 这一拍发读
wire                                            base_ok;        // 这个读地址还在写过的范围里

//==============================================================================
// 判断组合条件
//==============================================================================
assign comb_on              = line_fx_en;                                  // 有第二张图才走合成
assign line_imgRow_free     = ~comb_on | (state == S_EMPTY);               // 缓冲空着才允许接下一行

//   写缓冲那一拍：在 S_FILL 里取到一拍就往里写（这一段本来也不出数）
assign fill_take            = (state == S_FILL) & source_valid;

//   取数与出数：
//     S_FILL    ：只进不出（进料看 source_valid 即可）
//     S_COMBINE ：进来一拍就出去一拍 ⇒ 卡在"下游确实收下"
//     旁路       ：入口直接对接出口
//   ★★ imgCombine_valid 【绝不能】看 imgCombine_ready：下游（Compositor）的取数条件
//      src1_rd_en = src_en & (pixel_ready & pixel_valid)、而 pixel_valid 里含着本模块的
//      imgCombine_valid ⇒ 看了就成【组合环】，两边互相等，整条链一个拍都不动
//      （老代码的 src1_valid 来自源里寄存的一拍，所以没这个环；这里必须照做）
assign take_in              = (state == S_FILL) ? source_valid : (source_valid & imgCombine_ready);
assign source_ready             = comb_on ? ((state == S_FILL) ? 1'b1 : (state == S_COMBINE) ? imgCombine_ready : 1'b0)
                            : imgCombine_ready;

//   上游那一拍打包成缓冲里的一条（顺序和 BEAT_W 的切法一致：α 在高位）
assign cur_beat             = {source_alpha, source_mask, source_data};

//   ★★ 缓冲读：发读是【组合】的（不是寄存器发出）
//      读是同步的 ⇒ 这一拍发的读，数据下一个沿到。而下一拍的"底图"必须下一拍一来就能用，
//      所以在处理当前拍的同一拍，就要把【下一拍】的读发出去：
//          进 S_COMBINE 那一拍（line_image_next）：发地址 0（第一拍的底图）
//          处理第 n 拍那一拍        ：发地址 n+1
//      守一下范围：地址超出"写过的拍数"就不发（底图按全 0 处理，异常时也不会读到垃圾）
assign rd_addr_go           = line_image_next ? {ADDR_W{1'b0}} : (rd_ptr_r + 1'b1);
assign rd_en_go             = line_image_next | ((state == S_COMBINE) & source_valid & imgCombine_ready & base_ok);
assign base_ok              = (rd_ptr_r < wr_ptr_r);
//   底图：正常就用读出来的那一拍；越界就当成"什么都没有"（颜色全由上面那张决定）
assign base_beat            = (state == S_COMBINE) ? rd_beat_r : {BEAT_W{1'b0}};

//==============================================================================
// 合成：逐字节选颜色 + 掩码相或 + 逐像素选 α（见文件头"掩码"那段）
//==============================================================================
genvar gi;
generate
for (gi = 0; gi < PIXEL_BYTES; gi = gi + 1) begin : g_byte
    //   ★★ 合成规则（2026-10 板上定稿）：ck 判在【合成之前】（源里比颜色），这里只按掩码办事。
    //      ck 开：图二这一字节要写 → 用图二；被抠掉（掩码 0）→ 露出缓冲里图一那一字节。
    //      ck 关：两张图的掩码都当全 1 用 ⇒ 上面那张整张盖住下面那张（老行为）。
    //   ★ 使能就这一处（命令级 1 位）：Decoder 永远比颜色、永远给"这张图哪几个字节透明"，
    //     "这次要不要抠"由本模块一处 gate 掉 —— 不用把开关摊到每张图、也不用摊到下游。
    wire cur_wr_en;
    wire base_wr_en;

    assign cur_wr_en  = line_colorKey_en ? cur_beat[PIXEL_WIDTH + gi]  : 1'b1;
    assign base_wr_en = line_colorKey_en ? base_beat[PIXEL_WIDTH + gi] : 1'b1;

    assign mix_beat[gi*8 +: 8] = cur_wr_en ? cur_beat[gi*8 +: 8] : base_beat[gi*8 +: 8];
    //   出口掩码 = 两张图掩码相或（ck 关时恒全 1）
    assign mix_beat[PIXEL_WIDTH + gi] = cur_wr_en | base_wr_en;
end
for (gi = 0; gi < PIXELS; gi = gi + 1) begin : g_pixel
    //   α 跟着"这个像素最终取了谁"走（判据用该像素第一个字节的掩码：一个像素 3 字节同生同死）
    assign mix_beat[PIXEL_WIDTH + PIXEL_BYTES + gi*ALPHA_WIDTH +: ALPHA_WIDTH]
         = (line_colorKey_en ? cur_beat[PIXEL_WIDTH + gi*BYTES_PER_PIXEL] : 1'b1)
         ? cur_beat[PIXEL_WIDTH + PIXEL_BYTES + gi*ALPHA_WIDTH +: ALPHA_WIDTH]
         : base_beat[PIXEL_WIDTH + PIXEL_BYTES + gi*ALPHA_WIDTH +: ALPHA_WIDTH];
end
endgenerate

//==============================================================================
// 组合输出
//==============================================================================
//   ★ 旁路：入口直接接出口（data / mask / α 三样都原样过去）
assign imgCombine_data             = comb_on ? mix_beat[PIXEL_WIDTH-1:0]                                   : source_data;
//   ★ 旁路（单图）也要按同一个开关 gate：不开 ck 就是"每个字节都写"
assign imgCombine_mask             = comb_on ? mix_beat[PIXEL_WIDTH +: PIXEL_BYTES]
                                      : (line_colorKey_en ? source_mask : {PIXEL_BYTES{1'b1}});
assign imgCombine_alpha            = comb_on ? mix_beat[PIXEL_WIDTH + PIXEL_BYTES +: PIXELS*ALPHA_WIDTH]   : source_alpha;
//   ★ 合成时 valid 就是"输入这一拍有效"（和上游同拍）；旁路时直接对接 —— 都不看 imgCombine_ready
assign imgCombine_valid            = comb_on ? ((state == S_COMBINE) & source_valid) : source_valid;

//==============================================================================
// 状态机（三段式：状态寄存器 / 次态组合逻辑 / 数据通路时序逻辑）
//==============================================================================

//------------------------------------------------------------------------------
// 第一段：状态寄存器
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        state <= S_EMPTY;
    end
    else begin
        state <= next_state;
    end
end

//------------------------------------------------------------------------------
// 第二段：次态组合逻辑
//------------------------------------------------------------------------------
always @(*) begin
    next_state = state;
    case (state)
        // 缓冲空着：新的一行来了就开始填（不走合成的话一直待在这儿）
        S_EMPTY   : if (line_start & comb_on) next_state = S_FILL;
        // 上游说"图一到此为止" ⇒ 转合成
        S_FILL    : if (line_image_next)           next_state = S_COMBINE;
        // 上游两趟都排空了 ⇒ 这一行的活干完了，缓冲腾出来
        S_COMBINE : if (line_src_free)             next_state = S_EMPTY;
        default   :                           next_state = S_EMPTY;
    endcase
end

//------------------------------------------------------------------------------
// 第三段：数据通路时序逻辑
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        wr_ptr_r  <= {ADDR_W{1'b0}};
        rd_ptr_r  <= {ADDR_W{1'b0}};
        rd_beat_r <= {BEAT_W{1'b0}};
    end
    else begin

        case (state)
            // ---- 等新行：指针归零 ----
            S_EMPTY : if (line_start) begin
                          wr_ptr_r <= {ADDR_W{1'b0}};
                          rd_ptr_r <= {ADDR_W{1'b0}};
                      end

            // ---- 图一那一趟：写缓冲，读指针原地不动 ----
            S_FILL  : if (fill_take) begin
                          row_buf[wr_ptr_r] <= cur_beat;
                          wr_ptr_r          <= wr_ptr_r + 1'b1;
                      end

            // ---- 图二那一趟：每收一拍，读指针往前走一拍 ----
            S_COMBINE : if (take_in) rd_ptr_r <= rd_ptr_r + 1'b1;

            default : ;
        endcase

        //   进 S_COMBINE 那一拍：读指针归零（地址 0 的读由上面 rd_en_go 发出去）
        if (line_image_next) rd_ptr_r <= {ADDR_W{1'b0}};

        //   缓冲读出（同步读：这一拍发，下一拍到；rd_en_go 见上面 assign 区）
        if (rd_en_go) rd_beat_r <= row_buf[rd_addr_go];
    end
end

endmodule
