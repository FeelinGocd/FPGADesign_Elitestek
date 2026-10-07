//==============================================================================
//  key.v  ——  按键核心逻辑(不含总线)
//------------------------------------------------------------------------------
//  功能:
//    1) 同步,消除按键输入的亚稳态
//    2) 计数式消抖:连续 DEBOUNCE_CNT 拍稳定
//    3) 按下沿检测:状态由 0 变 1 时产生一个单周期脉冲
//
//  约定:板级按键按下为低电平(外部上拉),本模块内部统一转成"按下 = 1"
//  时钟:user_clk(100 MHz)
//==============================================================================

module key #(
    parameter KEY_NUM      = 4,             // 按键个数
    parameter DEBOUNCE_CNT = 1_000_000      // 消抖阈值,100MHz 下约 10 ms
)(
    input  wire                 clk,
    input  wire                 rst_n,

    input  wire [KEY_NUM-1:0]   key_pin,    // 原始引脚,按下 = 0
    output reg  [KEY_NUM-1:0]   key_state,  // 消抖后的状态,按下 = 1
    output reg  [KEY_NUM-1:0]   key_edge    // 按下瞬间的单周期脉冲
);

//------------------------------------------------------------------------------
// 同步:异步按键输入进到 user_clk 域
//------------------------------------------------------------------------------
reg [KEY_NUM-1:0] sync0;
reg [KEY_NUM-1:0] sync1;

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        sync0 <= {KEY_NUM{1'b1}};
        sync1 <= {KEY_NUM{1'b1}};
    end
    else begin
        sync0 <= key_pin;
        sync1 <= sync0;
    end
end

// 取反:统一成"按下 = 1"
wire [KEY_NUM-1:0] key_n = ~sync1;

//------------------------------------------------------------------------------
// 消抖:每个按键一个独立计数器
//    原始值和当前状态不同 -> 计数;计满 -> 更新状态
//    原始值和当前状态相同 -> 计数清零
//------------------------------------------------------------------------------
reg [19:0] db_cnt [0:KEY_NUM-1];

genvar i;
generate
for (i = 0; i < KEY_NUM; i = i + 1) begin : GEN_DEBOUNCE

    wire [19:0] cnt_cur = db_cnt[i];
    wire        diff    = (key_n[i] != key_state[i]);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            db_cnt[i]    <= 32'd0;
            key_state[i] <= 1'b0;
        end
        else if (diff) begin
            if (cnt_cur >= DEBOUNCE_CNT) begin
                db_cnt[i]    <= 32'd0;
                key_state[i] <= key_n[i];
            end
            else begin
                db_cnt[i]    <= cnt_cur + 32'd1;
            end
        end
        else
            db_cnt[i]    <= 32'd0;
    end
end
endgenerate

//------------------------------------------------------------------------------
// 按下沿检测
//------------------------------------------------------------------------------
reg [KEY_NUM-1:0] key_state_d;

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        key_state_d <= {KEY_NUM{1'b0}};
        key_edge    <= {KEY_NUM{1'b0}};
    end
    else begin
        key_state_d <= key_state;
        key_edge    <= key_state & (~key_state_d);     // 0 -> 1 的瞬间
    end
end

endmodule
