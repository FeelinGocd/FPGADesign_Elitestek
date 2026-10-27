//==============================================================================
//  GPU2D_APBHandler.v —— GPU_2D 的【Handler 集中层】
//------------------------------------------------------------------------------
//  角色
//      GPU_2D 里所有"寄存器值 <-> 功能侧有效信号"的翻译都集中在这一个模块：
//
//        ① 显示侧 Handler（原 LineBuffer/bufferAPBHandler.v 搬进来的）
//              静态寄存器值 -> 当前该显示的两段地址 / 使能 / 模式
//              帧边界消费 DONE -> 翻索引 + 回 done_clr 清位
//              帧率统计（窗口计数 -> fps_*_val 回寄存器格子）
//        ② 加速器侧 Handler（原来就在这个文件里）
//              事件 -> 计数 -> 观测值（忙/命令数/帧数/写读拍/忙周期）
//
//      ★ 为什么并到一起: 这两半本来是同一件事(寄存器与功能之间的一层),
//        分开就得让"寄存器格子 -> 显示通道"的线绕两个模块走。
//
//  ---------------------------------------------------------------------------
//  ★★★ 复位域（这个文件踩过一个很贵的坑，务必看懂再改）★★★ tips：确实很贵，花了快10块钱的token
//      rst_n       加速器侧(观测计数) = axi_rst_n
//      rst_n_disp  显示侧(翻页/帧率)  = lineBuffer_rst_n (= axi_rst_n & HDMI PLL 锁定)
//
//      · 显示侧的翻页/帧率必须等显示通路活着才动, 所以吃 rst_n_disp
//      · 【但是】加速器那一路的行模式(lineBuffer_mode 给 CmdChannel/CmdExec 用的)
//        【绝对不能】挂在这上面: 它一旦跟着 pll_HDMI_locked 被摁住, 加速器就会
//        按错误的几何算地址 -> 命令收发卡死 -> CI 是阻塞握手的 -> CPU 停住 ->
//        【调试器再也停不住 CPU】(vexriscv 断言崩溃), 表现成"整块板子没法调试"。
//      · 所以本模块的 lineBuffer_mode 输出【只给显示通道】;
//        给加速器的行模式由 GPU_2D 里那个 lineBuffer_mode_r 单独锁存(复位 rst_acc_n)。
//      · 教训: 合并两个模块时, 复位域要【显式】合并 —— 别让一个寄存器悄悄换域。
//        仿真里如果两个复位总是同一条线, 这个坑永远测不出来。
//  ---------------------------------------------------------------------------
//
//  显示侧：翻页规则（原 bufferAPBHandler 的文件头，一并搬过来）
//      lineBuffer_frame_start 那一拍看 done:
//          done_gpu2d=1 -> 翻 acc_idx(分屏切左半, 全屏切整帧) + done_clr[0]
//          done_cpu=1 -> 翻 cpu_idx(右半)                    + done_clr[1]
//      两位独立: acc 快就 acc 先翻, cpu 没画完就一直读它上一帧的地址。
//      模式(SPLIT)到帧边界才生效 —— 一帧中间的写不会换取数几何。
//      enable 不锁存: 要停就立刻停。
//
//  给 controller 的约定
//      lineBuffer_addr = {段0基址, 段1基址}
//          分屏(SPLIT=1): 段0 = 左半(加速器), 段1 = 右半(CPU)
//          全屏(SPLIT=0): 段0 = 整帧, 段1 恒 0
//
//  加速器侧：为什么计数放这里（原 GPU2D_APBHandler 的文件头）
//      · 和显示侧保持一致（显示侧的帧率也是在这一层数的）
//      · 数据通路那几级（CmdExec / WriteController / DDRSource）【一行都不用改】,
//        只把事件脉冲引出来即可 —— 观测是外挂, 不往数据通路里掺东西
//      · 以后加新的观测项, 只动这一个文件
//      不做"软件清零": 清零寄存器和自增同拍会打架; 软件记快照即可, 值只增不减。
//
//  位宽
//      帧率/拍数/周期数都是 32 位。拍数 @100MHz 约 43 秒回绕一次 —— 观测够用, 不处理。
//
//  上一级是谁
//      GPU_2D.v（只观测，不影响功能）

