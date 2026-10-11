//==============================================================================
//  PaletteRAM.v —— 素材/调色板用的片上 ROM（块 RAM 实现，读口带握手）
//------------------------------------------------------------------------------
//  干什么
//      把 BlockRAM 包一层，只留"带 valid/ready 的读口"。
//      素材索引（assets.hex）和调色板（palette.hex）都是【生成 bit 阶段】烧进去的，
//      运行期只读不写 —— 写口在这里一次性接常量，上层不用再操心"BRAM 写口要接满"。
//
//  读口为什么是 valid/ready，不是"读出来晚一拍"了事
//      同步块 RAM 的数据要下一拍才出来。只给一根 read_data 的话，"晚一拍"这件事就
//      只剩注释里的一句话 —— 每个使用者都得自己记一笔，谁忘了谁错，而且只在停顿/
//      行首/行尾这些边角处暴露（本工程在这上面栽过：见 PaletteDecoder.v 文件头）。
//
//      所以把"延迟"和"保持"一起封进接口：
//          read_request / read_accept   我要读一个字 / 这一拍能被受理
//          read_data / read_valid       数据（读后 1 拍出）/ 有效（★ 保持到 read_ready）
//          read_ready                   消费者取走
//      ⇒ "数据和它的有效位同拍"是【封装出来的】，不是靠使用者自觉。
//
//      为什么能保持：bram_en_rd = 0 时 BlockRAM 的输出寄存器不动（见 BlockRAM.v），
//      所以只要不发起新的读，数据就一直摆在那儿 —— 一个标志就够，不需要额外寄存器。
//
//  clear
//      行边界把这个标志清掉，免得上一行残留的字被当成新行的第一个字喂下去。
//
//  为什么不需要 ping-pong
//      同步块 RAM 的读延迟是【固定一拍】，没有突发、没有页边界、不会变长，
//      所以连一级 skid 都用不上。（DDRSource 那边要 ping-pong，是因为 DDR 的读延迟
//      和突发长度都不确定。）
//
//  参考：LineBuffer/PingPongBuffer/BlockRAM.v、docs/spec/硬件_加速器.md §4.1//  上一级是谁
//      pixel/palette/PaletteDecoder.v

//==============================================================================
`timescale 1ns/100ps

module PaletteRAM #(
    parameter DATA_WIDTH    = 32,                   // 一个字多少位
    parameter ADDRESS_WIDTH = 15,                   // 深度 2^ADDRESS_WIDTH 字（15 = 32768 字 = 128KB）
    parameter INIT_FILE     = "assets.hex"          // 初值文件（空串 = 不初始化）
)(
    //==================== 时钟 / 复位 ====================
    input  wire                         clk,
    input  wire                         rst_n,
    input  wire                         clear,          //(I) 单拍：丢掉手上这个字（行边界用）

    //==================== 读请求口 ====================
    input  wire                         read_request,   //(I) 我要读一个字（只在 read_accept=1 那拍受理）
    output wire                         read_accept,    //(O) 这一拍发请求能被受理
    input  wire [ADDRESS_WIDTH-1:0]     read_address,   //(I) 读地址

    //==================== 数据口 ====================
    output wire [DATA_WIDTH-1:0]        read_data,      //(O) 读后 1 拍出，保持到 read_ready
    output wire                         read_valid,     //(O) read_data 有效
    input  wire                         read_ready      //(I) 消费者取走
);

//==============================================================================
// wire / reg
//==============================================================================
//   data_valid_r = 手上有一个读出来的字，还没被取走
reg             data_valid_r;
wire            read_handshake;                 // 这一拍真的去动 BlockRAM 的读使能

//==============================================================================
// 判断组合条件
//==============================================================================
//   手上没货、或这拍正好被取走 ⇒ 能受理新请求
assign  read_accept    = ~data_valid_r | read_ready;
//   受理不了的时候 read_request 被忽略（RAM 没法把读请求排队）
assign  read_handshake = read_request & read_accept;

//==============================================================================
// 组合输出
//==============================================================================
assign  read_valid = data_valid_r;

//==============================================================================
// 时序逻辑
//==============================================================================
always @(posedge clk or negedge rst_n) begin
    if (!rst_n)
        data_valid_r <= 1'b0;
    else if (clear)
        data_valid_r <= 1'b0;                   // 行边界
    else if (read_handshake)
        data_valid_r <= 1'b1;                   // 发起了一次读 ⇒ 下一拍有货
    else if (data_valid_r & read_ready)
        data_valid_r <= 1'b0;                   // 被取走且没有再读 ⇒ 空了
end

//==============================================================================
// 例化
//==============================================================================
//------------------------------------------------------------------------------
// 底层块 RAM（★ 写口全部接常量：这里是把 BlockRAM 当 ROM 用）
//------------------------------------------------------------------------------
BlockRAM #(
    .DATA_WIDTH      (DATA_WIDTH     ),
    .ADDR_WIDTH      (ADDRESS_WIDTH  ),
    .USE_BYTE_EN     (0              ),
    .INIT_FILE_PATH  (INIT_FILE      )
) u_BlockRAM (
    .bram_clk_wr      (clk                          ),
    .bram_data_in     ({DATA_WIDTH{1'b0}}           ),
    .bram_en_wr       (1'b0                         ),
    .bram_addr_wr     ({ADDRESS_WIDTH{1'b0}}        ),
    .bram_byte_en_wr  ({(DATA_WIDTH/8){1'b0}}       ),

    .bram_clk_rd      (clk                          ),
    .bram_data_out    (read_data                    ),
    .bram_en_rd       (read_handshake               ),
    .bram_addr_rd     (read_address                 )
);

endmodule
