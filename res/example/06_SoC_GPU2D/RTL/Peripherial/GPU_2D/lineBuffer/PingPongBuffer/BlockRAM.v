/* 2026/7/29 by FeelinGood
BRAM：参数化 Block RAM 模块，支持单/双时钟访问、可选 Byte Enable 写入及初始化文件加载
 - 参数说明：
   - DATA_WIDTH      ：BRAM 数据位宽，支持任意 Byte 对齐宽度（16/32/64...）
   - ADDR_WIDTH      ：BRAM 地址位宽，存储深度为 2^ADDR_WIDTH
   - USE_BYTE_EN     ：Byte Enable 功能开关
                         0：整字写入（忽略 bram_byte_en_wr）
                         1：按 Byte 写入，每一位使能对应一个 Byte
   - INIT_FILE_PATH  ：初始化文件路径，若为空字符串则不初始化
 - 内部实现：
   - 使用寄存器阵列 BUFFER 推断 FPGA Block RAM（ram_style="block"）
   - 写端口和读端口相互独立，可工作在相同或不同的时钟域
   - 当 USE_BYTE_EN = 1 时，仅更新 Byte Enable 对应的数据字节
   - 当 USE_BYTE_EN = 0 时，每次写操作直接覆盖整个数据字
   - 若 INIT_FILE_PATH 不为空，则使用 $readmemh 加载初始化数据
 - 延迟特性：
   - 写操作在 bram_clk_wr 上升沿同步完成
   - 读操作在 bram_clk_rd 上升沿同步输出，读延迟为一个读时钟周期
   - 当读写地址相同且同时访问时，最终读出结果由 FPGA Block RAM 的读写冲突策略决定
 - 使用注意：
   - DATA_WIDTH 必须为 8 的整数倍，否则无法进行 Byte 划分
   - ADDR_WIDTH 决定 BRAM 深度，实际容量为 DATA_WIDTH × 2^ADDR_WIDTH bit
   - 当 USE_BYTE_EN = 1 时：
       DATA_WIDTH = 16 → bram_byte_en_wr[1:0]
       DATA_WIDTH = 32 → bram_byte_en_wr[3:0]
       DATA_WIDTH = 64 → bram_byte_en_wr[7:0]
     每一位控制对应 Byte 是否写入
   - 当 USE_BYTE_EN = 0 时，bram_byte_en_wr 输入无效，可悬空或接任意值
   - INIT_FILE_PATH 可填写相对路径或绝对路径，综合工具需支持对应初始化文件导入
*/
module BlockRAM #(
    parameter DATA_WIDTH    = 16,               // write width 数据位宽
    parameter ADDR_WIDTH    = 10,               // write depth 地址位宽 (ADDR_WIDTH only can be integer because in 2^n, n = 1,2,3...)
    parameter USE_BYTE_EN   = 0,                // 默认不使用 Byte Enable 写
    parameter INIT_FILE_PATH= ""                // 初始化文件地址 相对地址或绝对地址应该都行
)(
    //write domain
	input                           bram_clk_wr,
	input       [DATA_WIDTH-1:0]    bram_data_in,
	input                           bram_en_wr,
    input       [ADDR_WIDTH-1:0]    bram_addr_wr,
    input       [DATA_WIDTH/8-1:0]  bram_byte_en_wr,    // 每一位控制一个数据的 Byte
    //read domain
    input                           bram_clk_rd,
    output reg  [DATA_WIDTH-1:0]    bram_data_out,
    input                           bram_en_rd,
    input       [ADDR_WIDTH-1:0]    bram_addr_rd
);

//reg [DATA_WIDTH-1:0]  BUFFER [(1<<PTR)-1:0];  //ctrl BUFFER
(* ram_style="block" *)
reg [DATA_WIDTH-1:0]  BUFFER [0:(1<<ADDR_WIDTH)-1];   // ↓ 维度改成 [0:N-1]

initial begin
    if(INIT_FILE_PATH != "")
        $readmemh(INIT_FILE_PATH, BUFFER);
end

////////////////////////////////////////////////////////////
// control
////////////////////////////////////////////////////////////
integer i;
generate
if(USE_BYTE_EN) begin
    always @(posedge bram_clk_wr) begin
        if(bram_en_wr) begin
            for(i = 0; i < DATA_WIDTH / 8; i = i + 1) begin
                if(bram_byte_en_wr[i])
                    BUFFER[bram_addr_wr][i*8 +: 8] <= bram_data_in[i*8 +: 8];
            end
        end
    end
end
else begin
    always @(posedge bram_clk_wr)
        if(bram_en_wr)
            BUFFER[bram_addr_wr] <= bram_data_in;
end
endgenerate

always@(posedge bram_clk_rd) begin
    if(bram_en_rd) begin
        bram_data_out <= BUFFER[bram_addr_rd];
    end
end

endmodule