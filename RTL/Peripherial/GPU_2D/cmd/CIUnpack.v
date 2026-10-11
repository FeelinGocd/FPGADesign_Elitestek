//==============================================================================
//  CIUnpack.v —— 把一条命令的几条 CI 拼成【一条命令】发出去
//------------------------------------------------------------------------------
//  职责（就这一件事）
//      CI 一条一条进来 → 按功能号分成三类 → 攒成一条命令 → 写进命令队列
//          命令开始（FILL / BLIT）：这条命令画多大（宽高）+ 画到哪（目的地址）
//          参数 CI（ARGS）       ：第 k 条 = 第 k 张图的资料（源地址 + 参数 + 模式）
//          单 CI 命令（FENCE）   ：它自己就是一条完整命令，直接推出去
//      其余（NOP / 未知功能号 / 组号非 0）→ 什么都不做
//      ★ 它不认识 DDR、不认识帧、不碰 AXI
//      ★ 纯单时钟域（CI 侧 = CPU 时钟域），没有任何跨域逻辑

//  为什么要好几条 CI
//      一条 CI 只有两个 32 位参数（64 位），装不下一张图的全部资料，
//      所以"几张图就拆几条 CI"：一条命令开始 + 每张图一条参数 CI。
//      ★ 命令开始那条不给图，它给这条命令【共用】的东西：宽高 + 目的地址。

//  出口长什么样（★ 每张图都有的东西才并排，几张图就几格 IMAGE_NUM）
//      一份（几张图共用）：
//          cmd_type [2:0]   命令类型
//          cmd_dst  [31:0]  目的地址（几张图合成到同一块地方）
//          cmd_w    [15:0]  宽（目标矩形）
//          cmd_h    [15:0]  高（目标矩形）
//      一格 = 一张图：
//          cmd_src   [32*IMAGE_NUM-1:0]  每张图的源地址（32 位一格）
//          cmd_args  [32*IMAGE_NUM-1:0]  每张图自己的参数（就是原来那根 color 线：
//                                        FILL 时是纯色，其它指令下看指令自己怎么定义）
//          cmd_flags [7*IMAGE_NUM-1:0]   每张图的模式位
//      ★ 并排是按【图】并排：第 k 格 = 第 k 张图（第 1 张 = 原图，第 2 张 = 特效图）。
//      ★ 于是"多条 CI"不用另立一套名字，也不用把参数揉成一根大字：
//        地址还是地址那根线，数据还是数据那几根线，只是每根线多留了几格。

//  每条 CI 的参数怎么落位（= v1 的软件约定，位置是"在 CI 参数里"）
//      命令开始（FILL / BLIT）：inputs_0 = { 宽[31:16], 高[15:0] }
//                              inputs_1 = 目的地址
//      参数 CI（ARGS）        ：inputs_0 = 这一张图的源地址
//                              inputs_1 = { more[31], flags[30:24], 参数[23:0] }
//      ★ 第 k 条参数 CI = 第 k 张图 ⇒ 它的地址写进 cmd_src 第 k 格，依此类推
//        （所以不用给每格起位置名字，一个计数器就够）
//      ★ FILL 也有"一张图"，只不过是一张纯色图：它的源地址填 0，纯色放在参数格里。

//  ★ 第 2 张图（特效图）那条 CI 的字段约定（合成用；含义由像素侧解释，本模块只落格）
//      inputs_0[31: 0] → 特效图源地址      （→ cmd_src   第 2 格）
//      inputs_1[11: 0] → 特效图行跨距（按字，0 = 按目的跨距）
//      inputs_1[19:12] → 强度（8 位，0~255）
//      inputs_1[23:20] → 特效专用模式：{ [23:21] 色调模式, [20] 盖上/垫在底图下面 }
//      inputs_1[30:24] → flags（7 位，含义和第 1 张图【完全一样】）：合成模式 / 源类型 /
//                        抠色 / MX / MY
//      inputs_1[31]    → more = 0（最后一张）
//      ★ 于是 24 位正好用满：跨距 12 + 强度 8 + 特效模式 4；cmd_args 拓宽出来的高 8 位留着。
//      ★ 本模块【不解释】上面任何一位，只按"这是第几张图"原样落进对应那格。

//  什么时候算拼完
//      只看【这条参数 CI 的 more 位】（inputs_1[31]）：
//          more = 1 → 后面还有，继续填下一格
//          more = 0 → 这条就是最后一条 ⇒ 下一拍进 S_PUSH，把整条命令推出去

