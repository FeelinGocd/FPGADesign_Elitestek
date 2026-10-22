//==============================================================================
//  PaletteLUT.v —— 索引 → RGB888 的只读查找表（一拍查 4 个，带握手）
//------------------------------------------------------------------------------
//  干什么
//      收进 4 个索引（32 位）+ 一个 valid，下一拍吐出 4 个 RGB888 像素（96 位）。
//      ★ 输出宽度正好等于现有像素流的一拍（12 字节）—— 下游一行都不用改。
//
//  它是【带握手的读口】，不是"组合地址进、寄存数据出"的裸表
//      入口：index_data / index_valid / index_ready       给我一个索引字
//      出口：palette_data / palette_valid / palette_ready 这一拍 4 个像素，保持到被取走
//      "查表要花一拍"被封在这里，谁用谁不用自己算延迟；出口会一直保持到 palette_ready，
//      所以下游不需要任何 skid。
//      （早先 4 个读口的读使能恒为 1 —— 每个时钟都在重新查表、输出留不住，
//        下游只能补一级 skid 兜着。现在读使能由入口握手驱动，那个 skid 就删掉了。）
//
//  为什么没有写口
//      调色板在【生成 bit 阶段】就烧进初值：索引、调色板、素材表全部由 C 代码 +
//      tools/img2hex/pic2pram.py 生成初值文件，综合时走 BlockRAM 的 INIT_FILE_PATH 进 RAM。
//      运行期不装载、不写、也就没有竞态 ⇒ 这里一根写线都不留。
//
//  为什么一拍查 4 个
//      写侧实测约 2.19 周期/拍，折合需要 ≳2.44 像素/拍才跟得上；一拍查 1 个只有
//      0.75 像素/拍，差一个数量级。一拍查 4 个 = 4 像素/拍 = 12 字节/拍，
//      正好喂满 96 位出口。
//
//  为什么是 4 份 PaletteRAM
//      一个 BRAM 只有【一个】读口（见 BlockRAM.v），而我们要一拍读 4 个不同索引
//      ⇒ 复制 4 份，每份装同一张表、各读各的地址。表本身只有 256×24 = 6 Kbit，
//      4 份 24 Kbit，对空闲 160 块 RAM 来说可以忽略。
//      ★ 4 份共用同一个初值文件：读口 → 输出 lane 的对应在 RTL 里就是写死的
//        （第 i 份的输出接 palette_data[i*24 +: 24]），综合器没有重排的余地。
//      ★ 4 份【锁步】：同一个 clk / clear，同一个读请求(lane_take)，
//        同一个 read_ready(palette_ready) ⇒ 内部状态永远一致，
//        所以 palette_valid / index_ready 取第 0 份的就行。
//
//  接口契约
//      · 标准 valid/ready，入口和出口各一套
//      · clear 只在行边界那一拍有效（丢掉上一行残留的那个像素字）
//
//  ★ 字节序（全工程统一，别改）
//      调色板文件里一项写 RRGGBB，但输出的 96 位像素字里【低位字节 = B】——
//      显示侧 dvi_encoder_Top 拿 [7:0] 当蓝、[15:8] 当绿、[23:16] 当红
//      （帧缓存里就是 B,G,R 这个序）。本模块原样输出 0xRRGGBB，不做字节翻转。
//
//  参考：LineBuffer/PingPongBuffer/BlockRAM.v、docs/spec/硬件_加速器.md §7.3（调色板容量）//  上一级是谁
//      pixel/palette/PaletteDecoder.v

//==============================================================================
`timescale 1ns/100ps

module PaletteLUT #(
    parameter INDEX_WIDTH       = 8,                // 索引位宽（8bpp ⇒ 256 项）
    parameter COLOR_WIDTH       = 24,               // 一项的颜色位宽（RGB888；ARGB 留给 alpha）
    parameter INDEXES_PER_WORD  = 4,                // 一个索引字里几个索引（= 一拍出几个像素）
    parameter INIT_FILE         = "palette.hex"     // 调色板初值文件（4 份副本共用）
)(
    //==================== 时钟 / 复位 ====================
    input  wire                             clk,
    input  wire                             rst_n,
    input  wire                             clear,          //(I) 单拍：丢掉手上这个像素字（行边界用）

    //==================== 入口：一个索引字 ====================
    input  wire [INDEXES_PER_WORD*INDEX_WIDTH-1:0]  index_data,     //(I) 一拍要查的 4 个索引
    input  wire                             index_valid,    //(I) index_data 有效
    output wire                             index_ready,    //(O) 这一拍能收一个新索引字

    //==================== 出口：4 个像素 ====================
    output wire [INDEXES_PER_WORD*COLOR_WIDTH-1:0]  palette_data,   //(O) 4 个 RGB888（查表延 1 拍）
    output wire                             palette_valid,  //(O) 与 palette_data 同拍，保持到 palette_ready
    input  wire                             palette_ready   //(I) 下游把这 4 个像素取走了
);

//==============================================================================
// wire / reg
//==============================================================================
//   4 份读口在锁步，逐份的信号收在下面这两根向量里
wire [INDEXES_PER_WORD-1:0]     lane_accept;            // 各份读口这一拍能不能受理
wire [INDEXES_PER_WORD-1:0]     lane_valid;             // 各份读口手上有没有货
wire                            lane_take;              // 这一拍真的收下一个索引字

//==============================================================================
// 判断组合条件
//==============================================================================
assign  lane_take = index_valid & index_ready;

//==============================================================================
// 组合输出
//==============================================================================
//   ★ 4 份锁步 ⇒ 这两根取第 0 份即可
assign  index_ready   = lane_accept[0];
assign  palette_valid = lane_valid[0];

//==============================================================================
// 例化
//==============================================================================
//------------------------------------------------------------------------------
// 4 份表，每份一个读口（generate 展开：相互之间只差"读第几个索引/接哪条 lane"）
//------------------------------------------------------------------------------
genvar gi;
generate
    for (gi = 0; gi < INDEXES_PER_WORD; gi = gi + 1) begin : g_lane
        PaletteRAM #(
            .DATA_WIDTH    (COLOR_WIDTH),
            .ADDRESS_WIDTH (INDEX_WIDTH),
            .INIT_FILE     (INIT_FILE  )
        ) u_PaletteRAM (
            .clk            (clk),
            .rst_n          (rst_n),
            .clear          (clear),
            .read_request   (lane_take),
            .read_accept    (lane_accept[gi]),
            .read_address   (index_data[gi*INDEX_WIDTH +: INDEX_WIDTH]),
            .read_data      (palette_data[gi*COLOR_WIDTH +: COLOR_WIDTH]),
            .read_valid     (lane_valid[gi]),
            .read_ready     (palette_ready)
        );
    end
endgenerate

endmodule
