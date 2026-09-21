//==============================================================================
//  bufferControler.v —— 行预取控制器（写时钟域 clk）
//------------------------------------------------------------------------------
//  干什么
//      按 lineBuffer_addr 给的地址，把一行的 LINE_BEATS 拍从 DDR 搬进
//      async_PingPongBuffer 的写口；一行交一次 buffer_wr_block_done；
//      交完就等读侧让出 bank，再搬下一行。
//      ★ 它不知道什么叫双缓冲/DONE/翻页 —— 那些是 bufferAPBHandler 的事;
//        它只认 enable / mode / {段0,段1} / frame_start
//
//  上一级是 bufferAPBHandler
//      它把 APB 寄存器翻译成三根有效信号: enable / mode / addr{段0,段1}
//      本模块按 mode 决定"一行取几段":
//          mode=0 全屏: 1 段, 段0 = 整帧,          360 拍, 行距 5760
//          mode=1 分屏: 2 段, 段0 = 左半(加速器), 段1 = 右半(CPU), 各 180 拍, 行距 2880
//      行距 = 段拍数 x 16, 不用另外给
//
//  一行为什么必须拆笔
//      LINE_BEATS = 360 拍 > AXI 单笔上限 256 拍，且一笔不得跨 4KB 边界。
//      行首在 4KB 页内的偏移逐行漂移（5760 mod 4096 = 1664，32 行一个循环）：
//          页内偏移    0 → 256 + 104         共 2 笔
//          页内偏移 1664 → 152 + 208         共 2 笔
//          页内偏移 3328 →  48 + 256 + 56    共 3 笔   ★ 不是固定 2 笔
//      拆笔写成 burst_of() 函数: n = min(256, 到 4KB 线的拍数, 本段剩余拍数)
//      ★ 本笔拍数在命令期间必须【稳定】(AXI 要求), 所以用 burst_len_r 锁存
//
//  行号与帧对齐
//      开机没收到 frame_start 之前不许取数(frame_valid_r)
//      ★ 只有【使能着的那个帧边界】才给取数许可: 使能是软件在帧中间随时写下来的,
//        那时立刻从第 0 行取, 会让显示侧读者在"一行的中间"开始消费第一块,
//        之后每行固定错开同一段(上板实测: 整屏每行水平移位)
//      取完第 FRAME_LINES-1 行 → 帧尾停手, 等下一个 frame_start
//      frame_start 到 → 行号清 0, 段地址回基址, 这时才取第 0 行
//        ★ 不能在帧尾"顺手"把新帧第 0 行提前取回来: 那一刻基址还没翻
//        ★ 停手不花代价: 帧边界到第一个有效行有 41 行(607us), 取一行只要 ~5us
//        ★ frame_start 只在空闲时采纳: 正在取行时来的丢掉, 下一帧自纠
//
//  接口约定
//      0) lineBuffer_enable=0 时不出命令(停在 IDLE); mode/addr 由 bufferAPBHandler 给
//      1) lineBuffer_frame_start：★必须是【写域单拍事件】(见 lineBuffer.v 里的跨域桥)
//      2) 只读不写：cmd_write 恒 0（行预取只从 DDR 读）
//      3) 单 outstanding：上一笔收完(rd_last)之后才发下一笔
//      4) rd_ready = buffer_wr_block_ready：FIFO 满了就反压 master
//      5) cmd_err：本行剩余拍数【直接往 buffer 写 0】补满到 LINE_BEATS, 再跳下一行
//         ★ 块必须精确 LINE_BEATS 拍: 短一拍, 读侧像素就永久错位
//
//  全模块没有乘法器/除法器: 行距和字节数都是左移 4 位, 到 4KB 线是右移 4 位
//==============================================================================
module bufferControler #(
    parameter AXI_ADDR_WIDTH    = 32,
    parameter AXI_DATA_WIDTH    = 128,
    parameter AXI_LEN_WIDTH     = 8,
    parameter LINE_BYTES        = 5760,
    parameter FRAME_LINES       = 1080,
    parameter LINE_WIDTH        = 11,
    parameter LINE_BEATS        = LINE_BYTES/(AXI_DATA_WIDTH/8)
)(
    input  wire                         clk,
    input  wire                         rst_n,

    //------------------------------------------------------------
    // 显示参数（来自 bufferAPBHandler）
    //------------------------------------------------------------
    input  wire                         lineBuffer_enable,
    input  wire                         lineBuffer_mode,            // 0 = 全屏(1 段) 1 = 分屏(2 段)
    input  wire [2*AXI_ADDR_WIDTH-1:0]  lineBuffer_addr,            // {段0基址, 段1基址}
    input  wire                         lineBuffer_frame_start,     // [写域单拍]

    //------------------------------------------------------------
    // 写口（接 async_PingPongBuffer）
    //------------------------------------------------------------
    output wire                         buffer_wr_en,
    output wire [AXI_DATA_WIDTH-1:0]    buffer_wr_data,
    output wire                         buffer_wr_block_done,       // 每行一次，与最后一拍同拍
    input  wire                         buffer_wr_block_ready,

    //------------------------------------------------------------
    // 命令口（接 AXI4_Master）
    //------------------------------------------------------------
    output wire                         cmd_valid,
    output wire                         cmd_write,                  // 恒 0
    output wire [AXI_ADDR_WIDTH-1:0]    cmd_addr,
    output wire [AXI_LEN_WIDTH-1:0]     cmd_len,                    // 拍数 - 1
    input  wire                         cmd_ready,
    input  wire                         cmd_done,
    input  wire                         cmd_err,                    // RRESP 非 OKAY

    //------------------------------------------------------------
    // 读数据口（接 AXI4_Master）
    //------------------------------------------------------------
    input  wire [AXI_DATA_WIDTH-1:0]    rd_data,
    input  wire                         rd_valid,
    input  wire                         rd_last,
    output wire                         rd_ready
);