//  三个状态（和 CmdExec 一样的三段式写法）
//      S_IDLE ：等命令开始。收到 op → 去 S_ARGS；收到 FENCE → 直接去 S_PUSH
//      S_ARGS ：收图，按计数器填格子。收到结束词（more=0）→ 去 S_PUSH
//      S_PUSH ：只停一拍 —— 这一拍把命令线交给队列；判完合法性拉一拍 cmd_wr
//      ★ 三段的划法：第一段只做 state <= next_state；第二段只算 next_state；
//        第三段按当前状态干活（填寄存器），输出在组合区（cmd_wr 就是组合的）
//      ★ 为什么 S_PUSH 要占一拍：最后一张图的字段要到下一拍才落进寄存器，
//        不占这一拍就得给每根线配一个"当拍值还是旧值"的选择器（丑且容易错）。
//        这一拍是必须的，而 CPU 发一条 CI 要几十个周期，队列根本不会空。

//  上限（IMAGE_NUM）
//      线里的格子数是定的，所以一条命令能画几张图是个有限数。到顶还来 more=1
//      ⇒ 整条命令丢掉（不写队列），【绝不截断】：
//        静默截断会让软件看到"参数没生效"却查不出原因；丢掉的话，软件侧表现为
//        "这条命令永远等不到完成"，带超时的 gpu2d_waitDone() 立刻就能发现。
//      ★ 以后要画更多张：把 IMAGE_NUM 调大就行，其余一个字不用改
//        （每根线的宽度就是按它算出来的）。

//  合法性闸门（在 S_PUSH 这一拍判）
//      FILL / BLIT : 宽或高为 0 → 不发（丢）
//                    ★ 零尺寸会让下游的「拍数-1」下溢成 256 拍垃圾写，必须挡在入口
//      图数超限    : IMAGE_NUM 之外还有 more=1 → 不发（丢）
//      FENCE       : 不检查（它的宽高本来就是 0）
//      ★ 地址不检查：不对齐是常态（读侧丢头 / 写侧 WSTRB），越界更不该由硬件判

//  命令流失步怎么处理（不让一条坏命令把整条流带偏）
//      孤儿 ARGS（没在收图）      → 丢弃（S_IDLE 里不认参数 CI）
//      连续两个 op（上一条被截断）→ 丢掉旧的，把新的当新命令收下（重同步）
//      FENCE 前面有没拼完的命令   → 那条作废，FENCE 照常入队
//      零尺寸 / 图数超限          → 不发（等于丢掉），S_PUSH 走完就回 S_IDLE
//      ★ 丢弃 = 不写队列。线里的格子在下一个"命令开始"时会整段清零，
//        所以下一条只带一张图的命令读不到上一条的残值。
//      ★ 一律静默丢弃、继续跑 —— 上板演示不能因为一条坏命令停住
//      ★ 未知功能号必须【收下再丢】，不能靠拒绝：
//        拉低 in_ready 会让 CPU 永远卡住（它保持 cmd_valid 直到 ready）
//      ★ 所有组装动作都带 in_fire：CPU 发完一条 CI 会把 in_valid 拉低，
//        但功能号和参数【还挂在总线上】，少一个 in_fire 就会把残值当成又收了一条

//  接口约定
//      1) in_ready = ~fifo_full & (不在 S_PUSH)：队列满、或正在推命令那一拍都不收
//         （不收不等于丢：CI 是 valid/ready 握手，CPU 会一直举着；
//           推命令那一拍不收，是因为命令线这一拍还要给队列采）
//      2) cmd_wr 是【组合输出】，S_PUSH 这一拍为 1（不合法时这一拍为 0 = 丢掉）
//      3) 出口内容契约（给下游）：类型 ∈ {FILL, BLIT, FENCE}；
//         FILL / BLIT 保证 宽≥1 且 高≥1；FENCE 的其余线恒为 0
//      4) 字段按【第几张图】归位，与 more 位无关；more 只决定"还有没有下一张"

