//==============================================================================
//  bufferAPBHandler.v —— 把 APB 寄存器"翻译"成控制器要的有效信号
//------------------------------------------------------------------------------
//  角色
//      APB0_lineBuffer 那边只有"静态寄存器 + DONE 位";
//      本模块把它们变成 controller 能直接用的东西:
//          当前该显示哪两段地址 / 使能 / 模式 / DONE 清零脉冲 / 帧率数字
//      —— 就是一个"寄存器 -> 有效信号"的翻译层, 放在显示通道里(和 bufferControler 一起)
//
//  谁管什么(别越界)
//      DONE 的寄存器在 APB: 软件 W1S 写 / 加速器脉冲置位 / 软件回读
//      本模块只做两件事: 在帧边界"消费"它(拉 done_clr 回去清位), 并据此翻索引
//      FPS 的格子也在 APB, 但计数在本模块做, 算好经 fps_*_val 送回去
//
//  翻页规则
//      lineBuffer_frame_start 那一拍看 done:
//          done_acc=1 → 翻 acc_idx(分屏切左半, 全屏切整帧) + done_clr[0]
//          done_cpu=1 → 翻 cpu_idx(右半)                    + done_clr[1]
//      两位互相独立: 加速器快就先翻, CPU 没画完就一直显示它原来那一块
//      ★ 索引只在帧边界翻; 模式(SPLIT)也在帧边界锁存
//        → 一帧之内的取数几何(分屏两段 / 全屏一段)永远一致
//
//  帧率
//      "帧率" = 每秒发生了几次【交帧事件】, 事件就是 DONE 的 0->1
//        (本模块自己边沿检测, 不需要额外端口)
//      时间基准借显示的场起始: 数满 FPS_WINDOW 个 frame_start 算一秒(1080p60 → 60)
//      模式变了就重开窗口(不让"半幅帧 + 整帧"混在一个窗口里);
//      锁存值保留 —— 显示端的帧率数字不会闪 0
//
//  给 controller 的约定
//      lineBuffer_addr = {段0基址, 段1基址}
//          分屏(SPLIT=1): 段0 = 左半(加速器), 段1 = 右半(CPU)
//          全屏(SPLIT=0): 段0 = 整帧, 段1 恒 0
//==============================================================================
module bufferAPBHandler #(
    parameter ADDR_WIDTH = 32,
    parameter FPS_WINDOW = 60                // 帧率窗口:几个 frame_start 算一秒(1080p60 = 60)
)(
    input  wire                         clk,
    input  wire                         rst_n,

    //------------------------------------------------------------
    // 来自 APB 寄存器(静态值 + DONE 状态)
    //------------------------------------------------------------
    input  wire                         enable,
    input  wire                         split_pending,
    input  wire                         done_acc,
    input  wire                         done_cpu,
    input  wire [2*ADDR_WIDTH-1:0]      framea_base_addr,     //(I) {左半(acc), 右半(cpu)}
    input  wire [2*ADDR_WIDTH-1:0]      frameb_base_addr,     //(I) {左半(acc), 右半(cpu)}
    input  wire [2*ADDR_WIDTH-1:0]      frame_full_base_addr, //(I) {全屏 C, 全屏 D}

    //------------------------------------------------------------
    // 事件
    //------------------------------------------------------------
    input  wire                         lineBuffer_frame_start,     //(I) 帧起始【写域单拍】

    //------------------------------------------------------------
    // 回给 APB
    //------------------------------------------------------------
    output wire [1:0]                   done_clr,                   //(O) {cpu,acc} 翻页消费后清位
    output wire [31:0]                  fps_acc_val,                //(O) 算好的帧率(APB 的格子)
    output wire [31:0]                  fps_cpu_val,

    //------------------------------------------------------------
    // 给 controller / 加速器
    //------------------------------------------------------------
    output wire                         lineBuffer_enable,
    output wire                         lineBuffer_mode,
    output wire [2*ADDR_WIDTH-1:0]      lineBuffer_addr
);

