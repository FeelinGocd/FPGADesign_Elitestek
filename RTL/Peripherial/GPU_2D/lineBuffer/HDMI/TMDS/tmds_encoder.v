//==============================================================================
//  tmds_encoder.v —— 单通道 DVI/TMDS 8b→10b 编码
//------------------------------------------------------------------------------
//  功能
//      按 DVI 规范把一个字节编成 10bit：
//        de = 1（可见区）：走 8b→10b 数据编码（按 1/0 个数做差分平衡，维持 DC 平衡）
//        de = 0（消隐区）：输出控制码（c0/c1 = hs/vs）
//==============================================================================
`timescale 1ps/1ps

module tmds_encoder(
    input               clkin,      //像素时钟 pclk 输入
    input               rstin,      //异步复位信号 高有效
    input       [7:0]   din,        //像素数据输入
    input               c0,         //c0 input (hsync)
    input               c1,         //c1 input (vsync)
    input               de,         //de input (de signal)
    
    output  reg [9:0]   dout        //data output
    );
    
////////////////////////////////////////////////////////////
// 计算每个输入像素的1和0的个数
// 计算结果
// 处理输入数据使得其满足总线输出要求
// 最终输出
////////////////////////////////////////////////////////////
reg [3:0]   n1d;      //输入数据中 1 的个数
reg [7:0]   din_q;

//计算像素数据中'1'的个数
always@(posedge clkin) begin
    n1d <= din[0] +din[1] + din[2] + din[3] + din[4] + din[5] + din[6] + din[7];
    din_q <= din;
end

///////////////////////////////////////////////////////
// Stage 1: 8 bit -> 9 bit
// 参考DVI 1.0规范，第29页，图3-5
///////////////////////////////////////////////////////
wire decision1;

assign decision1 = (n1d > 4'h4) | ((n1d == 4'h4) & (din_q[0] == 1'b0));

wire [8:0] q_m;
assign q_m[0] = din_q[0];
assign q_m[1] = (decision1) ? (q_m[0] ^~ din_q[1]) : (q_m[0] ^ din_q[1]);
assign q_m[2] = (decision1) ? (q_m[1] ^~ din_q[2]) : (q_m[1] ^ din_q[2]);
assign q_m[3] = (decision1) ? (q_m[2] ^~ din_q[3]) : (q_m[2] ^ din_q[3]);
assign q_m[4] = (decision1) ? (q_m[3] ^~ din_q[4]) : (q_m[3] ^ din_q[4]);
assign q_m[5] = (decision1) ? (q_m[4] ^~ din_q[5]) : (q_m[4] ^ din_q[5]);
assign q_m[6] = (decision1) ? (q_m[5] ^~ din_q[6]) : (q_m[5] ^ din_q[6]);
assign q_m[7] = (decision1) ? (q_m[6] ^~ din_q[7]) : (q_m[6] ^ din_q[7]);
assign q_m[8] = (decision1) ? 1'b0 : 1'b1;

/////////////////////////////////////////////////////////
// Stage 2: 9 bit -> 10 bit
// 参考DVI 1.0规范，第29页，图3-5
/////////////////////////////////////////////////////////

reg [3:0] n1q_m, n0q_m;             //q_m中1和0的个数
always@(posedge clkin) begin
    n1q_m <= q_m[0] + q_m[1] + q_m[2] + q_m[3] + q_m[4] + q_m[5] + q_m[6] + q_m[7];
    n0q_m <= 4'h8 - (q_m[0] + q_m[1] + q_m[2] + q_m[3] + q_m[4] + q_m[5] + q_m[6] + q_m[7]);
end

parameter CTRLTOKEN0 = 10'b1101010100;
parameter CTRLTOKEN1 = 10'b0010101011;
parameter CTRLTOKEN2 = 10'b0101010100;
parameter CTRLTOKEN3 = 10'b1010101011;

reg [4:0]   cnt;                    //disparity counter MSB is the sign bit
wire        decision2, decision3;

assign decision2 = (cnt == 5'h0) | (n1q_m == n0q_m);
/////////////////////////////////////////////////////////////////////////
// [(cnt > 0) and (N1q_m > N0q_m)] or [(cnt < 0) and (N0q_m > N1q_m)]
/////////////////////////////////////////////////////////////////////////
assign decision3 = (~cnt[4] & (n1q_m > n0q_m)) | (cnt[4] & (n0q_m > n1q_m));

////////////////////////////////////
// 数据对齐
////////////////////////////////////
reg         de_q, de_reg;
reg         c0_q, c1_q;
reg         c0_reg, c1_reg;
reg [8:0]   q_m_reg;

always@(posedge clkin) begin
    de_q    <= de;
    de_reg  <= de_q;
    
    c0_q    <= c0;
    c0_reg  <= c0_q;
    c1_q    <= c1;
    c1_reg  <= c1_q;
    
    q_m_reg <= q_m;
end

///////////////////////////////
// 10-bit out
// 差距计数器
///////////////////////////////
always@(posedge clkin or posedge rstin) begin
    if(rstin) begin
        dout <= 10'h0;
        cnt <= 5'h0;
    end
    else begin
        if(de_reg) begin
            if(decision2) begin
                dout[9]     <= ~q_m_reg[8];
                dout[8]     <= q_m_reg[8];
                dout[7:0]   <= (q_m_reg[8]) ? q_m_reg[7:0] : ~q_m_reg[7:0];
                
                cnt <= (~q_m_reg[8]) ? (cnt + n0q_m - n1q_m) : (cnt + n1q_m - n0q_m);
            end
            else begin
                if(decision3) begin
                    dout[9]     <= 1'b1;
                    dout[8]     <= q_m_reg[8];
                    dout[7:0]   <= ~q_m_reg[7:0];
                    
                    cnt <= cnt + {q_m_reg[8], 1'b0} + (n0q_m - n1q_m);
                end
                else begin
                    dout[9]     <= 1'b0;
                    dout[8]     <= q_m_reg[8];
                    dout[7:0]   <= q_m_reg[7:0];
                    
                    cnt <= cnt - {~q_m_reg[8], 1'b0} + (n1q_m - n0q_m);
                end
            end
        end
        else begin
            case ({c1_reg, c0_reg})
                2'b00 : dout <= CTRLTOKEN0;
                2'b01 : dout <= CTRLTOKEN1;
                2'b10 : dout <= CTRLTOKEN2;
                default:dout <= CTRLTOKEN3;
            endcase
            cnt <= 5'h0;
        end
    end
end

endmodule