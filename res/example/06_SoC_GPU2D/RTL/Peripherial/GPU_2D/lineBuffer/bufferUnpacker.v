//==============================================================================
//  bufferUnpacker.v —— 128bit(16 字节) → 24bit(RGB888) 字节展开
//------------------------------------------------------------------------------
//  干什么
//      PingPongBuffer 读口吐出来的是"一拍 = IN_WIDTH/8 字节"的原始字节流
//      （从 DDR 按 AXI 位宽读回来、没做任何重排），本模块把它按 24bit 一像素切开，
//      逐拍送给 HDMI 编码器。
//
//  为什么需要它
//      IN_WIDTH/8 字节不是像素字节数的整数倍（16 字节 = 5⅓ 个像素），
//      一拍里的像素是错位的，必须跨拍拼接。本模块把这个判断做成"余量够不够一个像素"，
//      弹出节拍自然成为 5,5,6… 循环（3 拍 = 48 字节 = 16 像素 = 一个循环）。
//
//  接口契约（两条时序约定，必须遵守）
//      1) 像素侧：pixel_req 拉高后的【下一拍】，pixel_data 必须是下一个像素
//         （HDMI_Top 的 HDMI_data_req_o 就是这种"早一拍"的请求形式）
//      2) 取数侧：beat_req 必须【提前一拍】拉高 —— 源 FIFO 的读出有一拍延迟
//
//  相位
//      每行从相位 0 开始、行间不漂移（行长字节数与行像素数都能被循环长度整除），
//      不需要任何对齐输入：每个像素固定吃 3 字节、进出总量相等，指针自然落在行边界上。
//==============================================================================
module bufferUnpacker #(
    parameter DIN_WIDTH      = 128,                  // 输入一拍位宽（必须是 8 的整数倍）
    parameter DOUT_WIDTH     = 24                    // 输出像素位宽（RGB888 = 24）
)(
    input  wire                     clk,            // 像素时钟（clk_HDMI_x1）
    input  wire                     rst_n,          // ★本域同步释放后的复位（见上面 asyn_rst_syn）

    //------------------------------------------------------------
    // 取数侧（接 async_PingPongBuffer 读口）
    //------------------------------------------------------------
    output wire                     unpacker_rd_req,        // 申请弹一拍      → buffer_rd_en
    input  wire [DIN_WIDTH-1:0]     unpacker_rd_data,       // 弹出的一拍数据  → buffer_rd_data
    input  wire                     unpacker_rd_valid,      // 申请的那拍到了  → buffer_rd_valid
    input  wire                     unpacker_rd_block_ready,// 当前 bank 可读 → buffer_rd_block_ready（接进申请条件）

    //------------------------------------------------------------
    // 像素侧（接 HDMI_Top）
    //------------------------------------------------------------
    input  wire                     pixel_req,      // 下一拍要一个像素 → HDMI_data_req_o
    output wire [DOUT_WIDTH-1:0]    pixel_data      // 像素数据         → HDMI_data_i
);

//==============================================================================
// 参数换算
//==============================================================================
localparam  DIN_BYTES       = DIN_WIDTH/8;              // 申请数据的字节长度
localparam  PIXEL_BYTES     = DOUT_WIDTH/8;             // 输出像素的字节长度
localparam  BUF_BYTES       = 24;                       // 字节缓存大小 = 申请阈值 8 + 一拍 16
localparam  BUF_WIDTH       = BUF_BYTES*8;              // 缓存器总长
localparam  BUF_CNT_WIDTH   = 6;                        // 缓存器字节数
localparam  THRESH          = 8;                        // 申请下一段数据阈值

//==============================================================================
// 字节缓存 + 状态
//==============================================================================
reg  [BUF_WIDTH-1:0]        byte_buffer;            // 兜：还没吐出去的字节，低位对齐（低位 = 最老的字节）
reg  [BUF_CNT_WIDTH-1:0]    cnt_byte;               // 兜里有效字节数
reg  [BUF_WIDTH-1:0]        next_byte_buffer;       // 新到的一拍拼进兜之后的样子（兜的次态）
reg                         rd_inflight;            // 已申请一拍、数据还没回来
reg                         pixel_req_d;            // pixel_req 延后一拍 = 真正在吐像素的那一拍

