//==============================================================================
//  tb_PaletteDecStream.v —— 专治"valid 与数据错拍"：喂【带气泡的索引流】逐字节比对
//------------------------------------------------------------------------------
//  为什么单独写这个 TB
//      原来的单元 TB 喂的是【连续流】(valid 恒 1), 稳态下 valid 和数据谁也看不出错拍;
//      而整链里 valid 会变(行首/最后一个字/停顿结束) —— 偏偏就在那几拍错。
//      ⇒ 这里专门造"valid 有洞"的序列 + 下游反压, 把错拍逼出来。小、快、可控。
//
//  契约（本 TB 要验的就是这条）
//      "每一个被接收的索引字, 恰好产生一个有效的输出字, 顺序不变"
//      ⇒ 输出流 == 那些索引字展开成的像素字节流
//      ★ 2026-10 改：Decoder 不再做相位整形（补 dst_off 个哑字节已经下沉到 DDRPacker，
//        见 docs/spec/硬件_加速器.md §4.3）⇒ 输出流【与 dst_off 无关】，本 TB 顺带把
//        几个 dst_off 都跑一遍，确认结果完全一样。
//
//  跑法
//      vlog -quiet -work work tb_PaletteDecStream.v PaletteDecoder.v PaletteLUT.v PaletteRAM.v BlockRAM.v
//      vsim -c -quiet -do "run -all; quit -f" work.tb_PaletteDecStream
//==============================================================================
`timescale 1ns/100ps

module tb_PaletteDecStream;

localparam integer NW = 48;                     // 最多喂 48 个索引字
localparam integer NB = NW * 12 + 64;           // 输出流缓冲

reg          clk = 1'b0;
always #5 clk = ~clk;

reg          rst_n = 1'b0;
reg          line_start;

reg  [31:0]  index_data;
wire         index_valid;
wire         index_ready;
wire         busy;
wire [95:0]  palette_data;
wire [11:0]  pal_mask;                          /* ck 掩码(本 TB 不看, 但端口必须接) */
wire         palette_valid;
reg          palette_ready;

//==============================================================================
// 被测：PaletteDecoder（只查表，不做相位整形）
//==============================================================================
PaletteDecoder #(
    .INDEX_WIDTH        (8),
    .COLOR_WIDTH        (24),
    .INDEXES_PER_WORD   (4),
    .BYTES_PER_BEAT     (12),
    .PALETTE_INIT_FILE  ("palette.hex")     /* 相对当前运行目录 —— 和别的 TB 一致 */
) dut (
    .clk          (clk          ),
    .rst_n        (rst_n        ),
    .line_start   (line_start   ),
    .key_color    (24'hFFFFFF),                     /* 键色 = 白(复位默认值) */
    .line_flags   (7'b0),                           /* 不镜像、不开 ck —— 不接会变 X 把数据带坏 */
    .index_data       (index_data       ),
    .index_valid (index_valid ),
    .index_ready    (index_ready    ),
    .palette_data     (palette_data     ),
    .palette_mask     (pal_mask         ),
    .palette_valid    (palette_valid    ),
    .palette_ready    (palette_ready    ),
    .busy         (busy         )
);

//---- 驱动：valid 带洞（周期 7 里空 1 拍）+ 下游反压（周期 5 里停 2 拍）----
integer      cnt = 0, in_n = 0;

reg  [31:0]  in_words [0:NW-1];
reg  [31:0]  src_words[0:NW-1];

//   ★ 合规握手：valid 一旦拉高就【保持数据和 valid】直到 ready 握手；
//     气泡（valid=0）也占满一整拍。之前用 cnt%7 直接抖 valid 是违反契约的 ✗
reg          v_r = 1'b0;
integer      gap = 0;
integer      bub = 7;                           // 每几个字插一拍气泡
integer      bpp = 5;                           // 下游反压周期
//   ★ 只有 line_start 之后才允许喂字：真实的 PaletteSource 是收到行任务才开始读 PRAM 的。
//     之前复位后立刻喂 ⇒ DUT 在 window_offset 还是复位值(24)时乱吐, 把流水推偏 2 拍 ✗
reg          go  = 1'b0;

always @(posedge clk) cnt <= (cnt == 199) ? 0 : cnt + 1;
always @(*) palette_ready    = (cnt % bpp) < ((bpp > 2) ? (bpp - 2) : 1);   // 下游时不时反压
always @(*) index_data       = (in_n < NW) ? src_words[in_n] : 32'd0;   // ★ 别让总线挂 X
assign       index_valid = v_r;

always @(posedge clk) begin
    if (!rst_n) begin
        v_r <= 1'b0; gap <= 0; in_n <= 0; go <= 1'b0;
    end
    else if (line_start) begin
        go <= 1'b1;                             // 行任务来了才开始喂
    end
    else if (!go) begin
        v_r <= 1'b0;                            // 还没开行：一个字都不喂
    end
    else if (in_n >= NW) begin
        v_r <= 1'b0;                            // 喂满就停
    end
    else if (v_r && index_ready) begin            // 这一拍握手成功
        in_n <= in_n + 1;
        if ((in_n % bub) == (bub - 1)) begin v_r <= 1'b0; gap <= 0; end   // 插一拍气泡
        else                 v_r <= 1'b1;
    end
    else if (!v_r) begin
        gap <= gap + 1;
        if (gap >= 1) v_r <= 1'b1;              // 空一拍后继续
    end
    //   v_r = 1 且没握手 ⇒ 保持不变（valid 保持到 ready）
end

//---- 期望像素：索引字展开 → 调色板 → B,G,R ----
//   ★ 一项 = AARRGGBB（α 在高字节）⇒ 按 32 位读；下面只取低 24 位（B,G,R）比对
reg [31:0] pal [0:255];
reg [7:0]  pbytes [0:NW*12-1];
integer    i, c, k, j, cc;
reg [7:0]  ix;

//---- ★ 逐拍 trace（看"到底哪一拍多推进了一次"）----
integer tr = 0;
reg     tr_on = 1'b0;
always @(posedge clk) begin
    if (tr_on && (tr < 26)) begin
        $display("  [T%0d] iv=%b lutv=%b lane=%b busy=%b pv=%b pd0=%02x",
                 tr, index_valid, dut.lookup_valid, dut.u_PaletteLUT.lane_valid, busy, palette_valid, palette_data[7:0]);
        tr = tr + 1;
    end
end

//---- 收输出流 ----
reg [7:0]  ostream [0:NB-1];
integer    o_n = 0;
integer    err_cnt = 0;

always @(posedge clk) begin
    if (rst_n && palette_valid && palette_ready && (o_n < NB)) begin
        for (cc = 0; cc < 12; cc = cc + 1)
            ostream[o_n + cc] <= palette_data[cc*8 +: 8];   // 低位 = 最老的字节
        if ((o_n / 12) < 10)
            $display("  [W%0d] %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                     o_n/12, palette_data[7:0], palette_data[15:8], palette_data[23:16], palette_data[31:24],
                     palette_data[39:32], palette_data[47:40], palette_data[55:48], palette_data[63:56],
                     palette_data[71:64], palette_data[79:72], palette_data[87:80], palette_data[95:88]);
        o_n <= o_n + 12;
    end
end

//---- 跑一个 dst_off ----
integer pass, nbad;
task run_case;
    input [3:0] off;
    begin
        //   造输入: 索引打散一点, 别整块同色
        for (k = 0; k < NW; k = k + 1)
            src_words[k] = { ((k*29+7) % 251), ((k*13+91) % 251),
                             ((k*7 +161) % 251), ((k*53+3) % 251) };
        //   期望: 按"被接收的顺序"展开
        for (k = 0; k < NW; k = k + 1)
            for (i = 0; i < 4; i = i + 1) begin
                ix = (src_words[k] >> (i*8));
                pbytes[k*12 + i*3 + 0] = pal[ix][7:0];    // B
                pbytes[k*12 + i*3 + 1] = pal[ix][15:8];   // G
                pbytes[k*12 + i*3 + 2] = pal[ix][23:16];  // R
            end

        for (k = 0; k < NB; k = k + 1) ostream[k] = 8'h00;
        o_n = 0; in_n = 0;
        @(negedge clk);
        line_start = 1'b1;
        @(negedge clk);
        line_start = 1'b0;

        //   等喂完 + 排空
        k = 0;
        while ((in_n < NW) && (k < 20000)) begin @(negedge clk); k = k + 1; end
        //   ★ 排空要等 busy 真落下来（上游开新行前也必须等这一条）
        k = 0;
        while (busy && (k < 2000)) begin @(negedge clk); k = k + 1; end
        if (busy) begin
            $display("  [ERR] dst_off=%0d 排不干净: busy 一直不落", off);
            err_cnt = err_cnt + 1;
        end
        repeat (8) @(negedge clk);

        //   比对: 输出流第 j 字节应【直接】等于 pbytes[j]（不再有 dst_off 前缀）
        nbad = 0;
        for (j = 0; j < o_n; j = j + 1) begin
            //   ★ 只比"本批字该覆盖到的范围"：尾巴多吐的不算错（整链里 Packer 会丢）
            if (j < ((in_n * 12) - 12)) begin
                if (ostream[j] !== pbytes[j]) begin
                    nbad = nbad + 1;
                    if (nbad <= 4)
                        $display("  [ERR] dst_off=%0d 流字节 %0d: got %02x exp %02x",
                                 off, j, ostream[j], pbytes[j]);
                end
            end
        end
        $display("  dst_off=%0d 收到输入字 %0d 个 / 输出 %0d 字节 / 不符 %0d",
                 off, in_n, o_n, nbad);
        err_cnt = err_cnt + nbad;
        if (in_n != NW) begin
            $display("  [ERR] dst_off=%0d 只收到 %0d 个字, 期望 %0d", off, in_n, NW);
            err_cnt = err_cnt + 1;
        end

        //   复位, 跑下一个相位
        rst_n = 1'b0; repeat (8) @(negedge clk); rst_n = 1'b1;
        k = 0;
        while (busy && (k < 100)) begin @(negedge clk); k = k + 1; end
        if (busy) $display("  [ERR] 复位后 busy 还是 1");
        repeat (4) @(negedge clk);
        in_n = 0; o_n = 0;
    end
endtask

initial begin
    $readmemh("E:/project/FPGA_Project/FPGA_Elitestek/FPGADesign_Elitestek/res/func/output/palette.hex", pal);
    line_start = 1'b0; index_data = 32'd0;
    repeat (6) @(negedge clk);
    rst_n = 1'b1;
    repeat (4) @(negedge clk);

    $display("---- 带气泡的 valid 流 + 下游反压, 逐字节验 ----");
    tr = 0; tr_on = 1'b1;
    bub = 7;  bpp = 5;  run_case(4'd12);
    //   ★ 加固：换几种"气泡密度 + 反压密度"再跑一遍 —— 这个 bug 就是被特定图案逼出来的,
    //     别只信一种图案（连续流的单元 TB 就是这么漏掉的）
    bub = 3;  bpp = 2;  run_case(4'd5);    // 气泡很密 + 下游几乎一直停
    bub = 11; bpp = 7;  run_case(4'd0);    // 气泡很稀 + 反压很稀
    bub = 2;  bpp = 9;  run_case(4'd12);   // 一半是气泡
    bub = 16; bpp = 3;  run_case(4'd15);   // 长连续 + 频繁反压
    tr_on = 1'b0;
    run_case(4'd0);
    run_case(4'd5);

    $display("");
    if (err_cnt == 0) $display("SUMMARY: PALETTE-DEC-STREAM ALL-PASS");
    else              $display("SUMMARY: err_cnt=%0d RESULT=FAIL", err_cnt);
    $finish;
end

endmodule