//==============================================================================
`timescale 1ns/100ps

module GPU2D_APBHandler #(
    parameter ADDR_WIDTH = 32,
    parameter FPS_WINDOW = 60                // 帧率窗口: 几个 frame_start 算一秒(720p60 = 60)
)(
    //==================== 时钟 / 复位 ====================
    input  wire                         clk,            //(I) 命令/加速器域时钟(= user_clk)
    input  wire                         rst_n,          //(I) 加速器侧复位(观测计数)
    input  wire                         rst_n_disp,     //(I) 显示侧复位(= lineBuffer_rst_n)

    //==================== 来自 APB 寄存器格子(APB0_gpu2d 显示子块)：静态值 + DONE  ↔ APB0_GPU_2D（总线层寄存器格子）  ====================
    input  wire                         enable,                 //(I) STATUS.ENABLE
    input  wire                         split_pending,          //(I) STATUS.SPLIT(待生效值)
    input  wire [2*ADDR_WIDTH-1:0]      framea_base_addr,       //(I) {左半(acc), 右半(cpu)}
    input  wire [2*ADDR_WIDTH-1:0]      frameb_base_addr,       //(I) {左半(acc), 右半(cpu)}
    input  wire [2*ADDR_WIDTH-1:0]      frame_full_base_addr,   //(I) {全屏 C, 全屏 D}
    input  wire                         done_gpu2d,             //(I) 加速器那半有帧等着显示
    input  wire                         done_cpu,               //(I) CPU 那半有帧等着显示

    //==================== 回给 APB 寄存器格子  ↔ APB0_GPU_2D（总线层寄存器格子）  ====================
    output wire [1:0]                   done_clr,               //(O) {cpu,acc} 翻页消费后清位
    output wire [31:0]                  fps_acc_val,            //(O) 算好的帧率(APB 的格子)
    output wire [31:0]                  fps_cpu_val,

    //==================== 显示事件：帧边界 ====================
    input  wire                         lineBuffer_frame_start, //(I) 帧起始【写域单拍】

    //==================== 给显示通道 controller（★ 只给显示，不给加速器）  ←→ lineBuffer/lineBuffer  ====================
    output wire                         lineBuffer_enable,      //(O) 使能(不锁存)
    output wire                         lineBuffer_mode,        //(O) 显示侧生效的模式(帧边界锁存)
    output wire [2*ADDR_WIDTH-1:0]      lineBuffer_addr,        //(O) {段0基址, 段1基址}

    //==================== 加速器内部的事件 / 状态 ====================
    input  wire                         acc_busy,               //(I) 电平: 手上有命令没做完
    input  wire                         cmd_done,               //(I) 单拍: 这一条命令真做完了
    input  wire                         frame_done,             //(I) 单拍: FENCE 完成 = 本帧画完
    input  wire                         wr_beat_hs,             //(I) 写出一拍(wr_valid & wr_ready)
    input  wire                         rd_beat_hs,             //(I) 读回一拍(rd_valid & rd_ready)

    //==================== 回给 APB 寄存器格子(槽 2：全只读观测)  ↔ APB0_GPU_2D（总线层寄存器格子）  ====================
    output wire [31:0]                  busy_val,               //(O) STATUS.BUSY
    output wire [31:0]                  cmd_done_val,           //(O) 已完成命令数
    output wire [31:0]                  frame_done_val,         //(O) 已完成帧数
    output wire [31:0]                  wr_beats_val,           //(O) 已写出的 AXI 拍数
    output wire [31:0]                  rd_beats_val,           //(O) 已读回的 AXI 拍数
    output wire [31:0]                  busy_cyc_val            //(O) 忙的周期数
);

//==============================================================================
// localparam
//==============================================================================
localparam  WIN_W     = $clog2(FPS_WINDOW);     // 窗口计数器位宽
localparam  WIN_LAST  = FPS_WINDOW - 1;         // 数到第几个 frame_start 算满一秒

//==============================================================================
// wire / reg
//==============================================================================
//   ---- 静态解码: 64 位基址拆成两半 ----
wire [ADDR_WIDTH-1:0]   framea_base_addr_acc;
wire [ADDR_WIDTH-1:0]   framea_base_addr_cpu;
wire [ADDR_WIDTH-1:0]   frameb_base_addr_acc;
wire [ADDR_WIDTH-1:0]   frameb_base_addr_cpu;
wire [ADDR_WIDTH-1:0]   framec_full_base_addr;
wire [ADDR_WIDTH-1:0]   framed_full_base_addr;

//   ---- 索引状态(显示侧) ----
reg                     mode_r;         // SPLIT 的生效值(帧边界锁存)
reg                     acc_idx_r;      // 加速器那一路:分屏 A(0)/B(1), 全屏 C(0)/D(1)
reg                     cpu_idx_r;      // CPU 那一路(右半):A(0)/B(1)

//   ---- 帧率统计(显示侧) ----
reg  [WIN_W-1:0]        win_cnt_r;      // 本窗口数到第几个 frame_start
reg  [31:0]             acc_win_r;      // 本窗口内加速器交了几帧
reg  [31:0]             cpu_win_r;      // 本窗口内 CPU 交了几帧
reg  [31:0]             fps_acc_r;      // 上一个窗口的帧率(锁存值)
reg  [31:0]             fps_cpu_r;
reg                     done_gpu2d_d;   // DONE 打一拍, 用来挑 0->1
reg                     done_cpu_d;

//   ---- 观测计数(加速器侧, 只增不减) ----
reg  [31:0]             cmd_done_cnt_r;
reg  [31:0]             frame_done_cnt_r;
reg  [31:0]             wr_beats_cnt_r;
reg  [31:0]             rd_beats_cnt_r;
reg  [31:0]             busy_cyc_cnt_r;

//   ---- 判断组合条件 ----
wire                    swap_acc;       // 本拍把 acc 那一路切到刚画完的那块
wire                    swap_cpu;       // 本拍把 cpu 那一路切到刚画完的那块
wire                    win_tick;       // 本拍是帧率窗口边界
wire                    mode_chg;       // 模式刚变(重开帧率窗口)
wire                    acc_evt;        // 加速器侧交帧事件(DONE_ACC 的 0->1)
wire                    cpu_evt;        // CPU 侧交帧事件(DONE_CPU 的 0->1)

//   ---- 组合输出 ----
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
//   ---- 帧边界上的翻页(消费 DONE) ----
assign  swap_acc        = lineBuffer_frame_start & done_gpu2d;
assign  swap_cpu        = lineBuffer_frame_start & done_cpu;
assign  done_clr        = {swap_cpu, swap_acc};         // 回给 APB 清 DONE 位

//   ---- 帧率窗口 ----
assign  win_tick        = lineBuffer_frame_start & (win_cnt_r == WIN_LAST);
assign  mode_chg        = (mode_r != split_pending);

//   ---- 交帧事件 = DONE 的 0->1(软件 W1S 写 和 加速器硬件脉冲 都走这一条路) ----
assign  acc_evt         = done_gpu2d & ~done_gpu2d_d;
assign  cpu_evt         = done_cpu & ~done_cpu_d;

//==============================================================================
// 组合输出
//==============================================================================
//   段0: 分屏 = 左半(加速器 A/B), 全屏 = 整帧(C/D) —— 都由 acc_idx 选
assign  seg0_addr       = mode_r ? (acc_idx_r ? frameb_base_addr_acc : framea_base_addr_acc)
                                : (acc_idx_r ? framed_full_base_addr : framec_full_base_addr);
//   段1: 分屏 = 右半(CPU A/B); 全屏不用(在下面置 0)
assign  seg1_addr       = cpu_idx_r ? frameb_base_addr_cpu : framea_base_addr_cpu;

assign  lineBuffer_addr   = {seg0_addr, mode_r ? seg1_addr : {ADDR_WIDTH{1'b0}}};
assign  lineBuffer_enable = enable;         // 使能不锁存: 要停就立刻停
assign  lineBuffer_mode   = mode_r;         // ★ 只给显示通道; 加速器的行模式在 GPU_2D 里另锁存
assign  fps_acc_val       = fps_acc_r;
//   ★ 借 fps_cpu_val 的最高位把"帧边界脉冲"送给 APB 寄存器块:
//     这条 32 位线本来就存在(Handler -> GPU_2D 输出端口 -> APB0_gpu2d),
//     所以【不用新开任何端口、不用动顶层走线】—— 加了端口就走不通那次就是栽在这。
//     bit31 只有 1 拍宽; 软件读 CPU 帧率时把 bit31 掩掉(& 0x7FFFFFFF)即可。
wire    IRQ_frame_pulse   = lineBuffer_frame_start;      // 帧起始脉冲(借 fps_cpu_val[31] 传出去)
assign  fps_cpu_val       = fps_cpu_r | {IRQ_frame_pulse, 31'd0};

//   加速器观测(组合直出, 值来自下面的计数器)
assign  busy_val       = {31'd0, acc_busy};
assign  cmd_done_val   = cmd_done_cnt_r;
assign  frame_done_val = frame_done_cnt_r;
assign  wr_beats_val   = wr_beats_cnt_r;
assign  rd_beats_val   = rd_beats_cnt_r;
assign  busy_cyc_val   = busy_cyc_cnt_r;

//==============================================================================
// 时序逻辑 · 显示侧：帧边界锁模式 + 翻索引 + 交帧事件打拍
//==============================================================================
always @(posedge clk or negedge rst_n_disp) begin
    if (!rst_n_disp) begin
        mode_r      <= 1'b0;
        acc_idx_r   <= 1'b0;        // 上电默认显示 A / C
        cpu_idx_r   <= 1'b0;
        done_gpu2d_d  <= 1'b0;
        done_cpu_d  <= 1'b0;
    end
    else begin
        //   模式到帧边界才生效(一帧中间的写不会换取数几何)
        if (lineBuffer_frame_start)
            mode_r <= split_pending;

        //   交帧事件打拍(用来挑 0->1)
        done_gpu2d_d <= done_gpu2d;
        done_cpu_d <= done_cpu;

        //   翻页: 切到"刚画完的那一块"
        if (swap_acc) acc_idx_r <= ~acc_idx_r;   // 分屏 A<->B; 全屏 C<->D
        if (swap_cpu) cpu_idx_r <= ~cpu_idx_r;
    end
end

//==============================================================================
// 时序逻辑 · 显示侧：帧率统计（窗口边界锁存, 窗口内累加）
//==============================================================================
always @(posedge clk or negedge rst_n_disp) begin
    if (!rst_n_disp) begin
        win_cnt_r   <= {WIN_W{1'b0}};
        acc_win_r   <= 32'd0;
        cpu_win_r   <= 32'd0;
        fps_acc_r   <= 32'd0;
        fps_cpu_r   <= 32'd0;
    end
    else if (mode_chg) begin
        //   模式变了: 重开窗口(不让两种模式的帧混在一个窗口里统计); 锁存值保留
        win_cnt_r   <= {WIN_W{1'b0}};
        acc_win_r   <= 32'd0;
        cpu_win_r   <= 32'd0;
    end
    else begin
        if (lineBuffer_frame_start)
            win_cnt_r <= win_tick ? {WIN_W{1'b0}} : win_cnt_r + 1'b1;

        if (win_tick) begin
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

//==============================================================================
// 时序逻辑 · 加速器侧：纯计数（没有任何控制逻辑）
//   每个事件各一个独立计数器, 互不干扰; 全部只增不减
//      ★ 复位用 rst_n(加速器侧), 和显示侧彻底分开
//==============================================================================
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        cmd_done_cnt_r   <= 32'd0;
        frame_done_cnt_r <= 32'd0;
        wr_beats_cnt_r   <= 32'd0;
        rd_beats_cnt_r   <= 32'd0;
        busy_cyc_cnt_r   <= 32'd0;
    end
    else begin
        if (cmd_done)   cmd_done_cnt_r   <= cmd_done_cnt_r   + 32'd1;
        if (frame_done) frame_done_cnt_r <= frame_done_cnt_r + 32'd1;
        if (wr_beat_hs) wr_beats_cnt_r   <= wr_beats_cnt_r   + 32'd1;
        if (rd_beat_hs) rd_beats_cnt_r   <= rd_beats_cnt_r   + 32'd1;
        if (acc_busy)   busy_cyc_cnt_r   <= busy_cyc_cnt_r   + 32'd1;
    end
end

endmodule