//==============================================================================
// localparam
//==============================================================================
localparam  WIN_W     = $clog2(FPS_WINDOW);     // 窗口计数器位宽
localparam  WIN_LAST  = FPS_WINDOW - 1;         // 数到第几个 frame_start 算满一秒

//==============================================================================
// wire / reg
//==============================================================================
// ---- 静态解码: 64 位基址拆成两半 ----
wire [ADDR_WIDTH-1:0]   framea_base_addr_acc;
wire [ADDR_WIDTH-1:0]   framea_base_addr_cpu;
wire [ADDR_WIDTH-1:0]   frameb_base_addr_acc;
wire [ADDR_WIDTH-1:0]   frameb_base_addr_cpu;
wire [ADDR_WIDTH-1:0]   framec_full_base_addr;
wire [ADDR_WIDTH-1:0]   framed_full_base_addr;

// ---- 索引状态 ----
reg                     mode_r;         // SPLIT 的生效值(帧边界锁存)
reg                     acc_idx_r;      // 加速器那一路:分屏 A(0)/B(1), 全屏 C(0)/D(1)
reg                     cpu_idx_r;      // CPU 那一路(右半):A(0)/B(1)

// ---- 帧率统计 ----
reg  [WIN_W-1:0]        win_cnt_r;      // 本窗口数到第几个 frame_start
reg  [31:0]             acc_win_r;      // 本窗口内加速器交了几帧
reg  [31:0]             cpu_win_r;      // 本窗口内 CPU 交了几帧
reg  [31:0]             fps_acc_r;      // 上一个窗口的帧率(锁存值)
reg  [31:0]             fps_cpu_r;
reg                     done_acc_d;     // DONE 打一拍, 用来挑 0->1
reg                     done_cpu_d;

// ---- 判断组合条件 ----
wire                    swap_acc;       // 本拍把 acc 那一路切到刚画完的那块
wire                    swap_cpu;       // 本拍把 cpu 那一路切到刚画完的那块
wire                    win_tick;       // 本拍是帧率窗口边界
wire                    mode_chg;       // 模式刚变(重开帧率窗口)
wire                    acc_evt;        // 加速器侧交帧事件(DONE_ACC 的 0->1)
wire                    cpu_evt;        // CPU 侧交帧事件(DONE_CPU 的 0->1)

// ---- 组合输出 ----
wire [ADDR_WIDTH-1:0]   seg0_addr;      // 段0(分屏=左半加速器 / 全屏=整帧)
wire [ADDR_WIDTH-1:0]   seg1_addr;      // 段1(分屏=右半CPU)

//==============================================================================
// assign 区：静态解码（高半 = acc, 低半 = cpu, 和 {段0,段1} 顺序一致）
//==============================================================================
assign  framea_base_addr_acc    = framea_base_addr[2*ADDR_WIDTH-1:ADDR_WIDTH];
assign  framea_base_addr_cpu    = framea_base_addr[ADDR_WIDTH-1:0];
assign  frameb_base_addr_acc    = frameb_base_addr[2*ADDR_WIDTH-1:ADDR_WIDTH];
assign  frameb_base_addr_cpu    = frameb_base_addr[ADDR_WIDTH-1:0];
assign  framec_full_base_addr   = frame_full_base_addr[2*ADDR_WIDTH-1:ADDR_WIDTH];
assign  framed_full_base_addr   = frame_full_base_addr[ADDR_WIDTH-1:0];

//==============================================================================
// 判断组合条件
//==============================================================================
// ---- 帧边界上的翻页(消费 DONE) ----
assign  swap_acc        = lineBuffer_frame_start & done_acc;
assign  swap_cpu        = lineBuffer_frame_start & done_cpu;
assign  done_clr        = {swap_cpu, swap_acc};         // 回给 APB 清 DONE 位

