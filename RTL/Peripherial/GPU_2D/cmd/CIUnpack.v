//==============================================================================
//  CIUnpack.v —— 把两条 CI 拼成一条命令
//------------------------------------------------------------------------------
//  干什么
//      收下一条条 CI，按功能号分成三类，攒成一条完整命令写进队列：
//          命令开始（FILL / BLIT）→ 锁存 类型 / 源 / 目的，进入「组装中」
//          参数 CI（ARGS）        → 补上 宽 / 高 / flags / 颜色 → 命令完成
//          单 CI 命令（FENCE）    → 自己就是一条完整命令（帧结束）
//      其余（NOP / 未知功能号 / 组号非 0）→ 什么都不做
//      ★ 它不认识 DDR、不认识帧、不碰 AXI —— 出门就是七个扁平字段
//      ★ 纯单时钟域（CI 侧 = CPU 时钟域），没有任何跨域逻辑

//  为什么是「组装状态 + 完成事件」，而不是「第 2 条 CI 到了就算完」
//      v1 一条命令 = 两条 CI，但格式里已经留了 more 位，将来会有 3 条以上的命令。
//      所以这里不把「第 2 条」写死，而是拆成两件事：
//          组装状态（assembling + 三个锁存）：这条命令收到哪儿了
//          完成事件（cmd_complete）          ：这条命令的字段到齐了
//      ★ 合法性检查挂在【完成事件】上，不挂在某一条具体的 CI 上 ——
//        以后宽高从第 3、第 4 条 CI 来，检查的位置不用动
//      ★ 接变长时只改两处：参数 CI 进来时「再并一次」，完成条件加 & ~more

//  三类 CI 怎么认（v1：funct7 恒 0，用 funct3 区分）
//      命令开始  : funct3 = 1(FILL) / 2(BLIT)
//      单 CI 命令: funct3 = 3(FENCE)
//      参数 CI   : funct3 = 7(ARGS)，也是「命令结束词」
//      预留 4~6  : BG_ROW / BG_FULL / BULK（还没实现，一律丢弃）

//  合法性闸门（唯一出口，按类型分派）
//      FILL / BLIT : 宽或高为 0 → 丢
//                    ★ 零尺寸会让下游的「拍数-1」下溢成 256 拍垃圾写，必须挡在入口
//      FENCE       : 不检查（它的宽高本来就是 0，明写出来，不靠「别忘了排除」）
//      未定义类型  : 不放行
//      ★ 地址不检查：不对齐是常态（读侧丢头 / 写侧 WSTRB），越界更不该由硬件判 ——
//        硬件里没有任何帧缓冲地址的知识

//  命令流失步怎么处理（不让一条坏命令把整条流带偏）
//      孤儿 ARGS（没在组装中）    → 丢弃
//      连续两个 op（上一条被截断）→ 丢掉旧的，把新的当新命令收下（重同步）
//      FENCE 前面有没拼完的命令   → 那条作废，FENCE 照常入队
//      零尺寸命令                → 在完成事件上丢掉，不写队列
//      ★ 丢弃 = 不写队列 + 【清组装状态】。后者不能省：不清的话，
//        紧接着来一条孤儿 ARGS 会拿锁存里的旧字段拼出一条幽灵命令进队列
//      ★ 一律静默丢弃、继续跑 —— 上板演示不能因为一条坏命令停住
//      ★ 未知功能号必须【收下再丢】，不能靠拒绝：
//        拉低 in_ready 会让 CPU 永远卡住（它保持 cmd_valid 直到 ready）

//  接口约定
//      1) in_ready = ~fifo_full：队列满就【整条 CI 都不收】
//      2) cmd_wr 单拍；命令字段只在这一拍有效（组合输出，FIFO 当拍采样）
//      3) 出口内容契约（给下游）：类型 ∈ {FILL, BLIT, FENCE}；
//         FILL / BLIT 保证 宽≥1 且 高≥1；FENCE 的其余字段恒为 0
//      4) more（inputs_1[31]）v1 忽略：只支持「一条参数 CI」的命令

