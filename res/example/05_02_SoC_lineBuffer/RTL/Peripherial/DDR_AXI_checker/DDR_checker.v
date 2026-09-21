//==============================================================================
//  DDR_checker.v  ——  DDR 读写自检(自包含模块:APB 状态 + AXI 主口)
//------------------------------------------------------------------------------
//  干什么:
//      把一块图案从 BRAM 写进 DDR,再原样读回来逐拍比对,
//      结果通过 run/busy/... 一组信号交给 APB 寄存器(APB0_DDR_checker)。
//      整个过程不依赖 CPU。
//
//  流程:
//      run -> 图案自检 -> 写满整个测试区 -> 整体读回比对 -> 置 done/pass
//      (先写满再整体读回,不用"写一笔立刻读一笔" —— 那样可能命中写缓冲,
//       证明不了数据真的落到 DDR 了)
//
//  接口两侧:
//      左:run / busy / done / pass / errcode / err_addr / err_exp / err_act
//          -> 接 APB0_DDR_checker(在 APB0_Interconnect 里)
//      右:m_AXI_*(39 根) -> 接 Axi_Mux 的 S1
//          内部自带 AXI4_Master,所以本模块是完整的"APB + AXI"外设
//
//  errcode:
//      0 = 无错误                     1 = 读回不一致
//      2 = 响应非 OKAY(RRESP)         3 = 超时
//      4 = 图案未加载                 见下面「假通过」说明
//
//  !! 一笔必须跑完 !!
//      自检出错时**只能在笔边界收工**,不能中途停:
//      中途把 wr_valid / rd_ready 拉低会让 Master 卡在 W/R 状态,
//      而从机还占着通道等 WLAST/RLAST —— 单 outstanding 下无法恢复,
//      整条 AXI 数据通道会被永久卡死(连 CPU 一起遭殃)。
//      所以:先让本笔 cmd_done,再切 S_FINISH。
//
//  !! 假通过警告 !!
//      如果 PATTERN_INIT 路径不对,BRAM 会全是 0,而 DDR 初值也是 0,
//      "写 0 读 0"会一路通过。所以 S_CHK 状态会先检查图案首字非 0,
//      全 0 直接报 errcode = 4 并结束。
//
//  ★ rst_n 的约定:
//      本模块只认 rst_n。系统级门控(例如"DDR 校准完成")由**顶层**并进 rst_n,
//      再送进来。所以在 cal_done 之前本模块一直处于复位,此时写 CTRL.start 无效。
//
//  依赖:
//      AXI4_Master(AXI4/Master/) —— 通用主设备,本模块内部例化
//      BlockRAM                  —— 图案存储
//==============================================================================
`timescale 1ns/100ps

module DDR_checker #(
    parameter ADDR_WIDTH     = 32,
    parameter DATA_WIDTH     = 128,
    parameter STRB_WIDTH     = DATA_WIDTH/8,
    parameter ID_WIDTH       = 8,
    parameter LEN_WIDTH      = 8,
    parameter AXI_ID         = {ID_WIDTH{1'b0}},
    parameter TEST_BASE      = 32'h0020_0000,   // 测试区起始地址(FB-A)
    parameter TEST_BURSTS    = 16,              // 测试区大小 = TEST_BURSTS × 4KB
    parameter BURST_LEN      = 8'd255,          // 每笔 256 拍 = 4KB
    parameter TIMEOUT_CYCLES = 1_000_000,       // 单笔超时(100MHz 下约 10ms)
    parameter PATTERN_INIT   = "../res/test_Initial.hex"
)(
    input  wire                     clk,
    input  wire                     rst_n,          // 系统级门控由顶层并进来(见文件头)

    //------------------------------------------------------------
    // 控制 / 状态(接 APB0_DDR_checker)
    //------------------------------------------------------------
    input  wire                     run,            // 启动脉冲,单周期
    output wire                     busy,           // 自检进行中
    output wire                     done,           // 跑完了(粘滞到下次 run)
    output wire                     pass,           // 跑完且没错
    output wire [7:0]               errcode,        // 第一条错误
    output wire [ADDR_WIDTH-1:0]    err_addr,       // 第一个不一致的地址
    output wire [31:0]              err_exp,        // 期望值(低 32 位)
    output wire [31:0]              err_act,        // 实际值(低 32 位)

    //------------------------------------------------------------
    // AXI4 主口(接 Axi_Mux 的 S1)
    //------------------------------------------------------------
    // ---- AW ----
    output wire [ID_WIDTH-1:0]      m_AXI_AWID,
    output wire [ADDR_WIDTH-1:0]    m_AXI_AWADDR,
    output wire [LEN_WIDTH-1:0]     m_AXI_AWLEN,
    output wire [2:0]               m_AXI_AWSIZE,
    output wire [1:0]               m_AXI_AWBURST,
    output wire                     m_AXI_AWLOCK,
    output wire [3:0]               m_AXI_AWCACHE,
    output wire [2:0]               m_AXI_AWPROT,
    output wire [3:0]               m_AXI_AWQOS,
    output wire [3:0]               m_AXI_AWREGION,
    output wire                     m_AXI_AWVALID,
    input  wire                     m_AXI_AWREADY,
    // ---- W ----
    output wire [DATA_WIDTH-1:0]    m_AXI_WDATA,
    output wire [STRB_WIDTH-1:0]    m_AXI_WSTRB,
    output wire                     m_AXI_WLAST,
    output wire                     m_AXI_WVALID,
    input  wire                     m_AXI_WREADY,
    // ---- B ----
    input  wire [ID_WIDTH-1:0]      m_AXI_BID,
    input  wire [1:0]               m_AXI_BRESP,
    input  wire                     m_AXI_BVALID,
    output wire                     m_AXI_BREADY,
    // ---- AR ----
    output wire [ID_WIDTH-1:0]      m_AXI_ARID,
    output wire [ADDR_WIDTH-1:0]    m_AXI_ARADDR,
    output wire [LEN_WIDTH-1:0]     m_AXI_ARLEN,
    output wire [2:0]               m_AXI_ARSIZE,
    output wire [1:0]               m_AXI_ARBURST,
    output wire                     m_AXI_ARLOCK,
    output wire [3:0]               m_AXI_ARCACHE,
    output wire [2:0]               m_AXI_ARPROT,
    output wire [3:0]               m_AXI_ARQOS,
    output wire [3:0]               m_AXI_ARREGION,
    output wire                     m_AXI_ARVALID,
    input  wire                     m_AXI_ARREADY,
    // ---- R ----
    input  wire [ID_WIDTH-1:0]      m_AXI_RID,
    input  wire [DATA_WIDTH-1:0]    m_AXI_RDATA,
    input  wire [1:0]               m_AXI_RRESP,
    input  wire                     m_AXI_RLAST,
    input  wire                     m_AXI_RVALID,
    output wire                     m_AXI_RREADY
);

// 位宽保护:取 1 时 $clog2 会算出 0,位宽变成 [-1:0] 会直接报错
localparam  BCW = (TEST_BURSTS    > 1) ? $clog2(TEST_BURSTS)    : 1;  // burst 计数位宽
localparam  TCW = (TIMEOUT_CYCLES > 1) ? $clog2(TIMEOUT_CYCLES) : 1;  // 超时计数位宽

//------------------------------------------------------------------------------
// 状态编码
//------------------------------------------------------------------------------
localparam  S_IDLE    = 4'd0,       // 空闲,等 run
            S_CHK     = 4'd1,       // 图案自检(防初始化文件没加载)
            S_WR_CMD  = 4'd2,       // 发写命令,等 Master 收下
            S_WR_DATA = 4'd3,       // 供货写数据,等整笔完成
            S_RD_CMD  = 4'd4,       // 发读命令,等 Master 收下
            S_RD_DATA = 4'd5,       // 收读数据并比对
            S_NEXT    = 4'd6,       // 下一笔 / 阶段切换
            S_FINISH  = 4'd7;       // 结束,停在这

reg  [3:0]              state;
reg  [3:0]              next_state;

//------------------------------------------------------------------------------
// wire / reg 定义区
//------------------------------------------------------------------------------
// 命令接口(内部,接自带的 AXI4_Master)
wire                    cmd_valid;
wire                    cmd_ready;
wire                    cmd_write;
wire [ADDR_WIDTH-1:0]   cmd_addr;
wire [LEN_WIDTH-1:0]    cmd_len;
wire                    cmd_done;
wire                    cmd_err;

// 写数据接口(内部)
wire [DATA_WIDTH-1:0]   wr_data;
wire [STRB_WIDTH-1:0]   wr_strb;
wire                    wr_valid;
wire                    wr_ready;

// 读数据接口(内部)
wire [DATA_WIDTH-1:0]   rd_data;
wire                    rd_valid;
wire                    rd_last;
wire                    rd_ready;

// 握手
wire                    start_now;        // run 脉冲且当前空闲
wire                    wr_hs;          // 写数据握手
wire                    rd_hs;          // 读数据握手

// 计数
wire [8:0]              wr_beat_nxt;    // 写拍计数 +1
wire [8:0]              rd_beat_nxt;    // 读拍计数 +1
wire                    timeout;        // 看门狗超时

// 地址
wire [ADDR_WIDTH-1:0]   cur_burst_addr; // 当前笔起始地址
wire [ADDR_WIDTH-1:0]   cur_beat_addr;  // 当前比对拍的字节地址

// 图案
wire [DATA_WIDTH-1:0]   pat_data;       // BRAM 读出数据
reg  [7:0]              pat_addr;
reg                     pat_en;
wire                    pat_bad;        // 图案首字为 0(说明没加载)

// 判定
wire                    mismatch;       // 读回不一致
wire                    resp_err;       // 读响应非 OKAY

// 数据寄存器
reg  [BCW-1:0]          burst_cnt;      // 当前是第几笔
reg                     phase_r;        // 0 = 写阶段, 1 = 读阶段
reg  [8:0]              wr_beat;        // 本笔已发出的写数据拍数(0..256)
reg  [8:0]              rd_beat;        // 本笔已收到的读数据拍数
reg  [1:0]              chk_cnt;        // 图案自检延时
reg  [TCW-1:0]          to_cnt;         // 看门狗计数

// 自检结果
reg                     fail_r;         // 出过错
reg  [7:0]              errcode_r;
reg  [ADDR_WIDTH-1:0]   err_addr_r;
reg  [31:0]             err_exp_r;
reg  [31:0]             err_act_r;
reg                     done_r;

//------------------------------------------------------------------------------
// 状态输出
//------------------------------------------------------------------------------
assign busy     = (state != S_IDLE) && (state != S_FINISH);
assign done     = done_r;
assign pass     = done_r & ~fail_r;
assign errcode  = errcode_r;
assign err_addr = err_addr_r;
assign err_exp  = err_exp_r;
assign err_act  = err_act_r;

//------------------------------------------------------------------------------
// 图案 BRAM 读端口控制
//      128bit × 256 = 4KB,正好一笔
//      CMD 相预取第 0 拍,DATA 相每次握手后才取下一拍
//      (握手没发生就让输出寄存器保持住 —— 否则数据会跑偏)
//------------------------------------------------------------------------------
always @(*) begin
    pat_addr = 8'd0;
    pat_en   = 1'b0;
    case (state)
        S_CHK, S_WR_CMD, S_RD_CMD : begin
            pat_addr = 8'd0;
            pat_en   = 1'b1;
        end
        S_WR_DATA : begin
            pat_addr = wr_beat[7:0] + 8'd1;
            pat_en   = wr_hs;
        end
        S_RD_DATA : begin
            pat_addr = rd_beat[7:0] + 8'd1;
            pat_en   = rd_hs;
        end
        default : ;
    endcase
end

BlockRAM #(
    .DATA_WIDTH         (DATA_WIDTH),
    .ADDR_WIDTH         (8),
    .USE_BYTE_EN        (0),
    .INIT_FILE_PATH     (PATTERN_INIT)
) u_pattern (
    .bram_clk_wr        (clk),
    .bram_data_in       ({DATA_WIDTH{1'b0}}),
    .bram_en_wr         (1'b0),
    .bram_addr_wr       (8'd0),
    .bram_byte_en_wr    ({(DATA_WIDTH/8){1'b0}}),
    .bram_clk_rd        (clk),
    .bram_data_out      (pat_data),
    .bram_en_rd         (pat_en),
    .bram_addr_rd       (pat_addr)
);

//------------------------------------------------------------------------------
// 组合条件
//------------------------------------------------------------------------------
// 启动:空闲态、或上一轮已跑完(S_FINISH)都可以再启动一次
//   ★ S_FINISH 不能是死状态,否则自检就只能跑一次,必须重新上电
assign start_now        = ((state == S_IDLE) || (state == S_FINISH)) && run;

assign wr_hs            = wr_valid && wr_ready;
assign rd_hs            = rd_valid && rd_ready;
assign timeout          = (to_cnt == TIMEOUT_CYCLES - 1);

assign wr_beat_nxt      = wr_beat + 1'b1;
assign rd_beat_nxt      = rd_beat + 1'b1;

assign cur_burst_addr   = TEST_BASE + {burst_cnt, 12'd0};            // 每笔 4KB
assign cur_beat_addr    = cur_burst_addr + {rd_beat[7:0], 4'd0};     // 每拍 16 字节

// 图案自检:第 2 拍采样,此时 pat_data 已经是 BRAM[0]
assign pat_bad          = (chk_cnt == 2'd1) && (pat_data == {DATA_WIDTH{1'b0}});

// 读回比对:只有收到数据那拍才比
assign mismatch         = (state == S_RD_DATA) && rd_hs && (rd_data != pat_data);

// 读响应错误:整笔结束时一并判
assign resp_err         = (state == S_RD_DATA) && cmd_done && cmd_err;

// 出错收工:★ 必须等本笔读完整(cmd_done)才走
//   中途把 rd_ready 拉低 = Master 停止收 R 数据,而从机/mux 还占着读通道等 RLAST,
//   单 outstanding 下再也回不来 —— 整条 AXI 读通道会被永久卡死(连 CPU 一起遭殃)
//   所以"出错立即停"只能停在**笔边界**,不能停在拍中间
wire rd_stop            = (state == S_RD_DATA) && cmd_done && (fail_r || mismatch);

//------------------------------------------------------------------------------
// 看门狗
//      自检进行中就计数,一有进展(cmd_done)就清零
//      这样两种情况都兜得住:
//        1) DATA 相数据流卡住
//        2) CMD 相等不到 cmd_ready(例如 DDR 校准一直没完成)
//      没有它的话,出问题只能看到 busy 恒为 1,分不清"在跑"还是"死了"
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n)
        to_cnt <= {TCW{1'b0}};
    else begin
        case (state)
            S_IDLE, S_FINISH : to_cnt <= {TCW{1'b0}};
            default : begin
                // 命令完成、或数据在动,都算"有进展" -> 清零
                if (cmd_done || wr_hs || rd_hs || timeout)
                    to_cnt <= {TCW{1'b0}};
                else
                    to_cnt <= to_cnt + 1'b1;
            end
        endcase
    end
end

//------------------------------------------------------------------------------
// 错误记录:first error wins
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        fail_r     <= 1'b0;
        errcode_r  <= 8'd0;
        err_addr_r <= {ADDR_WIDTH{1'b0}};
        err_exp_r  <= 32'd0;
        err_act_r  <= 32'd0;
    end
    else if (start_now) begin                     // 新一轮自检,全部清零
        fail_r     <= 1'b0;
        errcode_r  <= 8'd0;
        err_addr_r <= {ADDR_WIDTH{1'b0}};
        err_exp_r  <= 32'd0;
        err_act_r  <= 32'd0;
    end
    else begin
        if (mismatch) begin                     // 比对不一致:连值一起记下
            fail_r <= 1'b1;
            if (errcode_r == 8'd0) begin
                errcode_r  <= 8'd1;
                err_addr_r <= cur_beat_addr;
                err_exp_r  <= pat_data[31:0];
                err_act_r  <= rd_data[31:0];
            end
        end
        else if (timeout || resp_err || pat_bad) begin
            fail_r <= 1'b1;
            if (errcode_r == 8'd0) begin
                if (timeout)        errcode_r <= 8'd3;
                else if (resp_err)  errcode_r <= 8'd2;
                else                errcode_r <= 8'd4;      // pat_bad:图案没加载
            end
        end
    end
end

//------------------------------------------------------------------------------
// 第一段:状态寄存器
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n)
        state <= S_IDLE;
    else
        state <= next_state;
end

//------------------------------------------------------------------------------
// 第二段:次态组合逻辑
//------------------------------------------------------------------------------
always @(*) begin
    next_state = state;
    case (state)
        S_IDLE    : if (start_now)                                next_state = S_CHK;
        S_CHK     : if (chk_cnt == 2'd1)                        next_state = pat_bad ? S_FINISH : S_WR_CMD;

        S_WR_CMD  : if (cmd_ready)                              next_state = S_WR_DATA;
                    else if (timeout)                           next_state = S_FINISH;

        S_WR_DATA : if (cmd_done)                               next_state = S_NEXT;
                    else if (timeout)                           next_state = S_FINISH;

        S_RD_CMD  : if (cmd_ready)                              next_state = S_RD_DATA;
                    else if (timeout)                           next_state = S_FINISH;

        S_RD_DATA : if (rd_stop)                                next_state = S_FINISH;
                    else if (cmd_done)                          next_state = S_NEXT;
                    else if (timeout)                           next_state = S_FINISH;

        // 一笔跑完:没跑满就下一笔,跑满了就切阶段(写->读)或收工
        S_NEXT    : if (burst_cnt == TEST_BURSTS - 1) begin
                        if (!phase_r)                           next_state = S_RD_CMD;
                        else                                    next_state = S_FINISH;
                    end
                    else                                        next_state = phase_r ? S_RD_CMD : S_WR_CMD;

        S_FINISH  : if (start_now)                              next_state = S_CHK;
                    else                                        next_state = S_FINISH;
        default   :                                             next_state = S_IDLE;
    endcase
end

//------------------------------------------------------------------------------
// 第三段:时序逻辑(数据通路 + 寄存器输出)
//------------------------------------------------------------------------------
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        chk_cnt     <= 2'd0;
        burst_cnt   <= {BCW{1'b0}};
        phase_r     <= 1'b0;
        wr_beat     <= 9'd0;
        rd_beat     <= 9'd0;
        done_r      <= 1'b0;
    end
    // 新一轮自检:所有进度计数器归零(不管从 S_IDLE 还是 S_FINISH 进来)
    else if (start_now) begin
        burst_cnt <= {BCW{1'b0}};
        phase_r   <= 1'b0;
        wr_beat   <= 9'd0;
        rd_beat   <= 9'd0;
        chk_cnt   <= 2'd0;
        done_r    <= 1'b0;
    end
    else begin
        case (state)
            //----------------------------------------------------------
            S_IDLE : ;                            // 空闲:结果寄存器保持不动

            //----------------------------------------------------------
            S_CHK : chk_cnt <= chk_cnt + 1'b1;

            //----------------------------------------------------------
            S_WR_CMD : wr_beat <= 9'd0;             // 本笔写数据从头开始

            //----------------------------------------------------------
            // 供货写数据:每次握手推进一拍,整笔由 Master 的 cmd_done 收尾
            S_WR_DATA : if (wr_hs)
                            wr_beat <= wr_beat_nxt;

            //----------------------------------------------------------
            S_RD_CMD : rd_beat <= 9'd0;             // 本笔读数据从头开始

            //----------------------------------------------------------
            S_RD_DATA : if (rd_hs)
                            rd_beat <= rd_beat_nxt;

            //----------------------------------------------------------
            // 一笔结束:推进计数器 / 切阶段
            S_NEXT : begin
                if (burst_cnt == TEST_BURSTS - 1) begin
                    burst_cnt <= {BCW{1'b0}};
                    if (!phase_r)
                        phase_r <= 1'b1;            // 写阶段结束 -> 转读阶段
                end
                else
                    burst_cnt <= burst_cnt + 1'b1;
            end

            //----------------------------------------------------------
            // 跑完停在这:done/pass/errcode/err_* 保持可读,直到下一次 start
            S_FINISH : done_r <= 1'b1;

            default : ;
        endcase
    end
end

//------------------------------------------------------------------------------
// 给内部 AXI4_Master 的命令 / 数据
//------------------------------------------------------------------------------
// 命令:CMD 相拉高 valid,直到 Master 返回 ready(state 才会变)
assign cmd_valid = (state == S_WR_CMD) || (state == S_RD_CMD);
assign cmd_write = (state == S_WR_CMD);
assign cmd_addr  = cur_burst_addr;
assign cmd_len   = BURST_LEN;

// 写数据:VALID 只由 (state, 计数) 决定,不等 wr_ready(契约要求)
assign wr_data   = pat_data;
assign wr_strb   = {STRB_WIDTH{1'b1}};
assign wr_valid  = (state == S_WR_DATA) && (wr_beat <= BURST_LEN);

// 读数据:本模块永远收得下
assign rd_ready  = (state == S_RD_DATA);

//------------------------------------------------------------------------------
// 通用 AXI4 主设备(自带)
//   cal_done 在 Master 内部并进复位:校准完成前不接受命令
//------------------------------------------------------------------------------
AXI4_Master #(
    .ADDR_WIDTH         (ADDR_WIDTH),
    .DATA_WIDTH         (DATA_WIDTH),
    .STRB_WIDTH         (STRB_WIDTH),
    .ID_WIDTH           (ID_WIDTH),
    .LEN_WIDTH          (LEN_WIDTH),
    .AXI_ID             (AXI_ID)
) u_AXI4_Master (
    .clk                (clk),
    .rst_n              (rst_n),

    .cmd_valid          (cmd_valid),
    .cmd_ready          (cmd_ready),
    .cmd_write          (cmd_write),
    .cmd_addr           (cmd_addr),
    .cmd_len            (cmd_len),
    .cmd_done           (cmd_done),
    .cmd_err            (cmd_err),

    .wr_data            (wr_data),
    .wr_strb            (wr_strb),
    .wr_valid           (wr_valid),
    .wr_ready           (wr_ready),

    .rd_data            (rd_data),
    .rd_valid           (rd_valid),
    .rd_last            (rd_last),
    .rd_ready           (rd_ready),

    .m_AXI_AWID         (m_AXI_AWID),
    .m_AXI_AWADDR       (m_AXI_AWADDR),
    .m_AXI_AWLEN        (m_AXI_AWLEN),
    .m_AXI_AWSIZE       (m_AXI_AWSIZE),
    .m_AXI_AWBURST      (m_AXI_AWBURST),
    .m_AXI_AWLOCK       (m_AXI_AWLOCK),
    .m_AXI_AWCACHE      (m_AXI_AWCACHE),
    .m_AXI_AWPROT       (m_AXI_AWPROT),
    .m_AXI_AWQOS        (m_AXI_AWQOS),
    .m_AXI_AWREGION     (m_AXI_AWREGION),
    .m_AXI_AWVALID      (m_AXI_AWVALID),
    .m_AXI_AWREADY      (m_AXI_AWREADY),

    .m_AXI_WDATA        (m_AXI_WDATA),
    .m_AXI_WSTRB        (m_AXI_WSTRB),
    .m_AXI_WLAST        (m_AXI_WLAST),
    .m_AXI_WVALID       (m_AXI_WVALID),
    .m_AXI_WREADY       (m_AXI_WREADY),

    .m_AXI_BID          (m_AXI_BID),
    .m_AXI_BRESP        (m_AXI_BRESP),
    .m_AXI_BVALID       (m_AXI_BVALID),
    .m_AXI_BREADY       (m_AXI_BREADY),

    .m_AXI_ARID         (m_AXI_ARID),
    .m_AXI_ARADDR       (m_AXI_ARADDR),
    .m_AXI_ARLEN        (m_AXI_ARLEN),
    .m_AXI_ARSIZE       (m_AXI_ARSIZE),
    .m_AXI_ARBURST      (m_AXI_ARBURST),
    .m_AXI_ARLOCK       (m_AXI_ARLOCK),
    .m_AXI_ARCACHE      (m_AXI_ARCACHE),
    .m_AXI_ARPROT       (m_AXI_ARPROT),
    .m_AXI_ARQOS        (m_AXI_ARQOS),
    .m_AXI_ARREGION     (m_AXI_ARREGION),
    .m_AXI_ARVALID      (m_AXI_ARVALID),
    .m_AXI_ARREADY      (m_AXI_ARREADY),

    .m_AXI_RID          (m_AXI_RID),
    .m_AXI_RDATA        (m_AXI_RDATA),
    .m_AXI_RRESP        (m_AXI_RRESP),
    .m_AXI_RLAST        (m_AXI_RLAST),
    .m_AXI_RVALID       (m_AXI_RVALID),
    .m_AXI_RREADY       (m_AXI_RREADY)
);

endmodule
