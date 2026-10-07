//==============================================================================
//  WriteController.v —— 把"一行"变成若干笔 AXI 写
//------------------------------------------------------------------------------
//  干什么
//      收一个行任务，把这一行写完，回一个 line_done 单拍：
//          1) 算这一行多少字节：宽 × 3（用 宽 + (宽<<1)，不用乘法器）
//          2) 拆笔：AXI 单笔不能跨 4KB、不超过 256 拍
//          3) 一笔一笔发地址任务：cmd_wr_valid + addr + len，等 ready，等 done
//          4) 最后一笔的 done 回来 → line_done
//      ★ 它不认识 CI、不认识命令队列、不认识 AXI 通道信号（AW/W/B 一个都不碰）
//      ★ 不做跨行的事：一次只处理一行，行与行之间没有任何遗留

//  数据从哪来
//      ★ 本模块【不生成数据】。数据由 Compositor 合成、DDRPacker 补成 AXI 一拍
//        之后送进来（多输入对单输出），本模块只从它那儿取。
//      ★ 首尾遮头（wr_strb）留在这里，因为它由【目的几何】决定（地址低 4 位
//        + 行跨度），和内容无关 —— 换什么源都一样。
//      ★ src_valid 没起来（BLIT 时源还没备好）wr_valid 就拉低，写侧因此
//        天然会等，不需要任何额外的握手状态。

//  WSTRB 只有首尾两拍要遮
//      首拍：低 offset 位为 0（那些字节属于别人，不能擦）
//      中间：全 1（含每一笔突发的末尾 —— 字节流是连续的，不用遮）
//      末拍：只开到本行最后一个字节

//  行任务怎么收（★ 锁任务和开工必须分开）
//      line_start 只有一拍，而数据可能要几十拍后才到（BLIT 要等 DDR 读回来）。
//      所以：line_start 那一拍只【锁存行上下文】并置 line_pend_r，
//      等 src_valid（数据备好）再进 S_CMD 发 AW —— 直接把两个条件写在一个 if 里，
//      数据没到的那一行就会被整行跳过，AR 发了、AW 永远不发。
//      ★ 开工前才发 AW 是有意为之：数据没备好就发 AW，Axi_Mux 的 W 通道会从那一刻
//        锁给我们，而我们一个字节都写不出，别人的写全被挡住（见 docs/spec/设计总纲.md §8.4）

//  wr_free：报给上游做发送节奏
//      含义 = "本行连 B 响应都回来了，我可以接下一行任务了"。
//      上游（Factory 容器）把它和各源的 free 相与成 buf_free，顶层再送给 CmdExec。
//      ★ 少这一根会出两种错，都实测过：
//        · 写侧还在忙时任务来了 → S_IDLE 那一拍看不见它 → 整行丢失、line_done 不回
//        · 更隐蔽的一种：源有整整一行余量（ping-pong 就是为预载下一行），
//          所以"源空"并不代表"这一行数据已经流完"，这时发下一行，
//          DDRPacker 会在 line_start 清兜，把本行还没吐完的数据当成尾巴丢掉

//  一笔一笔来（单 outstanding）
//      AXI4_Master_Write 空闲时才给 cmd_wr_ready，cmd_wr_done 要等 B 响应回来。
//      本模块每笔都等 done 再发下一笔，所以最后一笔 done 一响，整行已经落进 DDR ——
//      line_done 因此天然包含"写完成"，上层不需要另一根"写引擎空不空"的线。

//  拆笔公式（和 bufferControler.burst_of 同一套）
//      本笔拍数 = min(256, (4096 − (addr & 0xFFF)) >> 4, 本行剩余拍数)

