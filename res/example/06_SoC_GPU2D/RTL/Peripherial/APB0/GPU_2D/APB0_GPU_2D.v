//==============================================================================
//  APB0_gpu2d.v  ——  GPU_2D 的【唯一一个 APB0 总线解析层】
//------------------------------------------------------------------------------
//  为什么要合成一个
//      显示通道和加速器现在是一整条链(见 GPU_2D.v)，APB 信号也已经由
//      GPU_2D/interface/GPU2D_APBHandler.v 统一处理 —— 那总线层没理由还分两块：
//          · 一个片选、一套 APB 时序解析、一张寄存器表
//          · 原来 APB0_lineBuffer(槽1) + APB0_gpu2d(槽2) 两个模块 ➜ 本模块
//      ⇒ 外面只需一个基址(0xf8102000)，软件不用记两个外设。
//
//  例化在哪
//      在 APB0_Interconnect.v 里（外设总线层就该待在那儿），功能侧端口引到互连的端口上，
//      顶层再和 GPU_2D(功能层) 对接。"寄存器 <-> 功能"这组线因此只在顶层绕一下，
//      APB 口完全不经过顶层。
//
//  寄存器表（APB0 槽 2，绝对地址 = 0xF8102000 + 偏移）
//      子块位 = PADDR[6]:  0 = 观测  1 = 显示
//      两个子块基址都 64 字节对齐 ⇒ 子块内偏移 = PADDR[5:0]，不做加减法
//
//      ---- 子块 0: 0x2000 - 0x201F  加速器观测(全只读) ----
//0x2000   GPU2D_ID           RO     = 0x47503244 ("GP2D")  ← 通路自检
//0x2004   GPU2D_STATUS       RO     bit0 = BUSY(有命令没做完)
//0x2008   GPU2D_CMD_DONE     RO     已完成命令数
//0x200C   GPU2D_FRAME_DONE   RO     已完成帧数(FENCE 完成次数)
//0x2010   GPU2D_WR_BEATS     RO     已写出的 AXI 拍数
//0x2014   GPU2D_RD_BEATS     RO     已读回的 AXI 拍数
//0x2018   GPU2D_BUSY_CYC     RO     忙的周期数(算占用率, 分母软件自己数)
//
//      ---- 子块 1: 0x2040 - 0x206F  显示通道 ----
//0x2040   LINEBUFFER_ID      RO     = 0x4C423031 ("LB01")
//0x2044   FRAMEA_BASE_ACC    RW     缓冲A·左半(分屏时加速器画的)
//0x2048   FRAMEA_BASE_CPU    RW     缓冲A·右半(分屏时 CPU 画的)
//0x204C   FRAMEB_BASE_ACC    RW     缓冲B·左半
//0x2050   FRAMEB_BASE_CPU    RW     缓冲B·右半
//0x2054   FRAMEC_FULL        RW     缓冲C·全屏(加速器独占)
//0x2058   FRAMED_FULL        RW     缓冲D·全屏
//0x205C   STATUS             RW     bit0=ENABLE bit1=SPLIT(0=全屏 1=分屏)
//0x2060   FPS_ACC            RO     加速器帧率(GPU2D_APBHandler 算好送回来)
//0x2064   FPS_CPU            RO     CPU 帧率(同上)
//0x2068   DONE_CPU           RW1S   bit0: CPU 这半幅画完, 写1置位
//0x206C   DONE_ACC           RW1S   bit0: ACC 这半幅画完(加速器也可硬件置位)
//0x2070   IRQ_FRAME_EVT      RO/W1C bit0: 帧边界事件粘滞位(第 1 步先只做寄存器, 暂不接硬件事件)
//0x2074   IRQ_FRAME_EN       RW     bit0: 帧起始中断使能
//0x2078   CKEY_COLOR         RW     bit[23:0]: 颜色键控(ck)的键色, 复位默认 0xFFFFFF(白)
//                                    画精灵时"等于这个颜色的像素不写"(透明) —— 见 docs/spec/硬件_加速器.md §6
//
//  IRQ_frame_o(中断线): ★电平型, 寄存器直出 —— SoC 侧 SoC_RISCV32.v:6458 的 PLIC gateway
//      是 ip <= interrupt(跟随电平), 输入端还有 2FF 同步器 ⇒ 给稳定电平最保险。
//      置位: 帧边界(经 fps_cpu_val[31] 传来); 清零: 软件写 0x2070 的 bit0(W1C)
//
//  几条约定
//      · 观测子块【全只读】: 写入被忽略(但写到不存在的偏移仍然报 PSLVERROR)
//      · 观测计数器只增不减, 没有清零位(清零寄存器和自增同拍会打架)
//      · PREADY 恒 1: 零等待, 一次传输固定两拍; 读数据组合输出、当拍有效
//        ★ 别照抄 AHB 那套"锁一拍地址"——APB 不流水, ACCESS 相里地址全程稳定,
//          再锁一拍会让 PRDATA 晚一拍, 而 PREADY=1 已经把传输结束了 → 读到旧值
//      · 高位地址(PADDR[11:7])原样参与译码, 不截断: 截了 0x2080 会别名回 0x2000,
//        软件写错地址就静默改到别的寄存器上了 —— 现在它报 PSLVERROR
//
//  DONE 的规矩（软件最容易搞错的地方）
//      置位: CPU 写 1(W1S) / 加速器走 acc_frame_done 硬件脉冲
//      清零: 【只有显示侧 Handler 在帧边界翻页后发 done_clr 回来才清, 软件不许清】
//      写 SPLIT 的那一拍, 本模块自己把两个 DONE 丢掉(它们属于旧模式)
//      两位独立: acc 快就 acc 先翻, cpu 没画完就一直读它上一帧的地址
//      ★ 本模块例化在 APB0_Interconnect.v 里
//==============================================================================
`timescale 1ns/100ps

module APB0_gpu2d #(
    parameter ADDR_WIDTH = 32
)(
    input  wire                         clk,
    input  wire                         rst_n,

    //==================== APB3 从机侧(接 APB0_Interconnect) ====================
    input  wire [11:0]                  PADDR,
    input  wire                         PSEL,
    input  wire                         PENABLE,
    input  wire                         PWRITE,
    input  wire [31:0]                  PWDATA,
    output wire [31:0]                  PRDATA,
    output wire                         PREADY,
    output wire                         PSLVERROR,

    //==================== 显示子块(0x2040): 进 ====================
    input  wire                         acc_frame_done,         //(I) 加速器交帧【单拍脉冲】
    input  wire [1:0]                   done_clr,               //(I) {cpu,acc} 翻页消费后清位
    input  wire [31:0]                  fps_acc_val,            //(I) Handler 算好的帧率
    input  wire [31:0]                  fps_cpu_val,

    //==================== 显示子块(0x2040): 出 ====================
    output wire                         IRQ_frame_o,            //(O) 帧起始中断: ★寄存器直出【电平型】(软件写 0x2070 清)
    output wire                         enable,                 //(O) STATUS.ENABLE
    output wire                         split_pending,          //(O) STATUS.SPLIT(待生效值)
    output wire [2*ADDR_WIDTH-1:0]      framea_base_addr,       //(O) {左半(acc), 右半(cpu)}
    output wire [2*ADDR_WIDTH-1:0]      frameb_base_addr,       //(O) {左半(acc), 右半(cpu)}
    output wire [2*ADDR_WIDTH-1:0]      frame_full_base_addr,   //(O) {全屏 C, 全屏 D}
    output wire                         done_gpu2d,
    output wire                         done_cpu,
    output wire [23:0]                  ckey_color,             //(O) 颜色键控的键色(复位默认白)

    //==================== 观测子块(0x2000): 进(Handler 数好的值) ====================
    input  wire [31:0]                  busy_val,               //(I) STATUS.BUSY
    input  wire [31:0]                  cmd_done_val,           //(I) 已完成命令数
    input  wire [31:0]                  frame_done_val,         //(I) 已完成帧数
    input  wire [31:0]                  wr_beats_val,           //(I) 已写出的 AXI 拍数
    input  wire [31:0]                  rd_beats_val,           //(I) 已读回的 AXI 拍数
    input  wire [31:0]                  busy_cyc_val            //(I) 忙的周期数
);

//==============================================================================
// localparam
//==============================================================================
localparam  GPU2D_ID        = 32'h47503244;    // "GP2D"
localparam  LINEBUFFER_ID   = 32'h4C423031;    // "LB01"
localparam  OBS_LAST        = 4'd6;            // 观测子块最后一个寄存器号(0x2018)
localparam  DISP_LAST       = 4'd14;           // 显示子块最后一个寄存器号(0x2078)
//   ★ DISP_LAST 必须跟着改: hit_disp 用它判"地址命中", 漏了会让 0x2078 走 PSLVERROR

//==============================================================================
// wire / reg
//==============================================================================
//   ---- 子块选择与本地偏移 ----
wire                    sel_obs;        // 子块 0: 加速器观测
wire                    sel_disp;       // 子块 1: 显示通道
wire [11:0]             pa_obs;         // 观测格子看到的地址(原样)
wire [11:0]             pa_disp;        // 显示格子看到的地址(抹掉子块基址那一位)
wire [3:0]              reg_sel_obs;
wire [3:0]              reg_sel_disp;
wire                    hit_obs;        // 观测: 地址命中
wire                    hit_disp;       // 显示: 地址命中

//   ---- 总线解析 ----
wire                    trans_en;
wire                    write_en;
wire                    read_en;
wire                    read_err;
wire                    write_err;
wire                    split_change;

//   ---- 交帧事件 ----
wire                    acc_evt;        // 加速器交帧事件
wire                    cpu_evt;        // CPU 交帧事件
wire                    acc_wr_set;     // 软件 W1S 写 DONE_ACC

//   ---- 寄存器 ----
reg  [31:0]             PRDATA_r;
reg  [ADDR_WIDTH-1:0]   framea_acc_r;
reg  [ADDR_WIDTH-1:0]   framea_cpu_r;
reg  [ADDR_WIDTH-1:0]   frameb_acc_r;
reg  [ADDR_WIDTH-1:0]   frameb_cpu_r;
reg  [ADDR_WIDTH-1:0]   framec_full_r;
reg  [ADDR_WIDTH-1:0]   framed_full_r;
reg  [31:0]             status_r;       // {SPLIT_pending, ENABLE}
reg                     done_cpu_r;
reg                     done_gpu2d_r;
reg  [23:0]             ckey_color_r;   // 颜色键控(ck)的键色

//==============================================================================
// assign 区：子块译码
//==============================================================================
//   子块位 = PADDR[6]; 子块内偏移 = PADDR[5:0]
//   ★ pa_disp 把"子块基址 0x40"那一位抹掉 ⇒ 0x2040 在显示格子里就是 0x00
//   ★ 高位(PADDR[11:7])必须留着: 它让 0x2080 这种地址不命中, 而不是别名
assign  sel_obs         = PSEL & ~PADDR[6];
assign  sel_disp        = PSEL &  PADDR[6];
assign  pa_obs          = PADDR;
assign  pa_disp         = {PADDR[11:7], 1'b0, PADDR[5:0]};

assign  reg_sel_obs     = pa_obs[5:2];
assign  reg_sel_disp    = pa_disp[5:2];

assign  hit_obs         = (pa_obs[11:6]  == 6'h00) & (reg_sel_obs  <= OBS_LAST);
assign  hit_disp        = (pa_disp[11:6] == 6'h00) & (reg_sel_disp <= DISP_LAST);

//==============================================================================
// assign 区：APB3 总线解析（两相: SETUP 只有 PSEL; ACCESS 有 PSEL+PENABLE）
//==============================================================================
assign  trans_en        = PSEL & PENABLE;
assign  write_en        = trans_en &  PWRITE;
assign  read_en         = trans_en & ~PWRITE;

assign  PREADY          = 1'b1;                     // 零等待,一次传输固定两拍

//   非法判据: 被选中的子块里地址没命中 —— 读和写各一条, 都在 ACCESS 相才可能为 1
assign  read_err        = read_en  & ((sel_obs & ~hit_obs) | (sel_disp & ~hit_disp));
assign  write_err       = write_en & ((sel_obs & ~hit_obs) | (sel_disp & ~hit_disp));

assign  PSLVERROR       = read_err | write_err;

//==============================================================================
// 写寄存器（只有显示子块有 RW / W1S；观测子块写进来一律忽略）
//==============================================================================
//   基址必须 16 字节对齐(AXI 起始地址按传输大小对齐)。
//   不对齐的写【直接丢掉】, 读回还是旧值 —— 软件据此能发现写错了
assign  acc_wr_set      = write_en & ~write_err & sel_disp & (reg_sel_disp == 4'd11) & PWDATA[0];
assign  acc_evt         = (acc_frame_done | acc_wr_set) & ~done_gpu2d_r;
assign  cpu_evt         = write_en & ~write_err & sel_disp & (reg_sel_disp == 4'd10) & PWDATA[0] & ~done_cpu_r;

assign  split_change    = write_en & ~write_err & sel_disp & (reg_sel_disp == 4'd7) & (PWDATA[1] != status_r[1]);

//==============================================================================
// 功能侧输出：显示子块的静态值
//==============================================================================
assign  enable                    = status_r[0];
assign  split_pending             = status_r[1];
// 打包出去: 高位 = acc 那一半, 低位 = cpu 那一半(和 lineBuffer_addr 的 {段0,段1} 一致)
assign  framea_base_addr          = {framea_acc_r,  framea_cpu_r};
assign  frameb_base_addr          = {frameb_acc_r,  frameb_cpu_r};
assign  frame_full_base_addr      = {framec_full_r, framed_full_r};
assign  done_gpu2d                  = done_gpu2d_r;
assign  done_cpu                  = done_cpu_r;
assign  ckey_color                = ckey_color_r;

//==============================================================================
// IRQ: 帧起始中断的两个寄存器(第 1 步: 只做寄存器, 事件先不接硬件)
//==============================================================================
reg                     IRQ_frame_evt_r;        // 事件粘滞位(软件 W1C 清)
reg                     IRQ_frame_en_r;         // 使能位
reg                     IRQ_frame_o_r;          // ★ 直接驱动中断线的那一位(寄存器直出)

//   userInterruptA 是【电平型】(SoC_RISCV32.v:6458: ip <= interrupt), 输入端还有 2FF 同步器,
//   所以这里给寄存器电平: 帧边界置位 -> 一直保持 -> 软件写 0x2070 清才落下。
//   1 拍脉冲不保险(同步器有概率漏), 电平一定被采到。
assign  IRQ_frame_o = IRQ_frame_o_r;

//==============================================================================
// 时序逻辑：写寄存器 + DONE 的置位/清零
//==============================================================================
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        framea_acc_r    <= {ADDR_WIDTH{1'b0}};
        framea_cpu_r    <= {ADDR_WIDTH{1'b0}};
        frameb_acc_r    <= {ADDR_WIDTH{1'b0}};
        frameb_cpu_r    <= {ADDR_WIDTH{1'b0}};
        framec_full_r   <= {ADDR_WIDTH{1'b0}};
        framed_full_r   <= {ADDR_WIDTH{1'b0}};
        status_r        <= 32'd0;
        done_cpu_r      <= 1'b0;
        done_gpu2d_r      <= 1'b0;
        //   ★ 复位默认【白】: 软件不写它时, 行为和以前完全一样(透明像素 = 白)
        ckey_color_r    <= 24'hFFFFFF;
        IRQ_frame_evt_r <= 1'b0;
        IRQ_frame_en_r  <= 1'b0;
        IRQ_frame_o_r   <= 1'b0;
    end
    else begin
        // ---- IRQ: 帧边界事件 ----
        //   ★ 事件不另开端口: Handler 把它塞在 fps_cpu_val 的最高位传过来(1 拍脉冲),
        //     这里收到就置粘滞位; 软件写 0x2070 清。全程【不动任何端口和顶层线】。
        if (fps_cpu_val[31]) IRQ_frame_evt_r <= 1'b1;

        // ---- 中断线: 寄存器直出(电平 = 有事件 且 已使能) ----
        IRQ_frame_o_r <= IRQ_frame_evt_r & IRQ_frame_en_r;

        // ---- DONE: 交帧事件置位 ----
        if (acc_evt) done_gpu2d_r <= 1'b1;
        if (cpu_evt) done_cpu_r <= 1'b1;

        // ---- DONE: 显示侧 Handler 翻页消费后清位 ----
        if (done_clr[0]) done_gpu2d_r <= 1'b0;
        if (done_clr[1]) done_cpu_r <= 1'b0;

        // ---- APB 写(只认显示子块) ----
        if (write_en && !write_err && sel_disp) begin
            case (reg_sel_disp)
                4'd1    : if (PWDATA[3:0] == 4'h0) framea_acc_r  <= PWDATA;   // 0x2044
                4'd2    : if (PWDATA[3:0] == 4'h0) framea_cpu_r  <= PWDATA;   // 0x2048
                4'd3    : if (PWDATA[3:0] == 4'h0) frameb_acc_r  <= PWDATA;   // 0x204C
                4'd4    : if (PWDATA[3:0] == 4'h0) frameb_cpu_r  <= PWDATA;   // 0x2050
                4'd5    : if (PWDATA[3:0] == 4'h0) framec_full_r <= PWDATA;   // 0x2054
                4'd6    : if (PWDATA[3:0] == 4'h0) framed_full_r <= PWDATA;   // 0x2058
                4'd7    : status_r <= {30'd0, PWDATA[1:0]};                   // 0x205C
                4'd12   : if (PWDATA[0]) IRQ_frame_evt_r <= 1'b0;             // 0x2070 W1C
                4'd13   : IRQ_frame_en_r <= PWDATA[0];                        // 0x2074 使能
                4'd14   : ckey_color_r <= PWDATA[23:0];                       // 0x2078 键色(24 位色)
                default : ;
            endcase
        end

        // ---- 切模式: 旧模式的 DONE 丢掉(留着会在帧边界拿它去翻新模式的索引) ----
        if (split_change) begin
            done_gpu2d_r <= 1'b0;
            done_cpu_r <= 1'b0;
        end
    end
end

//==============================================================================
// 读寄存器（组合）
//      读是组合的 —— ACCESS 相里 PRDATA 必须当拍有效, SoC 桥在同一个边沿就采走
//==============================================================================
always @(*) begin
    PRDATA_r = 32'd0;
    if (read_en && !read_err) begin
        if (sel_disp) begin
            case (reg_sel_disp)
                4'd0    : PRDATA_r = LINEBUFFER_ID;                     // 0x2040
                4'd1    : PRDATA_r = framea_acc_r;                      // 0x2044
                4'd2    : PRDATA_r = framea_cpu_r;                      // 0x2048
                4'd3    : PRDATA_r = frameb_acc_r;                      // 0x204C
                4'd4    : PRDATA_r = frameb_cpu_r;                      // 0x2050
                4'd5    : PRDATA_r = framec_full_r;                     // 0x2054
                4'd6    : PRDATA_r = framed_full_r;                     // 0x2058
                4'd7    : PRDATA_r = {30'd0, status_r[1:0]};            // 0x205C(回读你写进去的值)
                4'd8    : PRDATA_r = fps_acc_val;                       // 0x2060
                4'd9    : PRDATA_r = fps_cpu_val;                       // 0x2064
                4'd10   : PRDATA_r = {31'd0, done_cpu_r};               // 0x2068 DONE_CPU
                4'd11   : PRDATA_r = {31'd0, done_gpu2d_r};               // 0x206C DONE_ACC
                4'd12   : PRDATA_r = {31'd0, IRQ_frame_evt_r};          // 0x2070
                4'd13   : PRDATA_r = {31'd0, IRQ_frame_en_r};           // 0x2074
                4'd14   : PRDATA_r = {8'd0, ckey_color_r};              // 0x2078 键色
                default : PRDATA_r = 32'h0;
            endcase
        end
        else begin                                                  // 观测子块
            case (reg_sel_obs)
                4'd0    : PRDATA_r = GPU2D_ID;                          // 0x2000
                4'd1    : PRDATA_r = busy_val;                          // 0x2004 STATUS
                4'd2    : PRDATA_r = cmd_done_val;                      // 0x2008
                4'd3    : PRDATA_r = frame_done_val;                    // 0x200C
                4'd4    : PRDATA_r = wr_beats_val;                      // 0x2010
                4'd5    : PRDATA_r = rd_beats_val;                      // 0x2014
                4'd6    : PRDATA_r = busy_cyc_val;                      // 0x2018
                default : PRDATA_r = 32'h0;
            endcase
        end
    end
    else
        PRDATA_r = 32'd0;
end

assign  PRDATA          = PRDATA_r;

endmodule