//  参考：docs/spec/硬件_加速器.md §1.1 / §2
//  上一级是谁
//      cmd/CmdBuffer.v —— 它在本模块外面排队（本模块只负责把 CI 拼成一条命令）。
//==============================================================================
`timescale 1ns/100ps

module CIUnpack #(
    parameter IMAGE_NUM = 2         // 一条命令几张图（合成就是 2 张：原图 + 特效图）
)(
    //==================== 时钟 / 复位 ====================
    input  wire                     clk,            //(I) CI 侧时钟（= CPU 时钟）
    input  wire                     rst_n,          //(I) 异步复位，低有效

    //==================== 一条 CI 进来  ← SoC 的 cpu0_customInstruction_*  ====================
    input  wire                     in_valid,       //(I) 有一条 CI
    input  wire [9:0]               in_function_id, //(I) {funct7[6:0], funct3[2:0]}
    input  wire [31:0]              in_inputs_0,    //(I) 第一参数
    input  wire [31:0]              in_inputs_1,    //(I) 第二参数
    output wire                     in_ready,       //(O) 能收下这条（队列满 / 正在推命令时拉低）

    //==================== 写命令队列  → CmdBuffer（跨时钟域 FIFO）====================
    output wire                     cmd_wr,         //(O) 写 FIFO 使能（S_PUSH 这一拍，组合）
    input  wire                     fifo_full,      //(I) 队列满 → 反压

    //==================== 组装好的命令  → CmdBuffer ====================
    output wire [2:0]               cmd_type,   //(O) 3'd1=FILL / 3'd2=BLIT / 3'd3=FENCE
    output wire [32*IMAGE_NUM-1:0]  cmd_src,    //(O) 每张图的源地址：一格 = 一张图
    output wire [31:0]              cmd_dst,    //(O) 目的地址（几张图共用一份）
    output wire [15:0]              cmd_w,      //(O) 宽（目标矩形，共用一份）
    output wire [15:0]              cmd_h,      //(O) 高（目标矩形，共用一份）
    output wire [7*IMAGE_NUM-1:0]   cmd_flags,  //(O) 每张图的模式位：一格 = 一张图
    output wire [32*IMAGE_NUM-1:0]  cmd_args   //(O) 每张图自己的参数：一格 = 一张图
);

//==============================================================================
// localparam
//==============================================================================
// 功能号：funct7 = function_id[9:3]，funct3 = function_id[2:0]
localparam  FUNCT7_CMD      = 7'h00;            // v1 只有命令组 0

localparam  CMD_NOP         = 3'd0;
localparam  CMD_FILL        = 3'd1;
localparam  CMD_BLIT        = 3'd2;
localparam  CMD_FENCE       = 3'd3;
localparam  CMD_ARGS        = 3'd7;             // 参数 CI（= 一张图的资料 / 命令结束词）

//   状态（三个：等命令 / 收图 / 推出去）
localparam  S_IDLE          = 2'd0;
localparam  S_ARGS          = 2'd1;
localparam  S_PUSH          = 2'd2;

//   一条 CI 的参数里各字段的位置
localparam  INPUTS0_W_LSB     = 16;             // 命令开始：inputs_0[31:16] → 宽
localparam  INPUTS0_H_LSB     = 0;              // 命令开始：inputs_0[15: 0] → 高
localparam  INPUTS1_FLAGS_LSB = 24;             // 参数 CI ：inputs_1[30:24] → flags
localparam  INPUTS1_ARGS_LSB  = 0;              // 参数 CI ：inputs_1[23: 0] → 这张图的参数
localparam  INPUTS1_MORE_LSB  = 31;             // 参数 CI ：inputs_1[31]    → more

//==============================================================================
// wire / reg
//==============================================================================
// ---- 状态机 ----
reg  [1:0]                          state_r;
reg  [1:0]                          next_state_r;

// 功能号拆开与识别
wire [6:0]                          in_funct7;
wire [2:0]                          in_funct3;
wire                                id_group0;                          // 属于命令组 0

// 这条 CI 属于哪一类
wire                                ci_op;                              // 命令开始：FILL / BLIT
wire                                ci_single;                          // 单 CI 命令：FENCE
wire                                ci_param;                           // 参数 CI：ARGS（= 一张图）

// 组装动作
wire                                in_fire;                            // 这一拍真的收下一条 CI
wire                                more;                               // 参数 CI：后面还有一张图吗
wire                                args_take;                          // 这一拍真的收下一条参数 CI
wire                                args_overflow;                      // 这一条已经超出上限（没格子了）
wire                                start_cmd;                          // 起一条新命令（op 或 FENCE）
wire                                fill_slot;                          // 往格子里填一张图

// ---- 命令线（模块自己的缓存：一格一格填，S_PUSH 那一拍整排交给队列）----
reg  [2:0]                          cmd_type_r;         // 类型
reg  [31:0]                         cmd_dst_r;          // 目的地址
reg  [15:0]                         cmd_w_r;            // 宽（一张命令共用）
reg  [15:0]                         cmd_h_r;            // 高（一张命令共用）
reg  [32*IMAGE_NUM-1:0]             cmd_src_r;          // 每张图的源地址
reg  [7*IMAGE_NUM-1:0]              cmd_flags_r;        // 每张图的模式位
reg  [32*IMAGE_NUM-1:0]             cmd_args_r;         // 每张图自己的参数

// ---- 收图用的计数器 ----
reg  [1:0]                          args_count_r;                       // 已经收了几张图（饱和在 IMAGE_NUM）
reg                                 args_overflow_r;                    // 本命令收到过多余的图 ⇒ 不发

// ---- 出口的零尺寸检查 ----
wire                                word_draw;                          // 是画图类命令（才需要查尺寸）
wire                                word_bad;                           // 这一条不能发

//==============================================================================
// assign 区：功能号解码（只看输入，与状态无关）
//==============================================================================
assign in_funct7    = in_function_id[9:3];
assign in_funct3    = in_function_id[2:0];

//   NOP(3'd0) / 组号非 0 / 未知功能号：这里【不生成任何信号】——
//   它们不是命令开始、不是单 CI 命令、也不是参数 CI，
//   于是哪个分支都不进 → 天然被丢弃
assign id_group0    = (in_funct7 == FUNCT7_CMD);

assign ci_op        = id_group0 && ((in_funct3 == CMD_FILL) || (in_funct3 == CMD_BLIT));
assign ci_single    = id_group0 &&  (in_funct3 == CMD_FENCE);
assign ci_param     = id_group0 &&  (in_funct3 == CMD_ARGS);

//==============================================================================
// 判断组合条件
//==============================================================================
// 握手：队列满不收；推命令那一拍也不收（命令线还要再站一拍给队列采）
assign in_ready     = ~fifo_full & (state_r != S_PUSH);
assign in_fire      = in_valid & in_ready;

//   ★ more 是【参数 CI】的 inputs_1[31]（命令开始那条 CI 上没有这个位）
assign more         = in_inputs_1[INPUTS1_MORE_LSB];

//   ★★ 下面这两个都带 in_fire —— 这一点是踩过的：
//       CPU 发完一条 CI 会把 in_valid 拉低，但【功能号和参数还挂在总线上】，
//       少一个 in_fire 就会把总线残值当成又收了一条参数 ⇒ 凭空多发一条命令。
assign args_take    = in_fire & ci_param & (state_r == S_ARGS);
assign args_overflow= args_take & (args_count_r >= IMAGE_NUM);   // 格子用完了

//   两个动作（★ 三者天然互斥：一条 CI 只可能属于一类，所以谁先谁后无所谓）
//       start_cmd ：起一条新命令 —— op（还要收图）或 FENCE（自己就是完整命令）。
//                   S_IDLE 里是第一条命令；S_ARGS 里再来一条就是重同步（旧的作废）
//       fill_slot ：往格子里填一张图（只有 S_ARGS 里才成立）
assign start_cmd    = in_fire & (ci_op | ci_single);
assign fill_slot    = args_take & ~args_overflow;

//==============================================================================
// 组合输出：★ 出口就是那几根命令线 + 一个组合的写使能，没有任何选择逻辑
//==============================================================================
assign cmd_type     = cmd_type_r;
assign cmd_dst      = cmd_dst_r;
assign cmd_w        = cmd_w_r;
assign cmd_h        = cmd_h_r;
assign cmd_src      = cmd_src_r;
assign cmd_flags    = cmd_flags_r;
assign cmd_args     = cmd_args_r;

//   零尺寸检查（FENCE 不查）：宽高就在这两根线上，不用切片
assign word_draw    = (cmd_type_r == CMD_FILL) || (cmd_type_r == CMD_BLIT);
assign word_bad     = word_draw ? ((cmd_w_r == 16'd0) || (cmd_h_r == 16'd0) || args_overflow_r)
                    : 1'b0;

//   ★ 写使能是组合的：S_PUSH 只停一拍，这一拍把命令交给队列（不合法就不发 = 丢掉）
assign cmd_wr       = (state_r == S_PUSH) & ~word_bad;

//==============================================================================
// 状态机（三段式：状态寄存器 / 次态组合逻辑 / 数据通路时序逻辑）
//==============================================================================

//------------------------------------------------------------------------------
// 第一段：状态寄存器
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        state_r <= S_IDLE;
    end
    else begin
        state_r <= next_state_r;
    end
end

//------------------------------------------------------------------------------
// 第二段：次态组合逻辑（只算"下一步去哪个状态"，不碰任何数据）
//------------------------------------------------------------------------------
always @(*) begin
    next_state_r = state_r;
    case (state_r)
        // 等命令：命令开始 ⇒ 去收图；FENCE 自己就是一条完整命令 ⇒ 直接推
        S_IDLE : begin
            if      (in_fire & ci_op)     next_state_r = S_ARGS;
            else if (in_fire & ci_single) next_state_r = S_PUSH;
        end
        // 收图：收到结束词（more=0）⇒ 推出去；
        //       FENCE 打进来 ⇒ 前面那条作废，FENCE 自己推出去（重同步）
        S_ARGS : begin
            if      (in_fire & ci_single) next_state_r = S_PUSH;
            else if (args_take & ~more)   next_state_r = S_PUSH;
        end
        // 推出去：只停一拍
        S_PUSH : next_state_r = S_IDLE;
        default: next_state_r = S_IDLE;
    endcase
end

//------------------------------------------------------------------------------
// 第三段：数据通路时序逻辑（按当前状态干活）
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        cmd_type_r      <= CMD_NOP;
        cmd_dst_r       <= 32'd0;
        cmd_w_r         <= 16'd0;
        cmd_h_r         <= 16'd0;
        cmd_src_r       <= {(32*IMAGE_NUM){1'b0}};
        cmd_flags_r     <= {(7*IMAGE_NUM){1'b0}};
        cmd_args_r      <= {(32*IMAGE_NUM){1'b0}};
        args_count_r    <= 2'd0;
        args_overflow_r <= 1'b0;
    end
    else begin
        case (state_r)
            // ---- 这两个状态里都可能干活：S_IDLE 起第一条命令；
            //      S_ARGS 收图，或者被下一条命令顶掉（重同步）----
            S_IDLE,
            S_ARGS : begin
                         if (start_cmd) begin
                             // 起一条新命令：每张图那几根线【整段清零】+ 填共用那几根线
                             //   ★ 不清零的话，下一条只带一张图的命令会读到上一条的残值
                             args_count_r    <= 2'd0;
                             args_overflow_r <= 1'b0;
                             cmd_src_r       <= {(32*IMAGE_NUM){1'b0}};
                             cmd_flags_r     <= {(7*IMAGE_NUM){1'b0}};
                             cmd_args_r      <= {(32*IMAGE_NUM){1'b0}};
                             //   op：类型 = 功能号，宽高 + 目的地址 = 这条 CI 的两个参数
                             //   FENCE：自己就是一条完整命令，其余线一律 0
                             cmd_type_r      <= ci_op ? in_funct3                        : CMD_FENCE;
                             cmd_w_r         <= ci_op ? in_inputs_0[INPUTS0_W_LSB +: 16] : 16'd0;
                             cmd_h_r         <= ci_op ? in_inputs_0[INPUTS0_H_LSB +: 16] : 16'd0;
                             cmd_dst_r       <= ci_op ? in_inputs_1                      : 32'd0;
                         end
                         else if (fill_slot) begin
                             //   ---- 第几张图就填第几格（每根线各填一格，互不干扰）----
                             cmd_src_r  [32*args_count_r +: 32] <= in_inputs_0;
                             cmd_flags_r[ 7*args_count_r +:  7] <= in_inputs_1[INPUTS1_FLAGS_LSB +:  7];
                             cmd_args_r [32*args_count_r +: 24] <= in_inputs_1[INPUTS1_ARGS_LSB  +: 24];
                             // 计数器饱和（不回绕）
                             if (args_count_r != IMAGE_NUM) args_count_r <= args_count_r + 2'd1;
                         end
                         else if (args_overflow) begin
                             args_overflow_r <= 1'b1;    // 格子用完了 ⇒ 这条命令不发
                         end
                     end

            // ---- 推出去：什么都不改（命令线就保持这一拍，给队列采）----
            S_PUSH : ;

            default: ;
        endcase
    end
end

endmodule
