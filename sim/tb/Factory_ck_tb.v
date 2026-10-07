`timescale 1ns/100ps
//==============================================================================
//  Factory_ck_tb.v —— 颜色键控(ck)端到端自检：索引 → PaletteDecoder → Factory → AXI
//------------------------------------------------------------------------------
//  ck 最容易错的【不是"比颜色"，是"掩码和数据有没有错位"】：
//  掩码按【字节】走 DDRPacker 的字节兜，数据也走同一个兜 —— 两者必须永远同一套移位。
//  错一位的表现就是"该写的没写 / 不该写的写了"，上板就是花屏，很难查。
//
//  怎么验（★ 不依赖任何"源像素怎么排"的假设）
//      同一行跑两遍，索引流完全一样，只改 flags[4]：
//        A 遍：ck 关 → 拿到这一行【该有的全部字节】(几何 + 数据)
//        B 遍：ck 开 → 拿到同一行
//      判据：
//        ① A 遍的 WSTRB 必须正好等于【几何掩码】= d ∈ [0,3w) 的位全 1，其余全 0
//        ② B 遍：对每个 d，看 A 遍里 d 所在的那个【目的像素】(d/3 那 3 个字节)
//                 是不是纯白(FF FF FF)：是 → WSTRB 必须 0；不是 → 必须 1
//        ③ B 遍凡是要写的位，数据必须和 A 遍【一模一样】
//      ② 就是 ck 的全部语义：白像素不写、其余照写。
//      错位的话，白像素的 3 个字节里必然有几个被写了 / 邻居被抠掉了 —— 一眼抓出来。
//
//  覆盖：各种相位(0/4/15)、长行、跨 4KB 多笔突发、FILL(不取源，ck 必须无效)、
//        行里没有白像素(等价全写)
//==============================================================================

module Factory_ck_tb;

//==============================================================================
// 时钟 / 复位
//==============================================================================
reg clk = 1'b0;
reg rst_n = 1'b0;
always #5 clk = ~clk;

integer errs = 0;
integer kase = 0;
//   模拟 APB 的 CKEY_COLOR 寄存器（0x2078）：本用例用哪个键色
reg [23:0] g_key = 24'hFFFFFF;
reg [6:0]  g_flags = 7'h00;     // 行任务里广播的 flags（Decoder 用 [5] 决定字内要不要反序）
//   调色板（TB 自己读一份，用来挑"真实存在的颜色"当键色）
//   ★ 一项 = AARRGGBB（32 位）：按 24 位读 8 位十六进制会读成 X（$readmemh 值超宽），
//     症状是"键色变成 X ⇒ 期望值全是 X ⇒ 满屏不一致"。用的时候一律取低 24 位。
reg [31:0] pal [0:255];
initial $readmemh("palette.hex", pal);

//==============================================================================
// 行任务
//==============================================================================
reg         line_start;
reg  [6:0]  line_flags;                     // ★ 模式（[2:0]）+ src 类型（[3]）+ ck（[4]）都在这 7 位里
reg  [23:0] line_param;
reg  [31:0] line_dst_addr;
reg  [15:0] line_w;
wire        line_done;
wire        buf_free;

//==============================================================================
// 索引源
//==============================================================================
reg  [31:0] idx_word;
wire        idx_valid;
reg         idx_en = 1'b0;
wire        idx_ready;
integer     iw;
integer     nwords;
reg  [7:0]  idx_mem [0:4095];      // w 最大 1920 ⇒ 索引要 480*4 + 余量
assign      idx_valid = idx_en & (iw < nwords);
always @(*) idx_word = { idx_mem[iw*4+3], idx_mem[iw*4+2], idx_mem[iw*4+1], idx_mem[iw*4+0] };
always @(posedge clk or negedge rst_n)
    if (!rst_n) iw <= 0;
    else if (idx_valid & idx_ready) iw <= iw + 1;

wire [95:0] pal_data;
wire [11:0] pal_mask;
wire [31:0] pal_alpha;                       // ★ 每像素 8 位 α（旁路）
wire        pal_valid;
wire        pal_ready;
wire        dec_busy;

//==============================================================================
// AXI 写从机模型
//==============================================================================
wire        cmd_wr_valid;
reg         cmd_wr_ready = 1'b1;
wire [31:0] cmd_wr_addr;
wire [7:0]  cmd_wr_len;
reg         cmd_wr_done = 1'b0;
reg         cmd_wr_err  = 1'b0;
wire [127:0] wr_data;
wire [15:0]  wr_strb;
wire         wr_valid;
wire         wr_ready;

wire [95:0] src0_data = 96'd0;
reg         src0_valid = 1'b0;
wire        src0_rd_en;
reg         src0_free = 1'b1;

integer wleft, wcnt;
reg     in_burst = 1'b0;
assign  wr_ready = in_burst;

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        in_burst <= 1'b0; wleft <= 0; wcnt <= 0; cmd_wr_done <= 1'b0;
    end
    else begin
        cmd_wr_done <= 1'b0;
        if (cmd_wr_valid & cmd_wr_ready & ~in_burst) begin
            wleft <= cmd_wr_len + 1; wcnt <= 0; in_burst <= 1'b1;
        end
        else if (in_burst & wr_valid & wr_ready) begin
            wcnt <= wcnt + 1;
            if (wcnt + 1 == wleft) begin in_burst <= 1'b0; cmd_wr_done <= 1'b1; end
        end
    end
end

//==============================================================================
// 抓取（一拍 16 字节）
//==============================================================================
integer     nbeat;
reg  [7:0]  cap_dat  [0:16383];
reg         cap_strb [0:16383];
integer     L;

always @(posedge clk)
    if (wr_valid & wr_ready) begin
        for (L = 0; L < 16; L = L + 1) begin
            cap_dat [nbeat*16 + L] <= wr_data[L*8 +: 8];
            cap_strb[nbeat*16 + L] <= wr_strb[L];
        end
        nbeat <= nbeat + 1;
    end

//  两遍的存档
reg  [7:0]  dat_a [0:16383];
reg         strb_a[0:16383];
reg  [7:0]  dat_b [0:16383];
reg         strb_b[0:16383];
integer     n_a, n_b;

//==============================================================================
// DUT
//==============================================================================
PaletteDecoder #(
    .INDEX_WIDTH(8), .COLOR_WIDTH(24), .INDEXES_PER_WORD(4), .BYTES_PER_BEAT(12),
    .PALETTE_INIT_FILE("palette.hex")
) u_dec (
    .clk(clk), .rst_n(rst_n), .line_start(line_start),   /* ★ 不再有 line_dst_off：相位整形已下沉到 DDRPacker */
    .key_color(g_key),                          // 键色: 来自 APB 寄存器(0x2078), TB 里用 g_key 模拟
    .line_flags(g_flags),                       // [4]=ck, [5]=MX（本 TB 只测 ck，MX 由 m3palette TB 测）
    .index_data(idx_word), .index_valid(idx_valid), .index_ready(idx_ready),
    .palette_data(pal_data), .palette_mask(pal_mask), .palette_valid(pal_valid),
    .palette_alpha(pal_alpha),                  // ★ α 旁路：直接接到 Factory 去（本 TB 不用混合，但别悬空）
    .palette_ready(pal_ready), .busy(dec_busy)
);

Factory #(.PIXEL_WIDTH(96), .DATA_WIDTH(128), .ADDR_WIDTH(32), .LEN_WIDTH(8)) u_fac (
    .clk(clk), .rst_n(rst_n), .line_start(line_start),
    .line_flags(line_flags), .line_param(line_param), .line_dst_addr(line_dst_addr),
    .line_w(line_w), .line_done(line_done),
    .src0_data(src0_data), .src0_valid(src0_valid), .src0_rd_en(src0_rd_en), .src0_free(src0_free),
    .src1_data(pal_data), .src1_mask(pal_mask), .src1_alpha(pal_alpha),
    .src1_valid(pal_valid), .src1_rd_en(pal_ready),
    .src1_free(1'b1), .buf_free(buf_free),
    .cmd_wr_valid(cmd_wr_valid), .cmd_wr_ready(cmd_wr_ready), .cmd_wr_addr(cmd_wr_addr),
    .cmd_wr_len(cmd_wr_len), .cmd_wr_done(cmd_wr_done), .cmd_wr_err(cmd_wr_err),
    .wr_data(wr_data), .wr_strb(wr_strb), .wr_valid(wr_valid), .wr_ready(wr_ready)
);

//==============================================================================
// 索引准备：每 7 个像素有 1 个用索引 0（= 白 = 透明），其余用索引 1..11
//==============================================================================
integer n;
task prepare_indices;
    input integer w;
    input integer mode;         // 0 = 每 7 个一个白；1 = 全用索引 5（没有白）
    begin
        nwords = (w / 4) + 8;
        for (n = 0; n < nwords*4; n = n + 1)
            idx_mem[n] = (mode == 1) ? 8'd5
                       : (((n % 7) == 3) ? 8'd0 : (8'd1 + (n % 11)));
    end
endtask

//==============================================================================
// 跑一行并抓取
//==============================================================================
integer guard;
task wait_done;
    begin
        guard = 0;
        while (!line_done && guard < 20000) begin @(negedge clk); guard = guard + 1; end
        if (!line_done) begin
            $display("!! [case %0d] line_done timeout", kase);
            errs = errs + 1;
            $finish;
        end
    end
endtask

//   ★ 每一遍都从复位开始：PaletteRAM 的读流水/窗口里可能留着上一遍的残字，
//     不清干净的话 B 遍会整行偏移几个像素 —— 那是 TB 的问题，不是 DUT 的。
task reset_dut;
    begin
        rst_n = 1'b0;
        repeat (5) @(negedge clk);
        rst_n = 1'b1;
        repeat (5) @(negedge clk);
    end
endtask

integer i, z;
task run_line;
    input [31:0] addr;
    input integer w;
    input integer ck;
    input integer is_fill;
    input integer imode;
    input integer slot;         // 0 → 存 A，1 → 存 B
    input [23:0] fill_col;
    begin
        prepare_indices(w, imode);
        reset_dut;
        //   ★ 先停料、再发 line_start、最后才开喂 —— 顺序不能反。
        //     反了的话，LUT 里"在飞"的那两个字会被 line_start 的 clear 吃掉，
        //     整行数据就前移 2 组(8 个像素)，看起来像"掩码和数据差 8 个像素"。
        //     真的 PaletteSource 也是 line_start 之后才开始取数的。
        idx_en = 1'b0;
        @(negedge clk);
        iw = 0; nbeat = 0;
        line_dst_addr = addr;
        line_w        = w[15:0];
        //   ★ 模式和源类型都在 flags 里：FILL = 模式 0；其余 = REPLACE(1) + 源是 PRAM(flags[3]=1) = 0x09
        //     （CmdExec 不再解"要哪几路源"，合成器 / DDRSource / 调色板源各自看 flags 判）
        line_flags    = (ck ? 7'h10 : 7'h00) | (is_fill ? 7'h00 : 7'h09);
        line_param    = fill_col;
        line_start    = 1'b1;
        @(negedge clk);
        line_start    = 1'b0;
        @(negedge clk);
        idx_en        = 1'b1;
        wait_done;
        idx_en = 1'b0;
        repeat (3) @(negedge clk);
        if (slot == 0) begin
            n_a = nbeat;
            for (i = 0; i < nbeat*16; i = i + 1) begin dat_a[i] = cap_dat[i]; strb_a[i] = cap_strb[i]; end
        end
        else begin
            n_b = nbeat;
            for (i = 0; i < nbeat*16; i = i + 1) begin dat_b[i] = cap_dat[i]; strb_b[i] = cap_strb[i]; end
        end
    end
endtask

//==============================================================================
// 比对
//==============================================================================
integer k, o, d, pix, jj, span, ebeat, exp_s, shown, nwhite, nsupp, b0;
task check_pair;
    input [31:0] addr;
    input integer w;
    input integer expect_ck;    // 1 = B 遍应当出现"白不写"；0 = B 遍必须和几何一致
    begin
        span  = addr[3:0] + 3*w;
        ebeat = (span + 15) / 16;

        if (n_a != ebeat) begin
            $display("!! [case %0d] A 遍拍数 %0d != 期望 %0d", kase, n_a, ebeat);
            errs = errs + 1;
        end
        if (n_b != n_a) begin
            $display("!! [case %0d] B 遍拍数 %0d != A 遍 %0d", kase, n_b, n_a);
            errs = errs + 1;
        end

        shown = 0; nwhite = 0; nsupp = 0;
        for (k = 0; k < n_a; k = k + 1) begin
            for (L = 0; L < 16; L = L + 1) begin
                o = 16*k + L;
                d = o - addr[3:0];

                // ---- ① A 遍 = 几何 ----
                exp_s = (d >= 0 && d < 3*w) ? 1 : 0;
                if (strb_a[k*16+L] !== exp_s) begin
                    errs = errs + 1;
                    if (shown < 10)
                        $display("!! [case %0d] A 拍%0d 字节%0d (d=%0d) WSTRB=%b 期望 %b",
                                 kase, k, L, d, strb_a[k*16+L], exp_s);
                    shown = shown + 1;
                end

                // ---- ②③ B 遍 ----
                //   ★ 透明单位是【源像素】，不是"目的像素"！源像素的 3 个字节落在
                //     d = dst_off + 3k + {0,1,2} —— dst_off 不是 3 的倍数时，
                //     它会横跨两个"目的像素"（按 d/3 分组是错的，第一版就栽在这）。
                //     抓取数组的下标 = o = d + dst_off，所以取 A 遍某个 d 的字节 =
                //     dat_a[d + dst_off]。
                if (d >= 0 && d < 3*w) begin
                    //   源像素 k 的 3 个字节：输出拍位置 p = dst_off + 3k + j
                    //   ⇒ 相对行首的偏移 d = p - dst_off = 3k + j，即【按 3 对齐】。
                    //   抓取数组下标 = p = d + dst_off，所以这个像素的 3 个字节在
                    //   dat_a[dst_off + 3*(d/3) + {0,1,2}]。
                    //   ★ 判据：这个源像素的 3 个字节【等于键色】⇒ 必须不写。
                    //     键色是 24 位整字比，流里的字节序是低位在前(B,G,R)。
                    b0 = addr[3:0] + 3*(d/3);
                    if (expect_ck) begin
                        if ((dat_a[b0]     == g_key[7:0])  &&
                            (dat_a[b0+1]   == g_key[15:8]) &&
                            (dat_a[b0+2]   == g_key[23:16])) begin
                            exp_s = 0; nwhite = nwhite + 1;
                        end
                        else exp_s = 1;
                    end
                    else exp_s = 1;

                    if (exp_s == 0) nsupp = nsupp + 1;

                    if (strb_b[k*16+L] !== exp_s) begin
                        errs = errs + 1;
                        if (shown < 10)
                            $display("!! [case %0d] B beat %0d byte %0d (d=%0d) WSTRB=%b exp %b  [A src-pix bytes %02x %02x %02x]",
                                     kase, k, L, d, strb_b[k*16+L], exp_s,
                                     dat_a[b0], dat_a[b0+1], dat_a[b0+2]);
                        shown = shown + 1;
                    end
                    else if (exp_s == 1 && dat_b[k*16+L] !== dat_a[k*16+L]) begin
                        errs = errs + 1;
                        if (shown < 10)
                            $display("!! [case %0d] B 拍%0d 字节%0d (d=%0d) WDATA=%02x 期望(=A) %02x",
                                     kase, k, L, d, dat_b[k*16+L], dat_a[k*16+L]);
                        shown = shown + 1;
                    end
                end
            end
        end
        if (expect_ck)
            $display("   case %0d: w=%0d off=%0d 拍%0d 白字节 %0d 被抑制 %0d -> %s",
                     kase, w, addr[3:0], n_a, nwhite, nsupp, (nwhite == nsupp) ? "OK" : "!! 数不对");
        else
            $display("   case %0d: w=%0d off=%0d 拍%0d (ck 不该有影响)", kase, w, addr[3:0], n_a);
    end
endtask

//==============================================================================
// ★ 绝对检查：常数色(FILL) 写进 DDR 的每一个字节，必须就是那个颜色
//   为什么单开一个：老的 check_pair 只比【A 遍和 B 遍一致】+ 掩码
//   ⇒ "整块颜色被轮转"（R/B 互换）这种错它永远看不见。
//   模型：目的块第 d 个字节应该是颜色的第 (d mod 3) 个字节（0=B, 1=G, 2=R，全工程字节序）。
//==============================================================================
task chk_fill_abs;
    input [31:0] addr;
    input integer w;
    input [23:0] col;
    integer k, L, d, bad;
    reg [31:0] maddr;
    reg  [7:0] exp;
    begin
        kase = kase + 1;
        run_line(addr, w, 0, 1, 0, 0, col);          // 只跑 A 遍：纯 FILL
        bad = 0;
        for (k = 0; k < n_a; k = k + 1) begin
            for (L = 0; L < 16; L = L + 1) begin
                maddr = (addr & 32'hFFFF_FFF0) + k*16 + L;   // 这一字节在内存里的地址
                d = maddr - addr;                            // 目的块内偏移
                if (d >= 0 && d < 3*w) begin
                    exp = (d % 3 == 0) ? col[7:0] : (d % 3 == 1) ? col[15:8] : col[23:16];
                    if ((strb_a[k*16+L] !== 1'b1) || (dat_a[k*16+L] !== exp)) begin
                        if (bad < 4)
                            $display("!! [FILL off=%0d] 内存+%0d (块内 d=%0d) got %02x/%b exp %02x/1",
                                     addr[3:0], k*16+L, d, dat_a[k*16+L], strb_a[k*16+L], exp);
                        bad = bad + 1;
                    end
                end
            end
        end
        if (bad == 0) $display("   FILL off=%0d w=%0d: 逐字节正确", addr[3:0], w);
        else begin
            errs = errs + bad;
            $display("!! [FILL off=%0d] 共 %0d 个字节不对（常数色被轮转了）", addr[3:0], bad);
        end
    end
endtask

//==============================================================================
// 一个用例 = 同一行跑两遍
//==============================================================================
task case_line;
    input [31:0] addr;
    input integer w;
    input integer ck;
    input integer is_fill;
    input integer imode;
    input [23:0] fill_col;
    input [23:0] key;
    begin
        kase = kase + 1;
        g_key = key;
        run_line(addr, w, 0,     is_fill, imode, 0, fill_col);   // A：ck 关
        run_line(addr, w, ck,    is_fill, imode, 1, fill_col);   // B：本用例的 ck
        check_pair(addr, w, (ck != 0) && (is_fill == 0) && (imode == 0));
    end
endtask

//==============================================================================
// 主流程
//==============================================================================
initial begin
    line_start = 0; line_flags = 0; line_param = 0;
    line_dst_addr = 0; line_w = 0;

    repeat (10) @(negedge clk);
    rst_n = 1'b1;
    repeat (10) @(negedge clk);

    $display("==== Factory color-key self check ====");

    case_line(32'h0800_0000,  65, 0, 0, 0, 24'h0, 24'hFFFFFF);      // 基准：ck 关（B 遍也关）
    case_line(32'h0800_0000,  65, 1, 0, 0, 24'h0, 24'hFFFFFF);      // off=0
    case_line(32'h0800_0004,  65, 1, 0, 0, 24'h0, 24'hFFFFFF);      // off=4
    case_line(32'h0800_000F,  65, 1, 0, 0, 24'h0, 24'hFFFFFF);      // off=15（最难）
    case_line(32'h0800_0107, 200, 1, 0, 0, 24'h0, 24'hFFFFFF);      // 长行
    case_line(32'h0800_0000, 1920, 1, 0, 0, 24'h0, 24'hFFFFFF);     // 整行 5760 字节
    case_line(32'h0800_0FF8, 100, 1, 0, 0, 24'h0, 24'hFFFFFF);      // 跨 4KB -> 多笔突发
    case_line(32'h0800_0004,  65, 1, 1, 0, 24'hFF8040, 24'hFFFFFF); // FILL：ck 必须无效
    case_line(32'h0800_0004,  65, 1, 0, 1, 24'h0, 24'hFFFFFF);      // 行里没有白像素

    //   ---- 键色可配（APB 0x2078）----
    //   用调色板里真实存在的一个颜色当键色：那一档像素必须被抠掉
    case_line(32'h0800_0004,  65, 1, 0, 0, 24'h0, pal[5]);
    case_line(32'h0800_0004,  65, 1, 0, 0, 24'h0, pal[11]);
    //   用一个调色板里【不存在】的颜色当键色 ⇒ 一个像素都不该被抠（等价全写）
    case_line(32'h0800_0004,  65, 1, 0, 0, 24'h0, 24'h123456);

    //   ---- ★ FILL 逐字节绝对检查：dst_off 全扫 0~15（老 TB 的盲区）----
    //   off%3 != 0 的那几档就是"常数色按拍网格铺"会翻车的地方（R/B 互换）。
    for (k = 0; k < 16; k = k + 1)
        chk_fill_abs(32'h0800_0000 + k, 16, 24'h123456);
    chk_fill_abs(32'h0800_0007, 64, 24'hC86432);         // 长一点，跨多拍
    if (errs == 0) $display("==== ALL PASS (%0d cases) ====", kase);
    else           $display("==== FAIL: %0d mismatches (%0d cases) ====", errs, kase);
    $finish;
end

endmodule