//  参考：docs/spec/硬件_加速器.md §1.1 / §2
//==============================================================================
`timescale 1ns/100ps

module CIUnpack (
    //==================== 时钟 / 复位 ====================
    input  wire             clk,                //(I) CI 侧时钟（= CPU 时钟）
    input  wire             rst_n,              //(I) 异步复位，低有效

    //==================== 一条 CI 进来 ====================
    input  wire             in_valid,           //(I) 有一条 CI
    input  wire [9:0]       in_function_id,     //(I) {funct7[6:0], funct3[2:0]}
    input  wire [31:0]      in_inputs_0,        //(I) 第一参数
    input  wire [31:0]      in_inputs_1,        //(I) 第二参数
    output wire             in_ready,           //(O) 能收下这条（队列满就拉低）

    //==================== 写命令队列 ====================
    output wire             cmd_wr,             //(O) 组装完成：写 FIFO 使能（单拍）
    input  wire             fifo_full,          //(I) 队列满 → 反压

    //==================== 组装好的命令（130 位）====================
    output wire [2:0]       cmd_type,           //(O) 3'd1=FILL / 3'd2=BLIT / 3'd3=FENCE
    output wire [31:0]      cmd_src,            //(O) 源地址
    output wire [31:0]      cmd_dst,            //(O) 目的地址
    output wire [15:0]      cmd_w,              //(O) 宽度（像素）
    output wire [15:0]      cmd_h,              //(O) 高度（行）
    output wire [6:0]       cmd_flags,          //(O) 源格式 / 透明使能
    output wire [23:0]      cmd_color           //(O) 填充色（RGB888）
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
localparam  CMD_ARGS        = 3'd7;             // 参数 CI / 命令结束词

//==============================================================================
// wire / reg
//==============================================================================
// 功能号拆开与识别
wire [6:0]  in_funct7;
wire [2:0]  in_funct3;
wire        id_group0;                          // 属于命令组 0

// 这条 CI 属于哪一类
wire        ci_op;                              // 命令开始：FILL / BLIT
wire        ci_single;                          // 单 CI 命令：FENCE
wire        ci_param;                           // 参数 CI：ARGS

// 组装动作
wire        param_arrive;                       // 参数 CI 落进一条正在组装的命令
wire        cmd_complete;                       // 完成事件：字段到齐（唯一出口）
wire        in_fire;

// 组装状态
reg         assembling;                         // 0 = 等命令开始；1 = 组装中，等参数
reg  [2:0]  cur_type_r;                         // 命令开始锁存：操作类型
reg  [31:0] cur_src_r;                          // 命令开始锁存：源地址
reg  [31:0] cur_dst_r;                          // 命令开始锁存：目的地址

// 合法性闸门
wire [2:0]  out_type;                           // 这一拍要入队的类型
wire        draw_cmd;                           // out_type 是会画东西的命令
wire        zero_size;                          // 宽或高为 0 —— 空命令
wire        cmd_bad;                            // 该不该丢（只在完成事件那一拍有效）

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
// 判断组合条件（状态机与组合输出都用这些名字，不再写长表达式）
//==============================================================================
// 握手：队列满就整条都不收
assign in_ready     = ~fifo_full;
assign in_fire      = in_valid & in_ready;

// 组装动作
//   param_arrive : 参数 CI 落进一条正在组装的命令
//                  ★ v1 的 more 位忽略 —— 这里直接当成「结束词」；
//                    将来做变长命令，这里加 & ~more 就行
//   cmd_complete : 完成事件。FENCE 自己就算一条；参数 CI 则要求「正在组装中」，
//                  孤儿 ARGS 不满足后半句 → 天然被丢弃
assign param_arrive = ci_param & assembling;
assign cmd_complete = in_fire & (ci_single | param_arrive);

// 合法性闸门（挂在完成事件上，按类型分派）
//   ★ zero_size 取自当拍 ARGS 的 inputs_0 —— v1 的宽高就在这条 CI 里；
//     将来宽高从别的 CI 来，改成从组装寄存器取，位置（完成事件）不用变
assign out_type     = ci_single ? CMD_FENCE : cur_type_r;
assign draw_cmd     = (out_type == CMD_FILL) || (out_type == CMD_BLIT);
assign zero_size    = (in_inputs_0[31:16] == 16'd0) || (in_inputs_0[15:0] == 16'd0);

assign cmd_bad      = draw_cmd  ? zero_size      // 画图命令：尺寸不能为 0
                    : ci_single ? 1'b0           // 帧结束：没有尺寸，不检查
                    :             1'b1;          // 未定义类型：不放行

//==============================================================================
// 组合输出
//   ★ 命令字段只在 cmd_wr = 1 那一拍有效；FENCE 的其余字段填 0
//     （免得队列里留上一条的残值）
//==============================================================================
assign cmd_wr    = cmd_complete & ~cmd_bad;

assign cmd_type  = out_type;
assign cmd_src   = ci_single ? 32'd0 : cur_src_r;
assign cmd_dst   = ci_single ? 32'd0 : cur_dst_r;
assign cmd_w     = ci_single ? 16'd0 : in_inputs_0[31:16];
assign cmd_h     = ci_single ? 16'd0 : in_inputs_0[15:0];
assign cmd_flags = ci_single ? 7'd0  : in_inputs_1[30:24];
assign cmd_color = ci_single ? 24'd0 : in_inputs_1[23:0];

//==============================================================================
// 时序逻辑：assembling 就是状态（等命令开始 / 组装中）
//==============================================================================
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        assembling <= 1'b0;
        cur_type_r <= CMD_NOP;
        cur_src_r  <= 32'd0;
        cur_dst_r  <= 32'd0;
    end
    else if (in_fire) begin
        if (ci_op) begin
            // 命令开始：锁存；上一条还没等到参数就被这条顶掉（重同步）
            assembling <= 1'b1;
            cur_type_r <= in_funct3;
            cur_src_r  <= in_inputs_0;
            cur_dst_r  <= in_inputs_1;
        end
        else if (cmd_complete) begin
            // 完成事件：不管最后有没有写进队列（可能是被丢掉的），组装状态一律复位
            //   ★ 这一步不能省 —— 不清的话，下一条孤儿 ARGS 会拼出幽灵命令
            assembling <= 1'b0;
        end
        // 其余情况（NOP / 孤儿 ARGS / 未知功能号）：什么都不做 —— 这就是丢弃
    end
end

endmodule