wire                        pixel_valid;        // 本拍真的吐出一个像素（消费拍 + 兜里够 3 字节）

//==============================================================================
// 组合条件
//==============================================================================
// 输出数据条件：上一拍的延时拍 && 缓存器里够一个像素
//   ★ 右移必须用这个前提，不能只看 pixel_req —— 请求数据那一拍如果也右移，会读到无效的 3 字节
assign  pixel_valid     = pixel_req_d && (cnt_byte >= PIXEL_BYTES);

// 申请数据条件：缓存器内部字节数≤阈值 && 没有正在发送的请求 && 当前 bank 有效
//   ★ 阈值 8 不是随便取的：
//     读取每次读 3 字节 申请后要等一拍数据才回来
//     申请的字节要再等一拍才进缓存器 而"第一个 <= 阈值的值"落点是 阈值/阈值-1/阈值-2，
//     要保证回来那一拍缓冲器里还剩 >= 3 字节 → 阈值 - 2 - 3 >= 3 → 阈值 >= 8
assign  unpacker_rd_req = (cnt_byte <= THRESH) && !rd_inflight && unpacker_rd_block_ready;

// 像素输出：兜里最低 3 字节，纯寄存器直连（零组合逻辑）
assign  pixel_data      = byte_buffer[DOUT_WIDTH-1:0];

//==============================================================================
// 拼接：新到的一拍 16 字节接到兜里已有字节的后面（纯拼接，没有移位器）
//   阈值 8 + 一次只放一拍在途 → 拼接时 cnt_byte 一定 <= 8，9 路就够
//==============================================================================
always @(*) begin
    case (cnt_byte[3:0])
        4'd0:   next_byte_buffer = { 64'b0, unpacker_rd_data                  };
        4'd1:   next_byte_buffer = { 56'b0, unpacker_rd_data, byte_buffer[  7:0] };
        4'd2:   next_byte_buffer = { 48'b0, unpacker_rd_data, byte_buffer[ 15:0] };
        4'd3:   next_byte_buffer = { 40'b0, unpacker_rd_data, byte_buffer[ 23:0] };
        4'd4:   next_byte_buffer = { 32'b0, unpacker_rd_data, byte_buffer[ 31:0] };
        4'd5:   next_byte_buffer = { 24'b0, unpacker_rd_data, byte_buffer[ 39:0] };
        4'd6:   next_byte_buffer = { 16'b0, unpacker_rd_data, byte_buffer[ 47:0] };
        4'd7:   next_byte_buffer = {  8'b0, unpacker_rd_data, byte_buffer[ 55:0] };
        default:next_byte_buffer = {        unpacker_rd_data, byte_buffer[ 63:0] };     // cnt_byte = 8
    endcase
end

//==============================================================================
// 
//==============================================================================
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        byte_buffer     <= {BUF_WIDTH{1'b0}};
        cnt_byte        <= {BUF_CNT_WIDTH{1'b0}};
        rd_inflight     <= 1'b0;
        pixel_req_d     <= 1'b0;
    end
    else begin
        if (unpacker_rd_valid) begin
            if (pixel_valid) begin
                byte_buffer <= next_byte_buffer >> (PIXEL_BYTES*8);         // 拼完顺手吐一个像素
                cnt_byte    <= cnt_byte + (DIN_BYTES - PIXEL_BYTES);        // +16 -3
            end
            else begin
                byte_buffer <= next_byte_buffer;                            // 只是收下，没吐
                cnt_byte    <= cnt_byte + DIN_BYTES;
            end
        end
        else if (pixel_valid) begin
            byte_buffer     <= byte_buffer >> (PIXEL_BYTES*8);              // 没新数据，只吐
            cnt_byte        <= cnt_byte - PIXEL_BYTES;
        end

        // ---- 在途标记：申请置位、数据回来清零（一次只放一拍在外面）----
        if (unpacker_rd_req)    rd_inflight <= 1'b1;
        if (unpacker_rd_valid)  rd_inflight <= 1'b0;

        // ---- 消费拍延后一拍，用来对齐"早一拍请求、下一拍要数"的像素契约 ----
        pixel_req_d <= pixel_req;
    end
end

endmodule
