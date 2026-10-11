//==============================================================================
//  CmdExec.v —— 把"一条命令"展开成"逐行任务"
//------------------------------------------------------------------------------
//  干什么
//      从命令队列取【一整条命令字】，按 h 行逐行推进，每行发一个【自包含】的行任务：
//          行任务 = { 这一行的目的地址, 这一行的源地址, 宽, flags, 参数 }
//          ★ "要哪几路源"不在这里解 —— 见下面"函数"那段的说明
//      发完 h 行、且在飞的都落盘了，才回 IDLE 取下一条。
//      ★ 发送节奏（line_buffer_free）由 GPU_2D 汇总下游表态：各源"有空块" + 写侧"上一行写完了"。
//        源那边有两块 ping-pong，本来就是为预载下一行；所以只算源是不够的 —— 详见 GPU_2D 顶部注释。
//      ★ 它不认识 DDR、不碰 AXI、不算字节/拍数/4KB —— 更没见过一个像素
//      ★ 单时钟域（clk_acc，和命令队列的读侧同域）

//  命令怎么用（★ 接口没变，只是几根参数线里并排了好几格）
//      队列出口还是那几根线：类型 / 源 / 目的 + 宽 / 高 / flags / 参数（args）。
//      每张图才有的那三根线（源地址 / 参数 / 模式）里并排放着 IMAGE_NUM 个格：
//          第 1 格 = 第 1 条参数 CI（原图那一份）—— 就是以前一直用的那几个字段
//          第 2 格 = 第 2 条参数 CI（特效那一份）—— 本模块现在还不用，以后合成时取
//      所以这里只是把"第 1 格"取个名字（_base 后缀），下面的状态机、地址递推一个字没改。
//      ★ 镜像（flags[5]=MX 水平 / flags[6]=MY 垂直）也在这里体现，但只改【源地址怎么给】：
//          MX → 广播的是"本行行尾那个字"（行首 + ceil(w/4) − 1），取出器每读一个字 −1；
//          MY → 起点放到最后一行，之后每行 −stride；目的地址照旧正向推进。
//        ★ 起点全在这算：软件侧的地址约定不变，取出器也不需要知道 w。
//
//  这一版相对 M1 改了五处（都是为了"读侧不要在写侧干活时全程空转"）
//
//   1) 行任务的【下发】不再等 line_done
//        M1:  发一行 → 死等 line_done → 才发下一行   ← 等的时候读侧完全空转
//        现在: 发一行 → 只要【下游还有空块】(line_buffer_free) 就继续发
//        于是两个信号各管一件事:
//            line_buffer_free  → 能不能发下一行（发送节奏，看下游空间）
//            line_done → 命令完成了没  （真的落盘了，决定何时收下一条命令）
//        ★ 只等 line_buffer_free 就宣告完成是错的：数据还在往 DDR 的路上就把命令收了，
//          基址/模式可能已经变了
//
//   2) 地址和行号在【发任务那一拍】推进，不再等 line_done
//        地址跟着"发出去的行"走，不跟着"完成的行"走 —— 这是能重叠的前提
//        N=1 时两者等价（发完就等，等完再发）
//
//   3) 模式 → 源使能的解码放在这里，而且只此一处
//        本模块是除 CIUnpack 之外唯一见过完整命令的地方。
//        放这儿，每条读通路只收【一位】（"要不要我"），解码不会散成几份
//        必须永远保持一致。以后加 alpha / ColorKey，只改这一个 case
//
//   4) 删掉 line_op
//        它的消费者都没了：读侧改收 src_en、合成器看模式、写侧本来就不看它
//        line_flags 改成【无条件转发】—— FILL 也是一种模式，flags 对它同样有效
//
//   5) 不再需要"在飞行数上限"这个参数
//        下游空不空由 line_buffer_free 直接告诉，不用自己按"ping-pong 有几块"去推
//        完成计数 line_done_cnt_r 保留，但它只用来判"命令真的完成了没"
//
//  两个"绝不能出错"的地方（M1 就有，这版保留）
//      · 高为 0 的命令必须挡在门外（draw_cmd）：
//        h=0 时 more_line 恒不成立，而进 S_SEND 是无条件的 —— 会白发一行
//        上游已保证 h≥1，这里再挡一道
//      · 一条命令期间 lineBuffer_mode 必须稳定：上游在帧边界锁存（见顶层），
//        dst_stride 由它派生，源和目的共用同一个值
//
//  ★ S_IDLE 为什么不查 line_buffer_free
//      看着该加，其实加不了：
//        - 只改次态 → 命令已被 FIFO 弹出却发不出去，直接丢命令
//        - 改成 cmd_ready 带 line_buffer_free → S_IDLE 时手上还是
//          【上一条命令】的上下文，拿它去问"要的几路够不够"本身就是错的
//      真正靠的是这个不变量：能回 S_IDLE ⟹ 上一条命令全部落盘 ⟹ 各源必然空
//
//  参考: docs/spec/硬件_加速器.md §3（命令生命周期）
//  上一级是谁
//      cmd/CmdChannel.v —— 它把本模块和 CmdBuffer 装成"命令通道"。
//==============================================================================
`timescale 1ns/100ps

module CmdExec #(
    parameter LINE_BYTES = 5760,                // 单段行宽（全屏 1920×3）；分屏取一半
    parameter IMAGE_NUM  = 2                    // 一条命令几张图（★ 必须与 CIUnpack 一致）
)(
    //==================== 时钟 / 复位 ====================
    input  wire                     clk_acc,            //(I) 命令侧时钟（与 CmdBuffer 读侧同域）
    input  wire                     rst_acc_n,          //(I) 异步复位，低有效

    //==================== 命令队列出口（接 CmdBuffer）  ====================
    input  wire                     cmd_valid,          //(I) 队列非空：有命令可取
    output wire                     cmd_ready,          //(O) 空闲，收下这一条
    input  wire [2:0]               cmd_type,   //(I) 3'd1=FILL / 3'd2=BLIT / 3'd3=FENCE
    input  wire [32*IMAGE_NUM-1:0]  cmd_src,    //(I) 每张图的源地址：第 1 格 = 原图
    input  wire [31:0]              cmd_dst,    //(I) 目的地址（块左上角）
    input  wire [15:0]              cmd_w,      //(I) 宽（目标矩形，几张图共用）
    input  wire [15:0]              cmd_h,      //(I) 高（目标矩形，几张图共用）
    input  wire [7*IMAGE_NUM-1:0]   cmd_flags,  //(I) 每张图的模式位：第 1 格 = 原图
    input  wire [32*IMAGE_NUM-1:0]  cmd_args,  //(I) 每张图自己的参数：第 1 格 = 原图（FILL 时是纯色）

    //==================== 配置口 ====================
    //   目的跨距由它在内部派生（和显示侧同一个来源）。
    //   ★ 源跨距不再恒等于目的跨距：素材（PRAM 索引图）一行只有"素材宽"那么长，
    //     所以改用命令的 color 字段带下来（见 src_stride_of）。
    input  wire                     lineBuffer_mode,    //(I) 0=全屏(5760) 1=分屏(2880)，和显示侧同源

    //==================== 行任务口 → 各源 / 合成器 / 写侧 ====================
    output wire                     line_start,         //(O) 单拍脉冲：这一行的任务在这拍有效
    output wire [31:0]              line_dst_addr,      //(O) 本行的目的起始地址（字节）—— 几张图共用
    output wire [15:0]              line_w,             //(O) 本行多少像素 —— 几张图共用
    output wire                     line_fx_en,         //(O) 本命令有没有第二张图（合成）：给 imageCombine / 旁路用
    //   ★ 下面三根【每张图一格】（和命令侧一个形状）：第 1 格 = 图一，第 2 格 = 图二…
    output wire [32*IMAGE_NUM-1:0]  line_src_addr,  //(O) 每张图那一行的源起始地址；该图不要源时它那格恒 0
    output wire [7*IMAGE_NUM-1:0]   line_flags,     //(O) 每张图那格的模式位，原样转发
    output wire [24*IMAGE_NUM-1:0]  line_param,     //(O) 每张图那格的参数：素材源行跨距 / FILL 的填充色
    //   ★ "看最后一格"的三根：背景源 / 合成器 / 写侧用的都是【最终结果】那一步的模式 ——
    //     合成时 = 第 2 张图那格；单图命令第 2 格是空的 ⇒ 用 line_fx_en 选回第 1 格。
    //   ★ 这个选择放在本模块（广播归本模块所有）：顶层 GPU_2D 只连线，不摆逻辑。
    output wire [31:0]              line_src_addr_last,    //(O) 最后一格那张图的源起始地址
    output wire [6:0]               line_flags_last,       //(O) 最后一格那格的模式位
    output wire [23:0]              line_param_last,       //(O) 最后一格那格的参数
    input  wire                     line_done,          //(I) 单拍脉冲：这一行【已经写进 DDR 了】
    input  wire                     line_buffer_free,   //(I) 下游（各源 + 写侧）都还能接下一行（合成器汇总后回传）

    //==================== 观测口（给 APB 寄存器，v1 只观测不影响功能）  ↔ APB0_GPU_2D（总线层寄存器格子）  ====================
    output wire                     acc_busy,           //(O) 有命令在手上（没做完）
    output wire                     cmd_done,           //(O) 单拍：这一条命令真做完了（含落盘）
    output wire                     frame_done          //(O) 单拍：FENCE 完成 = 本帧画完，可以交帧
);

//==============================================================================
// localparam
//==============================================================================
localparam  CMD_FILL        = 3'd1;
localparam  CMD_BLIT        = 3'd2;
localparam  CMD_FENCE       = 3'd3;

//   合成模式：命令里的 cmd_flags[2:0]。★ 本模块【只原样广播】（line_flags），不解释它
localparam  MODE_FILL       = 3'd0;             // 纯色：不要源
localparam  MODE_REPLACE    = 3'd1;             // 覆盖：源①（DDR 读回的那一路）
localparam  MODE_ALPHA      = 3'd2;             // ★ alpha 混合：源①(读回的目的) + 源②(调色板精灵)
localparam  MODE_CKEY       = 3'd3;             // 颜色键控：同 alpha —— 以后

//   本模块现在只用【第 1 格】（= 第 1 条参数 = 原图那一份），
//   所以格宽就是下面的字面位数，第 2 格（特效）等合成做的时候从 cmd_*[31:16] 之类取

localparam  S_IDLE          = 2'd0;             // 看队列，取命令
localparam  S_SEND          = 2'd1;             // 发一行任务（只停一拍）
localparam  S_RUN           = 2'd2;             // 发与收并行

//==============================================================================
// 函数
//==============================================================================
//   ★ 模式（flags[2:0]）这件事【本模块不再解码给下游用】。
//     谁要不要动，由各源自己看广播出去的 line_flags 决定：
//         DDRSource      : "源① 这一行要不要我 / 我读源行还是读目的行"
//         PaletteSource  : "源② 这一行要不要我"
//         Compositor     : "取哪一路 / 要不要混合"
//     本模块只管【地址怎么递推】（line_src_r 是它的），不替别人做选路决定。
//     ★ 改模式表要一起改那三处，权威表在 docs/spec/硬件_加速器.md §1.3。

//==============================================================================
// wire / reg
//==============================================================================
// ---- 宽 / 高 / 目的 / 类型 是几张图【共用】的，本来就只有一份，直接用端口名；
//      源地址 / 参数 / 模式 每张图各一格，下面都按"格"处理（generate / for 循环）----

// ---- 状态机 ----
reg  [1:0]                          state;
reg  [1:0]                          next_state;

// ---- 命令上下文（取命令那一拍锁存）----
//   ★ 这几个必须锁存：命令从队列出来只在【一个拍】有效, 而行任务要重复广播 h 次
reg  [15:0]                         cmd_w_r;                        // 宽（像素）
reg  [15:0]                         cmd_h_r;                        // 高 = 循环次数
reg                                 line_fx_en_r;                   // 本命令有没有第二张图（取命令那拍算好）
reg  [7*IMAGE_NUM-1:0]              cmd_flags_r;            // 每张图的模式位（原样转发）
reg  [24*IMAGE_NUM-1:0]             cmd_param_r;            // 每张图的参数（原样转发）
reg  [24*IMAGE_NUM-1:0]             cmd_src_stride_r;       // 每张图的源行跨距（取命令那拍算好）

// ---- 行推进（★ 每张图各推各的：各自跨距、各自 MX/MY 起点）----
reg  [15:0]                         line_y_r;                       // 已经【发出去】几行（0 … h）
reg  [31:0]                         line_dst_r;                     // 下一行的目的地址（累加，几张图共用）
reg  [32*IMAGE_NUM-1:0]             line_src_r;             // 每张图下一行的源地址（各推各的）
reg  [15:0]                         line_done_cnt_r;                // 已经【写进 DDR】几行

// ---- 判断组合条件 ----
wire                                take_cmd;                       // 这一拍取走一条命令
wire                                draw_cmd;                       // 这条命令要画东西（FILL/BLIT 且高不为 0）
wire                                more_line;                      // 还有行没发出去
wire                                can_send;                       // 这一拍可以发下一行
wire                                cmd_finished;                   // 这条命令真的做完了
wire [31:0]                         dst_stride;                     // 目的行跨距（由 mode 选）

// ---- 镜像(MX/MY)的中间量：★ 每张图各一份 ----
//   ★ 这里只【声明】，赋值在下面的 assign 区 —— 声明和连线不放在一起
//   ★ 都做成【打包向量】（每张图一格），不是 unpacked 数组 —— 数组在 generate 里
//     用 genvar 索引 ModelSim 不认；打包向量的变址 part-select 到处都能用
wire [24*IMAGE_NUM-1:0]             lane_take_stride;             // 取命令那拍算出：该图源行跨距
wire [40*IMAGE_NUM-1:0]             lane_take_last_row;           // MY 用：(h−1) × 该图源行跨距
wire [32*IMAGE_NUM-1:0]             lane_row_last_word;           // MX 用：该图本行行尾字
wire [IMAGE_NUM-1:0]                lane_uses_src;                // 该图这一行要不要源（决定它那格填不填）

integer i;                                            // for 循环用（按图跑）

//==============================================================================
// assign 区：配置解码（静态连线，不依赖状态）
//==============================================================================
//   目的行跨距 = 画布一行的字节数，和显示侧的 mode 同一个来源
//   全屏 1920×3 = 5760；分屏 960×3 = 2880
assign dst_stride           = lineBuffer_mode ? (LINE_BYTES/2) : LINE_BYTES;


//   源行跨距怎么来（★ 与目的跨距【无关】）
//     flags[3]=0（DDR RGB888 源，含 FILL）：恒为目的跨距 —— 【老行为，一个字都不变】
//     flags[3]=1（调色板 PRAM 索引源）  ：命令的 color 字段 = 源行跨距（按【字】）；
//                                        为 0 时退回目的跨距
//   ★ 为什么不能用 w 推：w 是"本次画多宽"（可以小于素材宽 = 裁剪），
//     而跨距是素材自己的行跨度 —— 两者是独立的两个量，所以必须由命令带下来。
//   ★ 为什么按 flags[3] 分而不是按 mode 分：DDR 源那条路已经上板验过，
//     color 字段对它是"原样转发"，动它就等于改老行为（回归 TB 立刻报错）。
function [31:0] src_stride_of;
    input [6:0]  flags;
    input [23:0] color;
    input [31:0] dst;
    begin
        //   ★ 参数 24 位里只有低 12 位是【源行跨距】，高 12 位是【特效参数】
        //     （色调模式 / 强度 / 上下，见 docs/spec/硬件_加速器.md §1.3.1）——
        //      这里只吃低 12 位，高 12 位原样跟着 line_param 广播下去。
        if (flags[3] == 1'b0)          src_stride_of = dst;                // DDR 源 / FILL：不变
        else if (color[11:0] == 12'd0) src_stride_of = dst;                // 跨距 0 = 沿用目的跨距
        else                            src_stride_of = {20'd0, color[11:0]};  // 调色板源：字
    end
endfunction

//==============================================================================
// 判断组合条件（状态机与组合输出都用这些名字，不再写长表达式）
//==============================================================================
// 空闲就收：exec 只在 S_IDLE 等命令，其余状态一律不收
assign cmd_ready            = (state == S_IDLE);
assign take_cmd             = cmd_valid & cmd_ready;

//   要画东西的命令才开工：FILL / BLIT 且 高不为 0
assign draw_cmd             = ((cmd_type == CMD_FILL) || (cmd_type == CMD_BLIT)) && (cmd_h != 16'd0);

//   还有行没发出去（line_y_r 数的是【发出去】的行，不是完成的行）
assign more_line            = (line_y_r < cmd_h_r);

//   能不能发下一行：还有行 + 下游有空块
//     ★ 不等 line_done —— 那正是"读侧空转"的来源
assign can_send             = more_line & line_buffer_free;

//   命令真的做完了：收到的 done 数 = 总行数
//     ★ 不能只看"最后一行发出去了" —— 完成虽然保序, 但最后一行发出时前面
//       还可能欠着几笔 done(h=3 时尤其明显), 那时判完成会提前收命令
assign cmd_finished         = (line_done_cnt_r == cmd_h_r);

//==============================================================================
// 观测输出（给 APB 寄存器）
//==============================================================================
//   ★ 三条都不参与控制，只往外报 —— 加/删它们不会影响任何时序
//
//   acc_busy   ：手上有命令没做完（含 FENCE 那一拍）。
//                给软件一个"现在能不能发下一条"的最便宜判据。
//   cmd_done   ：这一条命令真做完那一拍。因为 cmd_finished 会把它踢回 S_IDLE，
//                所以这个条件天然只成立一拍（单拍脉冲）。
//   frame_done ：★ FENCE 被取走的那一拍 = 本帧画完。
//                为什么这一拍就代表"本帧都落盘了"：
//                  · FENCE 排在命令队列里，顺序有保证（前面的命令都已经被 CmdExec 取走）
//                  · 而 CmdExec 只有 cmd_finished（全部行的 line_done 都回来了）才回 S_IDLE
//                  ⇒ 能取到 FENCE，就说明前面所有绘制【真的写进 DDR 了】
//                这条脉冲直接送给显示侧（APB0 槽 2 显示子块的 acc_frame_done）→ 置 DONE_ACC
//                → 下一个帧边界翻页 + 帧率统计。全程 CPU 不用轮询。
assign acc_busy             = (state != S_IDLE);
assign cmd_done             = (state == S_RUN) & cmd_finished;
assign frame_done           = take_cmd & (cmd_type == CMD_FENCE);

//==============================================================================
// 组合输出
//   ★ 行任务字段只在 line_start = 1 那一拍有效（下游当拍锁存）
//   ★ 与当前模式无关的字段填 0，免得下游拿到上一条命令的残值
//==============================================================================
assign line_start           = (state == S_SEND);       // 只停一拍 → 天然是单拍脉冲
//   ★ 本模块只关心一件事："这条命令用不用源" —— 决定 line_src_addr 要不要填。
//     用不用源是模式说了算（1/2/3 用源，0=FILL 和未定义模式不用）。
//     ★ 用哪一路、要不要混合，是各源和合成器自己的事（见上面"函数"那段的说明）。
assign uses_src             = (cmd_flags_r[2:0] == MODE_REPLACE) |
                              (cmd_flags_r[2:0] == MODE_ALPHA  ) |
                              (cmd_flags_r[2:0] == MODE_CKEY   );

assign line_dst_addr        = line_dst_r;
assign line_w               = cmd_w_r;
assign line_fx_en           = line_fx_en_r;

//   ★ 最后一格的三根：单图命令（line_fx_en = 0）没有第 2 格 ⇒ 取第 1 格
assign line_src_addr_last   = line_fx_en_r ? line_src_addr[32*(IMAGE_NUM-1) +: 32]
                                           : line_src_addr[31:0];
assign line_flags_last      = line_fx_en_r ? line_flags[7*(IMAGE_NUM-1) +: 7]
                                           : line_flags[6:0];
assign line_param_last      = line_fx_en_r ? line_param[24*(IMAGE_NUM-1) +: 24]
                                           : line_param[23:0];

//   ★ 每张图一格：源地址 / 模式 / 参数都按图发下去（和命令侧一个形状）
//     MY 起点偏移 (h−1) × 该图源行跨距：每条命令、每张图各算一次
generate
genvar gi;
    for (gi = 0; gi < IMAGE_NUM; gi = gi + 1) begin : g_lane
        wire [6:0]  take_flags;
        assign take_flags = cmd_flags[7*gi  +: 7];
        wire [31:0] take_addr;
        assign take_addr = cmd_src  [32*gi +: 32];

        // 取命令那一拍用【端口值】算（那拍命令字是有效的）
        assign lane_take_stride[24*gi +: 24]   = src_stride_of(take_flags, cmd_args[32*gi +: 24], dst_stride);
        assign lane_take_last_row[40*gi +: 40] = ({24'd0, cmd_h} - 40'd1) * {16'd0, lane_take_stride[24*gi +: 24]};
        //   ★★ 广播这一侧必须看【锁存的】上下文，不能看端口：
        //      命令队列是 FWFT 预读的 —— CmdExec 取走命令那一拍，端口上立刻换成【下一条命令】，
        //      而行任务要发 h 次。用端口就会把下一条命令的模式位当成这一条的（踩过）
        assign lane_uses_src[gi]               = (cmd_flags_r[7*gi +: 3] == MODE_REPLACE) |
                                                (cmd_flags_r[7*gi +: 3] == MODE_ALPHA  ) |
                                                (cmd_flags_r[7*gi +: 3] == MODE_CKEY   );

        //   ★ MX（水平镜像）的"倒读起点"在这里算 —— 取出器只负责"每读一个字 −1"。
        //     本图这一行的【行首字】是 line_src_r[gi]，镜像时广播它的【最后一个字】：
        //         行尾字 = 行首字 + ceil(w/4) − 1
        //     ★ 约束：行内像素数按 4 对齐（素材宽本来就是 4 的倍数）。
        assign lane_row_last_word[32*gi +: 32] = line_src_r[32*gi +: 32]
                                            + {16'd0, (cmd_w_r + 16'd3) >> 2} - 32'd1;

        assign line_src_addr[32*gi +: 32] = lane_uses_src[gi]
                                        ? (cmd_flags_r[7*gi + 5] ? lane_row_last_word[32*gi +: 32]
                                                                : line_src_r[32*gi +: 32])
                                        : 32'd0;                       // 这张图不要源就填 0
        assign line_flags[7*gi +: 7]      = cmd_flags_r[7*gi +: 7];      // 无条件转发
        assign line_param[24*gi +: 24]    = cmd_param_r[24*gi +: 24];    // 无条件转发
    end
endgenerate

//   ★ MX（水平镜像）的"倒读起点"在这里算 —— 取出器只负责"每读一个字 −1"。
//     line_src_r 是这一行的【行首字】，镜像时广播它的【最后一个字】：
//         行尾字 = 行首字 + ceil(w/4) − 1        （w = 本行画多少像素，CI 第 2 条给的）
//     所以取出器【不需要知道 w】，也不需要自己算行尾。
//     ★ 约束：行内像素数按 4 对齐（素材宽本来就是 4 的倍数）——
//       否则倒读的第一个字会从行外多取 1~3 个像素。
//   ★ 源① 读回目的（alpha 的背景）这件事不在这里出信号：
//     DDRSource 自己看它那格 flags 决定"读源行还是读目的行"，
//     目的行地址就是广播出去的 line_dst_addr。

//==============================================================================
// 状态机
//==============================================================================

//------------------------------------------------------------------------------
// 状态寄存器
//------------------------------------------------------------------------------
always @(posedge clk_acc or negedge rst_acc_n) begin
    if (!rst_acc_n)
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
        S_IDLE : if (take_cmd)          next_state = draw_cmd ? S_SEND : S_IDLE;
        S_SEND :                        next_state = S_RUN;             // 发任务只占一拍
        S_RUN  : if (can_send)          next_state = S_SEND;            // 下游有空就继续发
                 else if (cmd_finished) next_state = S_IDLE;            // 发完 + 落盘完 → 收工
        default:                        next_state = S_IDLE;
    endcase
end

//------------------------------------------------------------------------------
// 数据通路时序逻辑
//     取命令 / 发任务 / 收完成 分别挂在不同状态上，天然互斥
//------------------------------------------------------------------------------
always @(posedge clk_acc or negedge rst_acc_n) begin
    if (!rst_acc_n) begin
        cmd_w_r         <= 16'd0;
        cmd_h_r         <= 16'd0;
        line_fx_en_r    <= 1'b0;
        line_y_r        <= 16'd0;
        line_done_cnt_r <= 16'd0;
        line_dst_r      <= 32'd0;
        for (i = 0; i < IMAGE_NUM; i = i + 1) begin
            cmd_flags_r[7*i +: 7]      <= 7'd0;
            cmd_param_r[24*i +: 24]    <= 24'd0;
            cmd_src_stride_r[24*i +: 24] <= 24'd0;
            line_src_r[32*i +: 32]     <= 32'd0;
        end
    end
    else begin
        // ---- 这一行写进 DDR 了：完成数加一 ----
        //     注意：地址和行号【不在这里】推进 —— 它们跟着"发出去的行"走
        if (line_done)
            line_done_cnt_r <= line_done_cnt_r + 16'd1;

        case (state)
            // ---- 取命令：锁存上下文 + 装载两个地址（行号归零）----
            S_IDLE : if (take_cmd) begin
                         cmd_w_r     <= cmd_w;
                         cmd_h_r     <= cmd_h;
                         line_y_r    <= 16'd0;
                         line_done_cnt_r <= 16'd0;
                         line_dst_r  <= cmd_dst;
                         //   ★ 本命令有没有第二张图（合成）：第 2 格起只要有任何信息就算有
                         //     （单图 / FILL 命令里 CIUnpack 会把第 2 格整段清零 ⇒ 这里是 0）
                         line_fx_en_r <= (cmd_type != CMD_FENCE) &
                                        (|cmd_flags[7*IMAGE_NUM-1:7] | |cmd_src[32*IMAGE_NUM-1:32]);
                         for (i = 0; i < IMAGE_NUM; i = i + 1) begin
                             cmd_flags_r[7*i +: 7]         <= cmd_flags[7*i +: 7];
                             cmd_param_r[24*i +: 24]       <= cmd_args [32*i +: 24];
                             cmd_src_stride_r[24*i +: 24]  <= lane_take_stride[24*i +: 24];
                             //   ★ MY（垂直镜像）：这张图的源地址从【最后一行】开始，
                             //     之后每行往回走一个跨距。目的地址照旧正向推进。
                             //     (h−1)×stride 每条命令、每张图只算一次（不是每像素）。
                             line_src_r[32*i +: 32]        <= cmd_flags[7*i + 6]
                                                            ? (cmd_src[32*i +: 32] + lane_take_last_row[40*i +: 32])
                                                            :  cmd_src[32*i +: 32];
                         end
                     end

            // ---- 发一行任务那一拍：行号++ / 两个地址各推进一个跨距 ----
            S_SEND : begin
                         line_y_r    <= line_y_r + 16'd1;
                         line_dst_r  <= line_dst_r + dst_stride;
                         //   ★ 每张图各推各的：各用自己的跨距；MY 时取负（往回走一行）
                         //   ★★ 例外：纯色层（flags[2:0]=3）那一格带的【不是地址，是颜色】
                         //      （Compositor 拿它当常量色用）⇒ 那一格【不递推】，每行都得是同一个颜色。
                         //      递推了的话第 2 行起颜色就变了（症状：闪红一行一个色、越往下越怪）。
                         for (i = 0; i < IMAGE_NUM; i = i + 1)
                             line_src_r[32*i +: 32] <= (cmd_flags_r[7*i +: 3] == MODE_CKEY)
                                                     ?  line_src_r[32*i +: 32]
                                                     : (cmd_flags_r[7*i + 6]
                                                        ? (line_src_r[32*i +: 32] - {8'd0, cmd_src_stride_r[24*i +: 24]})
                                                        : (line_src_r[32*i +: 32] + {8'd0, cmd_src_stride_r[24*i +: 24]}));
                     end
            default: ;
        endcase
    end
end

endmodule
