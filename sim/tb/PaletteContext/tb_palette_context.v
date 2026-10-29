`timescale 1ns/1ps
// Exercise the real CmdExec broadcast: S_SEND advances both source addresses
// immediately after the task handshake. Compare the normal connection against
// a source whose input addresses are held at the task snapshot. Both are also
// checked against the ROM's independent coordinate-to-colour golden model.
module tb_palette_context #(
    parameter EMULATE_OLD_BUG = 0,
    parameter TEST_WIDTH = 0 // 0 runs 1,2,3,4,5,8,9; nonzero focuses one width.
);
reg clk = 1'b0;
always #1.667 clk = ~clk;
reg rst_n = 1'b0;
reg cmd_valid = 1'b0;
wire cmd_ready;
reg [15:0] cmd_flags = 0;
reg [15:0] command_width = 8;
wire line_start, line_fx_en, line_done, command_done;
wire [31:0] line_dst_addr;
wire [15:0] line_w;
wire [63:0] line_src_addr;
wire [15:0] line_flags;
wire [47:0] line_param;
reg [63:0] source_snapshot = 0;
wire [63:0] held_source_addr;
wire [1:0] source_free, palette_valid, image_next;
wire [1:0] source_read_take;
wire [95:0] palette_data [0:1];
wire [11:0] palette_mask [0:1];
wire [31:0] palette_alpha [0:1];
reg task_pending = 1'b0;
reg [15:0] random_r = 16'hacf1;
wire palette_ready;
integer row_number = 0;
integer command_number = 0;
integer checked [0:1];
integer errors [0:1];
integer beat_number [0:1];
integer read_number [0:1];
integer read_checked [0:1];
integer stalled_cycles [0:1];
reg pass_two [0:1];
integer current_rows = 0;
integer advance_checks = 0;
integer expected_checked = 0;
integer expected_advances = 0;
integer widths_checked = 0;
integer width_case;
integer row_width = 0;
integer row_beats = 0;
reg advance_seen = 1'b0;
integer dut_index, lane, pixel_number, address_word, index_value;
reg [95:0] expected_data;
reg [11:0] expected_mask;
reg [7:0] expected_index;
reg [7:0] expected_flags;
reg [31:0] expected_source;
reg [31:0] second_snapshot;

assign held_source_addr = line_start ? line_src_addr : source_snapshot;
assign palette_ready = random_r[0] | random_r[3];
assign line_done = task_pending & (&source_free) & ~line_start;

CmdExec u_cmd (
    .clk_acc(clk), .rst_acc_n(rst_n),
    .cmd_valid(cmd_valid), .cmd_ready(cmd_ready), .cmd_type(3'd2),
    .cmd_src({32'd80, 32'd0}), .cmd_dst(32'h0800_0000),
    .cmd_w(command_width), .cmd_h(16'd5), .cmd_flags(cmd_flags),
    .cmd_args({32'd4, 32'd4}), .lineBuffer_mode(1'b0),
    .line_start(line_start), .line_dst_addr(line_dst_addr), .line_w(line_w),
    .line_fx_en(line_fx_en), .line_src_addr(line_src_addr),
    .line_flags(line_flags), .line_param(line_param),
    .line_src_addr_last(), .line_flags_last(), .line_param_last(),
    .line_done(line_done),
    .line_buffer_free((&source_free) & ~task_pending),
    .acc_busy(), .cmd_done(command_done), .frame_done()
);

genvar si, pi;
generate
    for (si = 0; si < 2; si = si + 1) begin : g_source
        PaletteSource #(
            .RAM_ADDRESS_WIDTH(8), .PALETTE_INIT_FILE(""),
            .INDEX_INIT_FILE("")
        ) u_source (
            .clk(clk), .rst_n(rst_n), .line_start(line_start),
            .line_src_addr(si == 0 ? line_src_addr : held_source_addr),
            .line_dst_addr(line_dst_addr), .line_w(line_w),
            .line_fx_en(line_fx_en), .line_flags(line_flags),
            .line_param(line_param), .key_color(24'hffffff),
            .palette_ready(palette_ready), .palette_data(palette_data[si]),
            .palette_mask(palette_mask[si]),
            .palette_alpha(palette_alpha[si]),
            .palette_valid(palette_valid[si]),
            .line_src1_free(source_free[si]), .line_image_next(image_next[si])
        );
        assign source_read_take[si] = u_source.ram_read_request & u_source.index_read_accept;
        // Test-only deterministic ROM contents; no external fixture files.
        integer word_i, byte_i;
        reg [31:0] word_value;
        initial begin
            #0.1;
            for (word_i = 0; word_i < 256; word_i = word_i + 1) begin
                for (byte_i = 0; byte_i < 4; byte_i = byte_i + 1)
                    word_value[byte_i*8 +: 8] = (word_i*4 + byte_i) & 255;
                u_source.u_PaletteRAM.u_BlockRAM.BUFFER[word_i] = word_value;
            end
        end
        for (pi = 0; pi < 4; pi = pi + 1) begin : g_palette_init
            integer entry_i;
            initial begin
                #0.1;
                for (entry_i = 0; entry_i < 256; entry_i = entry_i + 1)
                    u_source.u_PaletteDecoder.u_PaletteLUT.g_lane[pi].u_PaletteRAM.u_BlockRAM.BUFFER[entry_i]
                        = 32'hff000000 | (entry_i * 32'h00010101);
            end
        end
    end
endgenerate

// Negative control: reproduce only the old second-pass capture after the
// source's NBA update, without editing or replacing any release RTL. This must
// make the live source fail while the held-address reference still passes.
generate if (EMULATE_OLD_BUG) begin : g_old_capture
    always @(posedge clk) begin
        if (rst_n && g_source[0].u_source.switched &&
            g_source[0].u_source.task_rot_1) begin
            #0.01;
            g_source[0].u_source.rot_lane_r = line_src_addr[63:62];
            g_source[0].u_source.rot_base_addr_r = {2'b0, line_src_addr[61:32]};
        end
    end
end endgenerate

always @(posedge clk) begin
    if (!rst_n) begin
        task_pending <= 1'b0;
        random_r <= 16'hacf1;
    end else begin
        random_r <= {random_r[14:0], random_r[15] ^ random_r[13] ^ random_r[12] ^ random_r[10]};
        if (line_start) begin
            source_snapshot <= line_src_addr;
            task_pending <= 1'b1;
            second_snapshot = line_src_addr[63:32];
            row_width = line_w;
            // A row occupies complete 16-byte destination beats; the source
            // supplies complete groups of four RGB pixels (12 bytes).
            row_beats = ((((row_width*3 + 15)/16)*16) + 11)/12;
            row_number = current_rows;
            current_rows = current_rows + 1;
            advance_seen = 1'b0;
            for (dut_index = 0; dut_index < 2; dut_index = dut_index + 1) begin
                pass_two[dut_index] = 1'b0;
                beat_number[dut_index] = 0;
                read_number[dut_index] = 0;
            end
        end else if (line_done) begin
            task_pending <= 1'b0;
            for (dut_index = 0; dut_index < 2; dut_index = dut_index + 1)
                if (!pass_two[dut_index] || beat_number[dut_index] != row_beats ||
                    read_number[dut_index] != (cmd_flags[15] ? row_width : row_beats))
                    $fatal(1, "case=%0d width=%0d dut=%0d row=%0d second-pass beats=%0d reads=%0d",
                           command_number, row_width, dut_index, row_number,
                           beat_number[dut_index], read_number[dut_index]);
        end
        if (task_pending && !line_start && !advance_seen && line_src_addr != source_snapshot) begin
            advance_seen = 1'b1;
            advance_checks = advance_checks + 1;
        end
        for (dut_index = 0; dut_index < 2; dut_index = dut_index + 1) begin
            if (source_read_take[dut_index]) begin
                read_number[dut_index] = read_number[dut_index] + 1;
                read_checked[dut_index] = read_checked[dut_index] + 1;
            end
            if (palette_valid[dut_index] && !palette_ready)
                stalled_cycles[dut_index] = stalled_cycles[dut_index] + 1;
            if (image_next[dut_index]) begin
                if (beat_number[dut_index] != row_beats ||
                    read_number[dut_index] != (cmd_flags[7] ? row_width : row_beats))
                    $fatal(1, "case=%0d width=%0d dut=%0d first-pass beats=%0d reads=%0d",
                           command_number, row_width, dut_index,
                           beat_number[dut_index], read_number[dut_index]);
                pass_two[dut_index] = 1'b1;
                beat_number[dut_index] = 0;
                read_number[dut_index] = 0;
            end
            if (palette_valid[dut_index] && palette_ready) begin
                expected_data = 96'd0;
                expected_mask = 12'd0;
                expected_flags = pass_two[dut_index] ? cmd_flags[15:8] : cmd_flags[7:0];
                expected_source = pass_two[dut_index] ? second_snapshot : source_snapshot[31:0];
                for (lane = 0; lane < 4; lane = lane + 1) begin
                    pixel_number = beat_number[dut_index]*4 + lane;
                    if (expected_flags[7]) begin
                        if (pixel_number >= row_width) expected_index = 0;
                        else begin
                            address_word = (expected_source & 32'h3fffffff)
                                         + (expected_flags[5] ? (row_width-1-pixel_number)*4 : pixel_number*4);
                            index_value = address_word*4 + expected_source[31:30];
                            expected_index = index_value & 255;
                        end
                    end else begin
                        address_word = expected_source
                                     + (expected_flags[5] ? -beat_number[dut_index] : beat_number[dut_index]);
                        expected_index = (address_word*4 + (expected_flags[5] ? 3-lane : lane)) & 255;
                    end
                    expected_data[lane*24 +: 24] = {expected_index, expected_index, expected_index};
                    expected_mask[lane*3 +: 3] = (expected_index == 8'hff) ? 3'b000 : 3'b111;
                end
                if (palette_data[dut_index] !== expected_data ||
                    palette_mask[dut_index] !== expected_mask ||
                    palette_alpha[dut_index] !== 32'hffffffff) begin
                    if (errors[dut_index] < 4)
                        $display("MISMATCH case=%0d dut=%0d row=%0d pass2=%b beat=%0d actual=%h expected=%h",
                                 command_number, dut_index, row_number, pass_two[dut_index],
                                 beat_number[dut_index], palette_data[dut_index], expected_data);
                    errors[dut_index] = errors[dut_index] + 1;
                end
                checked[dut_index] = checked[dut_index] + 1;
                beat_number[dut_index] = beat_number[dut_index] + 1;
            end
        end
    end
end

task run_command;
    input [7:0] first_flags;
    input [7:0] second_flags;
    integer live_before, held_before;
    begin
        @(negedge clk);
        live_before = errors[0];
        held_before = errors[1];
        command_number = command_number + 1;
        current_rows = 0;
        cmd_flags = {second_flags, first_flags}; // REPLACE|PRAM in both slots.
        cmd_valid = 1'b1;
        while (!cmd_ready) @(negedge clk);
        @(posedge clk);
        @(negedge clk);
        cmd_valid = 1'b0;
        while (!command_done) @(negedge clk);
        if (current_rows != 5) $fatal(1, "missing row tasks");
        expected_checked = expected_checked + 5*2*row_beats;
        expected_advances = expected_advances + 5;
        $display("CASE: %0d width=%0d flags0=%h flags1=%h live_errors=%0d held_errors=%0d",
                 command_number, command_width, first_flags, second_flags,
                 errors[0]-live_before, errors[1]-held_before);
        repeat (3) @(negedge clk);
    end
endtask

task run_width;
    input integer width_value;
    integer live_stalls_before, held_stalls_before;
    begin
        command_width = width_value;
        live_stalls_before = stalled_cycles[0];
        held_stalls_before = stalled_cycles[1];
        run_command(8'h09, 8'h89); // ROT90.
        run_command(8'h09, 8'hc9); // ROT90|MY: includes column/lane wrap.
        run_command(8'h09, 8'ha9); // ROT90|MX: reverse within the sampled column.
        run_command(8'h09, 8'he9); // ROT90|MX|MY.
        run_command(8'h29, 8'h89); // First-pass MX must clear for rotated pass two.
        run_command(8'h89, 8'h29); // Rotated pass one must latch ordinary MX in pass two.
        if (stalled_cycles[0] <= live_stalls_before || stalled_cycles[1] <= held_stalls_before)
            $fatal(1, "width=%0d did not exercise output backpressure", width_value);
        widths_checked = widths_checked + 1;
    end
endtask

initial begin
    checked[0] = 0; checked[1] = 0;
    errors[0] = 0; errors[1] = 0;
    beat_number[0] = 0; beat_number[1] = 0;
    read_number[0] = 0; read_number[1] = 0;
    read_checked[0] = 0; read_checked[1] = 0;
    stalled_cycles[0] = 0; stalled_cycles[1] = 0;
    pass_two[0] = 0; pass_two[1] = 0;
    repeat (8) @(negedge clk);
    rst_n = 1'b1;
    if (TEST_WIDTH != 0) run_width(TEST_WIDTH);
    else begin
        for (width_case = 1; width_case <= 5; width_case = width_case + 1)
            run_width(width_case);
        run_width(8);
        run_width(9);
    end
    if (checked[0] != expected_checked || checked[1] != expected_checked ||
        advance_checks != expected_advances)
        $fatal(1, "coverage live=%0d held=%0d preadvance=%0d", checked[0], checked[1], advance_checks);
    $display("SUMMARY: tb_palette_context widths=%0d commands=%0d old_bug=%0d live_errors=%0d held_errors=%0d live_beats=%0d held_beats=%0d live_reads=%0d held_reads=%0d preadvanced_rows=%0d stalls=%0d/%0d RESULT=%0s",
             widths_checked, command_number, EMULATE_OLD_BUG, errors[0], errors[1],
             checked[0], checked[1], read_checked[0], read_checked[1], advance_checks,
             stalled_cycles[0], stalled_cycles[1],
             (errors[0] == 0 && errors[1] == 0) ? "ALL-PASS" : "FAIL");
    $finish;
end
initial begin
    #200000;
    $fatal(1, "SUMMARY: tb_palette_context timeout");
end
endmodule
