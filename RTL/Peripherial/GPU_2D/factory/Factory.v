//==============================================================================
//  Factory.v —— 加工面（容器）：像素流进来，AXI 写出去
//------------------------------------------------------------------------------
//  它把 factory/ 里的三级装成一个模块
//
//      各路像素流 ─▶ ┌────────────┐  96b  ┌───────────┐  128b  ┌────────────────┐
//                    │ Compositor │──────▶│ DDRPacker │───────▶│ WriteController│──AW/W/B──▶
//       行任务(广播)─▶│  合成/选路  │       │ 跨拍拼接   │        │   纯写入器      │
//                    └────────────┘       └───────────┘        └────────────────┘
//                        │                     │                      │
//                    src0_rd_en            （无对外）              line_done
//                        │                                            │
//                   line_buffer_free = 各源 free & 写侧 line_wr_free ──────────────┘
//
//  为什么要这一层（★ 不是为好看）
//      1) GPU_2D 只剩"广播 + 接线"：这一层内部三条握手（96→128 的补拍、
//         "本行几拍"、等 B 的收工）不再摊在顶层，顶层也就不会再出现
//         "谁都以为别人算过"的边界错。
//      2) 它是个【可独立验的单元】：喂几路像素流 + 行任务，直接看 AXI 上吐出什么。
//         TB 不必再拼半条链。
//      3) ★ line_buffer_free 的汇总点只能在这里：它要 AND 的两项 ——
//         "各源还能不能接"（在 Compositor）和"写侧写完了没"（在 WriteController）
//         —— 都在这层里面。放到顶层去 AND，就等于把一个内部约定摊到外面。
//
//  对外就三类口（照着"对面是谁"记）
//      行任务口   ：对面是 CmdExec 的广播
//      像素流入口 ：每一路源一组（src0_* …）；加一路源 = 加一组口 + 一行选路
//      AXI 写口   ：对面是 AXIMastersHandler（只报"发几拍"，不碰 AXI 协议）
//
//  完成怎么回
//      ★ 只有 WriteController 有资格报 line_done（"这一行真的落进 DDR 了"）。
//        合成器/打包器数据早就发完了，但那不等于落盘 —— 它们没有 done 可报。
//
//  谁例化它：GPU_2D（顶层只剩"广播 + 接线"，不再出现本层内部的任何一根线）
//
//  参考：docs/spec/硬件_加速器.md §4（数据通路）
//        factory/Compositor.v / DDRPacker.v / WriteController.v（各自的文件头）//  上一级是谁
//      GPU_2D.v（加工面：合成 → 打包 → 写）