//  参考：RTL/Peripherial/LineBuffer/bufferControler.v（读方向的镜像）
//        docs/spec/硬件_加速器.md §3
//==============================================================================
`timescale 1ns/100ps

module WriteController (
    //==================== 时钟 / 复位 ====================
    input  wire             clk_acc,            //(I) 命令侧时钟（与 CmdExec 同域）
    input  wire             rst_acc_n,          //(I) 异步复位，低有效

    //==================== 行任务口（接 CmdExec）====================
    input  wire             line_start,         //(I) 单拍脉冲：这一行的任务在这拍有效
    input  wire [31:0]      line_dst_addr,      //(I) 这一行的目的起始地址
    input  wire [15:0]      line_w,             //(I) 这一行多少像素
    output wire             line_done,          //(O) 单拍脉冲：这一行真的写完了
    output wire             wr_free,            //(O) 能接下一行任务（本行已经全部写完）

    //==================== 地址任务（接 AXIMastersHandler）====================
    output wire             cmd_wr_valid,       //(O) 提交一笔写
    input  wire             cmd_wr_ready,       //(I) 可接收（空闲且不在复位）
    output wire [31:0]      cmd_wr_addr,        //(O) 本笔起始字节地址
    output wire [7:0]       cmd_wr_len,         //(O) 本笔突发拍数 - 1
    input  wire             cmd_wr_done,        //(I) 本笔完成（含 B 响应）
    input  wire             cmd_wr_err,         //(I) 恒 0（Axi_Mux 不透传 BRESP）

    //==================== 数据口 ====================
    //   ★ 数据由 Compositor 给（多输入对单输出），本模块不再自己生成 ——
    //     它只负责"什么时候能供"（wr_valid）和首尾字节使能（wr_strb）。
    //     首尾遮头是【目的几何】决定的，和内容无关，所以留在这里。
    input  wire [127:0]     src_data,           //(I) 来自 Compositor 的一拍 16 字节
    input  wire             src_valid,          //(I) 这一拍有效（BLIT 时要等源备好）
    //   ck 掩码：从 DDRPacker 的掩码兜出来，和 src_data 是【同一个字节网格】。
    //   本行没开 ck 时恒全 1 ⇒ 一与就是原值，老行为一位不变。
    input  wire [15:0]      ck_mask,            //(I) 每字节要不要写（1=写）
    
    output wire [127:0]     wr_data,            //(O) 一拍 16 字节 → AXIMastersHandler
    output wire [15:0]      wr_strb,            //(O) 字节使能
    output wire             wr_valid,           //(O) 还有数据要供
    input  wire             wr_ready            //(I) 写主收下了这一拍
);

//==============================================================================
// 函数
//==============================================================================
// ---- 拆笔：纯加法/移位/比较，没有乘法器 ----
//   ★ addr 的低 4 位必须先清掉再算页内余量。
//     AXI 从机是按 AWADDR 对齐后的地址逐拍走的 —— 拍网格天然 16 字节对齐。
//     不清低 4 位：行首地址未对齐时，页尾只剩 4 字节，(4096-off)>>4 整除得 0，
//     于是本笔拍数 0 → cmd_wr_len 从 0 下溢成 255（256 拍），
//     而数据只够几十拍 → 主机等不到第 256 拍的 WLAST →
//     Axi_Mux 的 W 通道被永久锁死（它靠 WLAST 解锁），
//     整条 AXI 上的写（含 CPU 打串口）全部堵死。
//     清掉低 4 位后，4096-off 必是 16 的整数倍且 ≥16 → 结果恒 ≥1
function [13:0] burst_of;
    input [31:0] addr;              // 本笔起始地址（低 4 位在这里清掉）
    input [13:0] left;              // 本行还剩几拍
    reg   [13:0] to_page;
    begin
        to_page = 14'd4096 - {2'b0, addr[11:4], 4'b0};  // 到下一个 4KB 线还有几字节
        to_page = to_page >> 4;                         // 换算成拍（恒 ≥1）
        if (to_page > 14'd256) to_page = 14'd256;       // 单笔上限 256 拍
        if (left < to_page)    to_page = left;
        burst_of = to_page;
    end
endfunction

//==============================================================================
// localparam
//==============================================================================
localparam  S_IDLE = 2'd0,      // 等行任务
            S_CMD  = 2'd1,      // 发一笔地址任务
            S_RUN  = 2'd2;      // 等这一笔完成（含 B）

//==============================================================================
// wire / reg
//==============================================================================
// ---- 状态机 ----
reg  [1:0]      state;
reg  [1:0]      next_state;

// ---- 行上下文（行开始时采一次）----
reg  [31:0]     cmd_addr_r;                     // 本笔起始地址（命令期间必须稳定）
reg  [13:0]     burst_len_r;                    // 本笔拍数（命令期间必须稳定）
reg  [13:0]     beats_left_r;                   // 本行还剩几拍

// ---- 供数进度 ----
reg  [3:0]      offset_r;                       // 首拍字节偏移
reg  [4:0]      last_bytes_r;                   // 末拍有效字节数（1~16）
reg  [13:0]     beat_idx_r;                     // 已经供到第几拍（0 起）
reg  [13:0]     beats_total_r;                  // 这一行一共几拍
reg             line_pend_r;                    // 行任务已锁存、还在等数据备好

// ---- 行开始时的一次性算术 ----
wire [17:0]     line_bytes;                     // 宽 × 3
wire [17:0]     line_span;                      // 首拍偏移 + 总字节
wire [13:0]     line_beats;                     // 这一行一共几拍
wire [4:0]      line_last;                      // 末拍有效字节数
wire [31:0]     line_base;                      // 拍网格基准 = 行首地址清低 4 位

// ---- 判断组合条件 ----
wire            skip_line;                      // 这一行不写（BLIT / 空行）
wire            skip_done;                      // 跳过时的那一拍 line_done
wire            take_line;                      // 这一拍收下行任务
wire            last_burst;                     // 本笔把整行发完了
wire            beat_take;                      // 本拍数据被写主收走
wire            is_first;                       // 正在供第一拍
wire            is_last;                        // 正在供最后一拍
wire            line_ready;                     // 行任务在手上（可能刚到手，也可能上一拍就锁下了）
wire [31:0]     addr_next;                      // 下一笔的起始地址
wire [13:0]     left_next;                      // 下一笔时还剩几拍

// ---- 组合输出 ----
wire [15:0]     head_mask;                      // 首拍字节使能（遮头）
wire [15:0]     tail_mask;                      // 末拍字节使能（遮尾）

//==============================================================================
// assign 区：行开始时的一次性算术
//==============================================================================
//   拍网格基准：突发一律从 16 字节对齐的地址起（见 burst_of 的说明）
//   行首真正的字节偏移 line_dst_addr[3:0] 仍然用 —— 它决定首拍遮头（不该写的字节位设 0）
assign line_base  = {line_dst_addr[31:4], 4'b0000};

//   宽 × 3 = 宽 + (宽<<1) —— 两个加法器，不用乘法器
assign line_bytes = {2'b0, line_w} + ({2'b0, line_w} << 1);

//   首拍偏移 + 总字节 = 这一行占用的字节跨度
assign line_span  = {14'b0, line_dst_addr[3:0]} + line_bytes;

assign line_beats = (line_span + 18'd15) >> 4;                      // 总拍数 ceil(跨度/16)
assign line_last  = ((line_span - 18'd1) & 18'd15) + 5'd1;          // 末拍 1~16 字节

//==============================================================================
// 判断组合条件
//==============================================================================
//   不写的行任务：总拍数为 0 的空行。
//     ★ 必须挡：拍数 0 会让 cmd_wr_len 从 0 下溢成 255，变成一笔 256 拍的垃圾写
//   （BLIT 现在也要写 —— 数据由 Compositor 给, 不再是"跳过"）
assign skip_line  = (line_beats == 14'd0);
assign skip_done  = (state == S_IDLE) & line_start & skip_line;
assign take_line  = (state == S_IDLE) & line_start & ~skip_line;
//   ★ 任务【锁下来】和【开工】必须分开：line_start 只有一拍，而数据可能要几十拍后才到
//     （BLIT 要等 DDR 读回来）。原来写成 S_IDLE : if (take_line & src_valid) ——
//     数据没到的那些行，任务就在这一拍被跳过了，AW 一直不发、line_done 永远不回。
//     实测：发 3 行，第 1 行有 AR（源收到了）却没有 AW。
assign line_ready = take_line | line_pend_r;

//   ★ 能接下一行任务 = "回到 S_IDLE" 且 "手上没有已锁下、还没开工的行任务"
//     行任务是【单拍脉冲】，S_IDLE 之外的那个拍本模块看不见它 —— 丢了就是永久丢，
//     命令的最后一行的 line_done 再也回不来（实测：发 3 行只写出去 1 行）。
//
//     ★★ 为什么必须带 ~line_pend_r（不是"顺手加的保险"，是实测过的洞）：
//        行任务锁进 line_pend_r 之后、数据还没到的那段时间里，state 【仍然停在 S_IDLE】。
//        这时 wr_free 若还是 1，上游就会再发一个 line_start，而 take_line 在这个窗口里
//        照样成立 → 整组行上下文被覆盖 → 上一行静默消失、它的 line_done 永不回来
//        → CmdExec 按 line_done_cnt_r == h 判完成，整条命令永久卡在 S_RUN。
//        实测复现：A 行锁下后数据未到，pend 窗口内再给 B 行 line_start，
//        结果只发出 1 笔 AW（B 行的）、只回 1 个 line_done —— A 行不见了。
//        （BLIT 要等 DDR 读回、src_valid 几十拍后才来，正好落在这个窗口里）
//
//     回到 S_IDLE 本身的含义是"这一行连 B 响应都回来了"，同时还保证：
//       · 上游各源的 ping-pong 那一块已经被取空（src_free 的含义）
//       · 字节兜里剩下的只可能是本行多读回来的尾巴
//     两条合起来，下一次 line_start 清兜才是安全的（见 DDRPacker 的文件头）。
//     代价只是"等 B 的那十几拍不和下一行的读重叠"，行内的读写重叠一点没动。
assign wr_free    = (state == S_IDLE) & ~line_pend_r;

assign last_burst = (beats_left_r == burst_len_r);      // 本笔正好把剩下的发完
assign beat_take  = wr_valid & wr_ready;
assign is_first   = (beat_idx_r == 14'd0);
assign is_last    = (beat_idx_r == (beats_total_r - 14'd1));

assign addr_next  = cmd_addr_r + (burst_len_r << 4);    // 地址 += 本笔字节数
assign left_next  = beats_left_r - burst_len_r;

//==============================================================================
// 组合输出
//==============================================================================
// ---- 地址任务：只在 S_CMD 那一拍有效 ----
assign cmd_wr_valid = (state == S_CMD);
assign cmd_wr_addr  = cmd_addr_r;
assign cmd_wr_len   = burst_len_r[7:0] - 8'd1;          // 拍数 - 1

// ---- 收工：最后一笔的 done 回来（含 B 响应）----
assign line_done    = skip_done | ((state == S_RUN) & cmd_wr_done & last_burst);

// ---- 数据：内容由 Compositor 给（本模块不再生成）----
assign wr_data = src_data;

// 首拍遮头、末拍遮尾、中间全开
//   ★ 两个遮罩要【分别】算再相与，不能写成多路选择：
//     一行只有一拍时，这一拍既是首拍也是末拍，两头都得遮
assign head_mask = is_first ? (16'hFFFF << offset_r)              : 16'hFFFF;
assign tail_mask = is_last  ? (16'hFFFF >> (16 - last_bytes_r))   : 16'hFFFF;

//   ★ ck（颜色键控）就是在这上面【再与一项】：
//       头尾遮罩是"目的几何"决定的（和内容无关）
//       ck 掩码是"内容"决定的（这个像素命中键色 = 不写）
//     两者相与 = 最终这一拍写哪几个字节。
//   ★ 掩码按字节、跟着数据走同一套移位 ⇒ 像素就算横跨两拍也自动对。
//     ck 没开时 ck_mask 恒全 1，这里结果和改造前逐拍一致（回归）。
assign wr_strb   = head_mask & tail_mask & ck_mask;

//   供完了就拉低; 源还没备好（BLIT 时 Compositor 没数据）也跟着拉低 —— 
//   写侧因此天然会等, 不需要任何额外的握手状态
assign wr_valid = (beat_idx_r < beats_total_r) & src_valid;

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
        //   ★ 任务字段在 take_line 那一拍就锁存(它们只在 line_start 有效),
        //     但【开写】要等 src_valid —— 数据没备好就发 AW 的话, Axi_Mux 的
        //     W 通道会从 AW 那一刻就锁给我们, 而我们一个字节都写不出,
        //     期间别人的写(CPU 串口/显示侧)全被挡住(见 §9.1)。
        //     FILL 的常数色源恒有效 → 立刻开写; BLIT 等 bank 装满。
        S_IDLE : if (line_ready & src_valid) next_state = S_CMD;
        S_CMD  : if (cmd_wr_ready) next_state = S_RUN;      // 命令被收下
        S_RUN  : if (cmd_wr_done)                           // 这一笔完成了
                     next_state = last_burst ? S_IDLE : S_CMD;
        default:                   next_state = S_IDLE;
    endcase
end

//------------------------------------------------------------------------------
// 数据通路时序逻辑
//     赋值顺序(同拍后者覆盖前者):
//       供数进度在 case 之前(它跟状态无关, 只看数据握手);
//       case 里 S_IDLE 那支带 take_line, 而 take_line 时 wr_valid 必为 0
//       (上一行的拍已经供完), 两者不会同拍打架
//------------------------------------------------------------------------------
always @(posedge clk_acc or negedge rst_acc_n) begin
    if (!rst_acc_n) begin
        cmd_addr_r    <= 32'd0;
        burst_len_r   <= 14'd0;
        beats_left_r  <= 14'd0;
        offset_r      <= 4'd0;
        last_bytes_r  <= 5'd16;
        beat_idx_r    <= 14'd0;
        beats_total_r <= 14'd0;
        line_pend_r   <= 1'b0;
    end
    else begin
        // ---- 供出一拍: 拍号 +1 ----
        if (beat_take) begin
            beat_idx_r <= beat_idx_r + 14'd1;
        end

        case (state)
            // ---- 收下行任务: 锁行上下文 + 装载首笔 + 装载供数参数 ----
            S_IDLE : begin
                         if (take_line) begin
                             cmd_addr_r    <= line_base;
                             beats_left_r  <= line_beats;
                             burst_len_r   <= burst_of(line_base, line_beats);
                             offset_r      <= line_dst_addr[3:0];
                             last_bytes_r  <= line_last;
                             beat_idx_r    <= 14'd0;
                             beats_total_r <= line_beats;
                             line_pend_r   <= 1'b1;
                         end
                         //   数据备好了 → 交给 S_CMD（同拍 take_line 时也成立，写在后面覆盖）
                         if (line_ready & src_valid) line_pend_r <= 1'b0;
                     end

            S_CMD  : ;

            // ---- 本笔完成: 还有剩就递推下一笔, 否则收工 ----
            S_RUN  : if (cmd_wr_done && !last_burst) begin
                         cmd_addr_r   <= addr_next;
                         beats_left_r <= left_next;
                         burst_len_r  <= burst_of(addr_next, left_next);
                     end

            default: ;
        endcase
    end
end

endmodule