//==============================================================================
// localparam
//==============================================================================
localparam  BEAT_BYTES      = AXI_DATA_WIDTH/8;                     // 16
localparam  SEG_BEATS_ALL   = LINE_BEATS;                           // 全屏: 360 拍
localparam  SEG_BEATS_HALF  = LINE_BEATS/2;                         // 分屏: 180 拍
localparam  SEG_STRIDE_ALL  = LINE_BYTES;                           // 全屏行距 5760
localparam  SEG_STRIDE_HALF = LINE_BYTES/2;                         // 分屏行距 2880
localparam  BEATS_W         = 10;                                   // 拍数位宽(≤512)
localparam  ONE_BEAT        = {{(BEATS_W-1){1'b0}}, 1'b1};          // 拍数比较用的"1"

localparam  S_IDLE  = 3'd0,
            S_CMD   = 3'd1,
            S_DATA  = 3'd2,
            S_FILL  = 3'd3,                                         // cmd_err: 补零到本行满
            S_LAST  = 3'd4;                                         // 收尾: 行号/行距/帧尾

//==============================================================================
// 函数：拆笔计算（纯加法/比较/移位, 没有乘法器）
//==============================================================================
function [BEATS_W-1:0] burst_of;
    input [AXI_ADDR_WIDTH-1:0]  addr;       // 本笔起始地址
    input [BEATS_W-1:0]         left;       // 本段还剩几拍
    reg   [12:0]                to_page;
    begin
        to_page = 13'd4096 - {1'b0, addr[11:0]};        // 到下一个 4KB 线还有几字节
        to_page = to_page >> 4;                         // 换算成拍
        if (to_page > 13'd256) to_page = 13'd256;       // 单笔上限 256 拍
        if ({3'b0, left} < to_page) to_page = {3'b0, left};
        burst_of = to_page[BEATS_W-1:0];
    end
endfunction

//==============================================================================
// wire / reg
//==============================================================================
// ---- 状态机 ----
reg  [2:0]                  state;
reg  [2:0]                  next_state;

// ---- 数据通路寄存器 ----
reg  [LINE_WIDTH-1:0]       line_display_r;   // 即将被显示的那一行
reg  [BEATS_W-1:0]          beat_cnt_r;       // 本行已写进 buffer 的拍数(0..359)
reg  [1:0]                  seg_r;            // 当前第几段
reg  [BEATS_W-1:0]          beats_left_r;     // 本段还剩几拍(含正在收的这一拍)
reg  [BEATS_W-1:0]          burst_len_r;      // 本笔拍数(命令期间必须稳定)
reg  [AXI_ADDR_WIDTH-1:0]   cmd_addr_r;       // 本笔起始地址(命令期间必须稳定)
reg  [AXI_ADDR_WIDTH-1:0]   seg0_addr_r;      // 段0 的行地址累加器
reg  [AXI_ADDR_WIDTH-1:0]   seg1_addr_r;      // 段1 的行地址累加器
reg  [BEATS_W-1:0]          fill_left_r;      // 补零还剩几拍
reg                         wait_frame_r;     // 帧尾停手标志
reg                         frame_valid_r;    // 取数许可: 见过"使能着的帧边界"才置起
reg                         frame_start_d_r;  // [写域] 打过一拍的帧边界

// ---- 段配置 / 递推量 ----
wire [1:0]                  seg_num;          // 本模式下 一行几段
wire [BEATS_W-1:0]          seg_beats;        // 每段几拍
wire [AXI_ADDR_WIDTH-1:0]   seg_stride;       // 行距(字节)
wire [1:0]                  last_seg;         // 最后一段的编号
wire [BEATS_W-1:0]          beats_next;       // 收完这一拍后本段还剩几拍
wire [AXI_ADDR_WIDTH-1:0]   cmd_addr_next;    // 本笔收完后 下一笔的起始地址
wire [BEATS_W-1:0]          burst_len_next;   // 下一笔的拍数

// ---- 判断组合条件 ----
wire                        frame_boundary;   // 本拍是"打过一拍的帧边界"
wire                        idle_start;       // 空闲态: 可以开始取下一行
wire                        beat_accept;      // 读数据这一拍被写进 buffer
wire                        fill_beat;        // 补零态: 这一拍被写进 buffer
wire                        burst_done;       // 本笔最后一拍正在收
wire                        seg_done;         // 这一笔正好把本段取完
wire                        line_done;        // 这一笔正好把整行取完
wire                        fill_last;        // 补零补到了本行最后一拍
wire                        last_beat;        // 真正写进 buffer 的最后一拍(含补零)

//==============================================================================
// assign 区：段配置 / 递推量（静态连线, 不依赖状态）
//==============================================================================
assign  seg_num         = lineBuffer_mode ? 2'd2 : 2'd1;
assign  seg_beats       = lineBuffer_mode ? SEG_BEATS_HALF[BEATS_W-1:0] : SEG_BEATS_ALL[BEATS_W-1:0];
assign  seg_stride      = lineBuffer_mode ? SEG_STRIDE_HALF            : SEG_STRIDE_ALL;
assign  last_seg        = seg_num - 2'd1;

assign  beats_next      = beats_left_r - 1'b1;
assign  cmd_addr_next   = cmd_addr_r + (burst_len_r << 4);            // 地址 += 本笔字节数
assign  burst_len_next  = burst_of(cmd_addr_next, beats_next);        // 下一笔的拍数

//==============================================================================
// 判断组合条件（状态机与组合输出都用这些名字, 不再写长表达式）
//==============================================================================
// ---- 帧边界 / 空闲 ----
assign  frame_boundary  = (state == S_IDLE) && frame_start_d_r;
assign  idle_start      = lineBuffer_enable && frame_valid_r && !wait_frame_r &&
                          buffer_wr_block_ready && !frame_start_d_r;

// ---- 数据拍 / 补零拍 ----
assign  beat_accept     = (state == S_DATA) && rd_valid && buffer_wr_block_ready;
assign  fill_beat       = (state == S_FILL) && buffer_wr_block_ready;

// ---- 笔尾 / 段尾 / 行尾 ----
assign  burst_done      = beat_accept && rd_last;
assign  seg_done        = burst_done && (beats_left_r == ONE_BEAT);  // 本笔正好收完本段
assign  line_done       = seg_done && (seg_r == last_seg);           // 本笔正好收完整行
assign  fill_last       = fill_beat && (fill_left_r == ONE_BEAT);    // 补零补到最后一拍
assign  last_beat       = (burst_done && ~cmd_err && line_done) || fill_last;

//==============================================================================
// 组合输出
//==============================================================================
assign  cmd_valid           = (state == S_CMD);
assign  cmd_write           = 1'b0;                                 // 只读
assign  cmd_addr            = cmd_addr_r;
assign  cmd_len             = burst_len_r[AXI_LEN_WIDTH-1:0] - 1'b1;

assign  rd_ready            = buffer_wr_block_ready;                // FIFO 满了反压 master

assign  buffer_wr_en        = beat_accept || fill_beat;
assign  buffer_wr_data      = (state == S_FILL) ? {AXI_DATA_WIDTH{1'b0}} : rd_data;
assign  buffer_wr_block_done= last_beat;                            // 与最后一拍同拍

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
        S_IDLE : if (idle_start)   next_state = S_CMD;
        S_CMD  : if (cmd_ready)    next_state = S_DATA;
        S_DATA : if (burst_done) begin
                     if (cmd_err)          next_state = S_FILL;     // 出错: 本行补零
                     else if (!line_done)  next_state = S_CMD;      // 还有下一笔/下一段
                     else                  next_state = S_LAST;     // 整行取完
                 end
        S_FILL : if (fill_last)    next_state = S_LAST;             // 补到本行最后一拍
        S_LAST :                   next_state = S_IDLE;
        default:                   next_state = S_IDLE;
    endcase
end

//------------------------------------------------------------------------------
// 数据通路时序逻辑
//     赋值顺序(同拍后者覆盖前者):
//       帧边界处理 在 case 之前; case 里 S_IDLE 那支带 !frame_start_d_r,
//       所以"帧边界回基址"和"空闲开始取下一行"不会同拍打架
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        line_display_r    <= {LINE_WIDTH{1'b0}};
        beat_cnt_r        <= {BEATS_W{1'b0}};
        seg_r             <= 2'd0;
        beats_left_r      <= {BEATS_W{1'b0}};
        burst_len_r       <= {BEATS_W{1'b0}};
        cmd_addr_r        <= {AXI_ADDR_WIDTH{1'b0}};
        seg0_addr_r       <= {AXI_ADDR_WIDTH{1'b0}};
        seg1_addr_r       <= {AXI_ADDR_WIDTH{1'b0}};
        fill_left_r       <= {BEATS_W{1'b0}};
        wait_frame_r      <= 1'b0;
        frame_valid_r     <= 1'b0;
        frame_start_d_r   <= 1'b0;
    end
    else begin
        // ---- 帧边界打一拍: handler 先翻索引, 本模块下一拍再按新基址取数 ----
        frame_start_d_r <= lineBuffer_frame_start;

        // ---- 使能一撤就取消取数许可: 下次使能拉高后必须重新等一个帧边界 ----
        if (!lineBuffer_enable)
            frame_valid_r <= 1'b0;

        // ---- 帧边界: 行号清 0 + 段地址回基址 + 给取数许可(只在空闲时采纳) ----
        if (frame_boundary) begin
            line_display_r  <= {LINE_WIDTH{1'b0}};
            seg0_addr_r     <= lineBuffer_addr[2*AXI_ADDR_WIDTH-1:AXI_ADDR_WIDTH];
            seg1_addr_r     <= lineBuffer_addr[AXI_ADDR_WIDTH-1:0];
            cmd_addr_r      <= lineBuffer_addr[2*AXI_ADDR_WIDTH-1:AXI_ADDR_WIDTH];
            beats_left_r    <= seg_beats;
            burst_len_r     <= burst_of(lineBuffer_addr[2*AXI_ADDR_WIDTH-1:AXI_ADDR_WIDTH], seg_beats);
            seg_r           <= 2'd0;
            beat_cnt_r      <= {BEATS_W{1'b0}};
            wait_frame_r    <= 1'b0;
            frame_valid_r   <= lineBuffer_enable;       // 使能着的帧边界才给许可
        end

        case (state)
            // ---- 空闲: 读侧让出 bank 就开始取下一行 ----
            S_IDLE : if (idle_start) begin
                         seg_r         <= 2'd0;
                         beats_left_r  <= seg_beats;
                         beat_cnt_r    <= {BEATS_W{1'b0}};
                         cmd_addr_r    <= seg0_addr_r;
                         burst_len_r   <= burst_of(seg0_addr_r, seg_beats);
                     end

            S_CMD  : ;

            // ---- 收数据 / 写 buffer ----
            S_DATA : if (beat_accept) begin
                         beat_cnt_r   <= beat_cnt_r + 1'b1;
                         beats_left_r <= beats_next;

                         if (burst_done) begin
                             if (cmd_err) begin
                                 // 本行还差几拍(这一拍已经写进去了)
                                 fill_left_r <= LINE_BEATS[BEATS_W-1:0] - (beat_cnt_r + 1'b1);
                             end
                             else if (!line_done) begin
                                 if (seg_done) begin
                                     // 本段取完: 换下一段
                                     seg_r        <= seg_r + 2'd1;
                                     beats_left_r <= seg_beats;
                                     cmd_addr_r   <= seg1_addr_r;
                                     burst_len_r  <= burst_of(seg1_addr_r, seg_beats);
                                 end
                                 else begin
                                     // 本段还有剩: 地址 += 本笔字节数, 装下一笔
                                     cmd_addr_r   <= cmd_addr_next;
                                     burst_len_r  <= burst_len_next;
                                 end
                             end
                         end
                     end

            // ---- cmd_err: 直接写 0 补满本行 ----
            S_FILL : if (fill_beat) begin
                         fill_left_r <= fill_left_r - 1'b1;
                         beat_cnt_r  <= beat_cnt_r + 1'b1;
                     end

            // ---- 收尾: 行号++ / 行距累加 / 帧尾停手 ----
            S_LAST : begin
                         seg0_addr_r <= seg0_addr_r + seg_stride;
                         seg1_addr_r <= seg1_addr_r + seg_stride;
                         if (line_display_r == FRAME_LINES[LINE_WIDTH-1:0] - 1'b1)
                             wait_frame_r <= 1'b1;
                         else
                             line_display_r <= line_display_r + 1'b1;
                     end
            default: ;
        endcase
    end
end

endmodule