//==============================================================================
`timescale 1ns/100ps

module Factory #(
    parameter PIXEL_WIDTH     = 96,                       // 一路像素流一拍 = 4 个 RGB888 像素
    parameter DATA_WIDTH    = 128,                      // AXI 写一拍（16 字节）
    parameter ADDR_WIDTH    = 32,
    parameter LEN_WIDTH     = 8
)(
    //==================== 时钟 / 复位 ====================
    input  wire                             clk,
    input  wire                             rst_n,

    //==================== 行任务口（CmdExec 广播）  ====================
    input  wire                             line_start,     //(I) 单拍脉冲：这一行的任务在这拍有效

    input  wire [6:0]                       line_flags,     //(I) 命令 flags：[4] = 颜色键控(ck) 使能
    input  wire [23:0]                      line_param,     //(I) FILL 的填充色
    //   ★ 图片合成那一路要的两根（都在 Compositor 里做，本层只是透传）
    input  wire                             line_fx_en,     //(I) 本命令有没有第二张图（合成）
    input  wire                             line_image_next,     //(I) 单拍：源② 那条流的"图一到此为止"
    input  wire [ADDR_WIDTH-1:0]            line_dst_addr,  //(I) 这一行的目的起始地址
    input  wire [15:0]                      line_w,         //(I) 这一行多少像素
    output wire                             line_done,      //(O) 单拍脉冲：这一行真的写进 DDR 了

    //==================== 像素流入口：源①（每加一路源就加一组）  ← DDRSource  ====================
    input  wire [PIXEL_WIDTH-1:0]           src0_data,      //(I) 一拍 4 个像素，已对齐到目的字节 0
    input  wire                             src0_valid,     //(I) 这一拍有效（标准 valid/ready）
    output wire                             src0_rd_en,     //(O) 取走一拍（本行不用源① 时恒 0）
    input  wire                             line_src0_free,      //(I) 源① 能接下一行
    //==================== 像素流入口：源②（调色板素材源）  ← PaletteSource  ====================
    input  wire [PIXEL_WIDTH-1:0]           src1_data,      //(I) 一拍 4 个像素，契约同源①
    input  wire [PIXEL_WIDTH/8-1:0]         src1_mask,      //(I) 源② 的字节掩码（ck：1=写）
    input  wire [(PIXEL_WIDTH/24)*8-1:0]    src1_alpha,     //(I) 源② 每像素的 α（8 位/像素，旁路，alpha 混合用）
    input  wire                             src1_valid,     //(I) 这一拍有效
    output wire                             src1_rd_en,     //(O) 取走一拍（本行不用源② 时恒 0）
    input  wire                             line_src1_free,      //(I) 源② 能接下一行

    //==================== 发送节奏汇总（→ CmdExec）====================
    output wire                             line_buffer_free,       //(O) 本命令要的各路源 + 写侧都还能接下一行

    //==================== AXI 写命令口（→ AXIMastersHandler）====================
    output wire                             cmd_wr_valid,   //(O) 提交一笔写
    input  wire                             cmd_wr_ready,   //(I) 可接收
    output wire [ADDR_WIDTH-1:0]            cmd_wr_addr,    //(O) 本笔起始字节地址
    output wire [LEN_WIDTH-1:0]             cmd_wr_len,     //(O) 本笔突发拍数 - 1
    input  wire                             cmd_wr_done,    //(I) 本笔完成（含 B 响应）
    input  wire                             cmd_wr_err,     //(I) 恒 0

    //==================== 写数据口（→ AXIMastersHandler）====================
    output wire [DATA_WIDTH-1:0]            wr_data,        //(O) 一拍 16 字节
    output wire [DATA_WIDTH/8-1:0]          wr_strb,        //(O) 字节使能（首尾遮头）
    output wire                             wr_valid,       //(O) 还有数据要供
    input  wire                             wr_ready        //(I) 写主收下了这一拍
);

//==============================================================================
// wire：子模块各一组（名字用子模块自己的端口名）
//==============================================================================
// ---- Compositor inst ----
wire [PIXEL_WIDTH-1:0]              pixel_data;
wire [PIXEL_WIDTH/8-1:0]            pixel_mask;      // 合成器出口这一拍的字节掩码
wire                                pixel_valid;
wire                                pixel_ready;
wire                                line_buf_free;   // 合成器汇总的各源 free

// ---- DDRPacker inst ----
wire [DATA_WIDTH-1:0]               packer_data;
wire [DATA_WIDTH/8-1:0]             packer_mask;     // 打包之后的一拍掩码（→ wr_strb 的一个与项）
wire                                packer_valid;
wire                                packer_ready;

// ---- WriteController inst ----
wire                                line_wr_free;    // 写侧：本行连 B 都回来了（能接下一行）
//   cmd_wr_* / wr_* 直接就是本模块的端口

//==============================================================================
// assign：子模块 ↔ 本模块的连线（★ 一行一条；左边永远是【被驱动】的那一侧）
//==============================================================================
//   打包器的出账口："写侧真的把这一拍收走了"
assign  packer_ready        = wr_valid & wr_ready;

//   ★ 发送节奏 = 各源能接 且 写侧能接 —— 两条缺一不可
//     各源 free ⟹ 它的 ping-pong 那块已被取空（上一行的数据都进兜了）
//     写侧 free ⟹ 上一行连 B 都回来了（该吐的拍全吐出去了）
//     只算源是不够的：源有两块 ping-pong，本来就是为【预载下一行】，
//     所以"源空"只说明"我有空块"，不说明"上一行的数据已经流完"。
//     这时发下一行任务，DDRPacker 会在 line_start 清兜，
//     把【本行还没吐出去的正经数据】当尾巴丢掉（实测：整段偏移 4 字节）。
assign  line_buffer_free    = line_buf_free & line_wr_free;

//==============================================================================
// 例化
//==============================================================================
//------------------------------------------------------------------------------
// ① 合成器：多路像素流 → 一路像素流（多输入对单输出，自己不存数据）
//------------------------------------------------------------------------------
Compositor #(
    .PIXEL_WIDTH                (PIXEL_WIDTH            )
) u_Compositor (
    .clk                        (clk                    ),
    .rst_n                      (rst_n                  ),

    // cmd 口（上游 = cmd/CmdExec.v 的广播）
    .line_start                 (line_start             ),
    .line_flags                 (line_flags             ),
    .line_done                  (line_done              ),   // ★ 写侧完成：合成器用它把源手上的尾巴丢掉
    .line_param                 (line_param             ),
    .line_fx_en                 (line_fx_en             ),
    .line_image_next            (line_image_next        ),

    // 数据口：源①（DDR 读回的一行）
    .src0_data                  (src0_data              ),
    .src0_valid                 (src0_valid             ),
    .src0_rd_en                 (src0_rd_en             ),
    .line_src0_free             (line_src0_free         ),
    // 数据口：源②（调色板素材源）
    .src1_data                  (src1_data              ),
    .src1_mask                  (src1_mask              ),   // ★ 源给的（ck）字节掩码
    .src1_alpha                 (src1_alpha             ),
    .src1_valid                 (src1_valid             ),
    .src1_rd_en                 (src1_rd_en             ),
    .line_src1_free             (line_src1_free         ),

    .line_buf_free              (line_buf_free          ),   // 各源 free 汇总 → 本层再和写侧相与

    // 数据口：合成后的一路（→ DDRPacker）
    .pixel_data                 (pixel_data             ),
    .pixel_mask                 (pixel_mask             ),
    .pixel_valid                (pixel_valid            ),
    .pixel_ready                (pixel_ready            )
);

//------------------------------------------------------------------------------
// ② 打包器：像素流 96 位/拍 → AXI 写拍 128 位/拍
//      ★ 行边界把"上一行多读回来的尾巴"丢掉，否则下一行的字节会拼错位
//------------------------------------------------------------------------------
DDRPacker #(
    .IN_WIDTH                   (PIXEL_WIDTH            ),
    .OUT_WIDTH                  (DATA_WIDTH             )
) u_DDRPacker (
    .clk                        (clk                    ),
    .rst_n                      (rst_n                  ),

    .line_start                 (line_start             ),
    .line_dst_off               (line_dst_addr[3:0]     ),   // ★ 拍对齐：行首预置 dst_off 个哑字节

    .pixel_data                 (pixel_data             ),
    .pixel_mask                 (pixel_mask             ),   // ★ 掩码和数据同进同出（同一个字节兜）
    .pixel_valid                (pixel_valid            ),
    .pixel_ready                (pixel_ready            ),

    .packer_data                (packer_data            ),
    .packer_mask                (packer_mask            ),
    .packer_valid               (packer_valid           ),
    .packer_ready               (packer_ready           )
);

//------------------------------------------------------------------------------
// ③ 写控制器：一行 → 若干笔 AXI 写（纯写入器，不认识内容）
//------------------------------------------------------------------------------
WriteController u_WriteController (
    .clk_acc                    (clk                    ),
    .rst_acc_n                  (rst_n                  ),

    .line_start                 (line_start             ),
    .line_dst_addr              (line_dst_addr          ),
    .line_w                     (line_w                 ),
    .line_done                  (line_done              ),
    .line_wr_free               (line_wr_free           ),

    .packer_data                (packer_data            ),
    .packer_valid               (packer_valid           ),
    .packer_mask                (packer_mask            ),   // ★ 并进 wr_strb 的第三个与项

    .cmd_wr_valid               (cmd_wr_valid           ),
    .cmd_wr_ready               (cmd_wr_ready           ),
    .cmd_wr_addr                (cmd_wr_addr            ),
    .cmd_wr_len                 (cmd_wr_len             ),
    .cmd_wr_done                (cmd_wr_done            ),
    .cmd_wr_err                 (cmd_wr_err             ),

    .wr_data                    (wr_data                ),
    .wr_strb                    (wr_strb                ),
    .wr_valid                   (wr_valid               ),
    .wr_ready                   (wr_ready               )
);

endmodule