// ---- 帧率窗口 ----
assign  win_tick        = lineBuffer_frame_start & (win_cnt_r == WIN_LAST);
assign  mode_chg        = (mode_r != split_pending);

// ---- 交帧事件 = DONE 的 0->1(软件 W1S 写 和 加速器硬件脉冲 都走这一条路) ----
assign  acc_evt         = done_acc & ~done_acc_d;
assign  cpu_evt         = done_cpu & ~done_cpu_d;

//==============================================================================
// 组合输出
//==============================================================================
// 段0: 分屏 = 左半(加速器 A/B), 全屏 = 整帧(C/D) —— 都由 acc_idx 选
assign  seg0_addr       = mode_r ? (acc_idx_r ? frameb_base_addr_acc : framea_base_addr_acc)
                                : (acc_idx_r ? framed_full_base_addr : framec_full_base_addr);
// 段1: 分屏 = 右半(CPU A/B); 全屏不用(在下面置 0)
assign  seg1_addr       = cpu_idx_r ? frameb_base_addr_cpu : framea_base_addr_cpu;

assign  lineBuffer_addr   = {seg0_addr, mode_r ? seg1_addr : {ADDR_WIDTH{1'b0}}};
assign  lineBuffer_enable = enable;         // 使能不锁存: 要停就立刻停
assign  lineBuffer_mode   = mode_r;         // 模式帧边界生效
assign  fps_acc_val       = fps_acc_r;
assign  fps_cpu_val       = fps_cpu_r;

//==============================================================================
// 时序逻辑
//==============================================================================

//------------------------------------------------------------------------------
// 帧边界: 锁模式 + 翻索引 + 交帧事件打拍
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        mode_r      <= 1'b0;
        acc_idx_r   <= 1'b0;        // 上电默认显示 A / C
        cpu_idx_r   <= 1'b0;
        done_acc_d  <= 1'b0;
        done_cpu_d  <= 1'b0;
    end
    else begin
        // 模式到帧边界才生效(一帧中间的写不会换取数几何)
        if (lineBuffer_frame_start)
            mode_r <= split_pending;

        // 交帧事件打拍(用来挑 0->1)
        done_acc_d <= done_acc;
        done_cpu_d <= done_cpu;

        // 翻页: 切到"刚画完的那一块"
        if (swap_acc) acc_idx_r <= ~acc_idx_r;   // 分屏 A<->B; 全屏 C<->D
        if (swap_cpu) cpu_idx_r <= ~cpu_idx_r;
    end
end

//------------------------------------------------------------------------------
// 帧率统计: 窗口边界锁存, 窗口内累加
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        win_cnt_r   <= {WIN_W{1'b0}};
        acc_win_r   <= 32'd0;
        cpu_win_r   <= 32'd0;
        fps_acc_r   <= 32'd0;
        fps_cpu_r   <= 32'd0;
    end
    else if (mode_chg) begin
        // 模式变了: 重开窗口(不让两种模式的帧混在一个窗口里统计); 锁存值保留
        win_cnt_r   <= {WIN_W{1'b0}};
        acc_win_r   <= 32'd0;
        cpu_win_r   <= 32'd0;
    end
    else begin
        if (lineBuffer_frame_start)
            win_cnt_r <= win_tick ? {WIN_W{1'b0}} : win_cnt_r + 1'b1;

        if (win_tick) begin
            // 本拍若也有事件, 一起算进刚结束的这一个窗口
            fps_acc_r <= acc_win_r + (acc_evt ? 32'd1 : 32'd0);
            fps_cpu_r <= cpu_win_r + (cpu_evt ? 32'd1 : 32'd0);
            acc_win_r <= (acc_evt ? 32'd1 : 32'd0);
            cpu_win_r <= (cpu_evt ? 32'd1 : 32'd0);
        end
        else begin
            if (acc_evt) acc_win_r <= acc_win_r + 32'd1;
            if (cpu_evt) cpu_win_r <= cpu_win_r + 32'd1;
        end
    end
end

endmodule
