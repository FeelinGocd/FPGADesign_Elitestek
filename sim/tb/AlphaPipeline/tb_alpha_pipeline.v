// Alpha pipeline scoreboard: independent integer /255 reference, backpressure,
// full-rate traffic, and row/reset cancellation of queued pixels.
`timescale 1ns/1ps
module tb_alpha_pipeline;
reg clk = 1'b0;
always #1.667 clk = ~clk;

reg rst_n = 1'b0;
reg line_start = 1'b0;
reg line_done = 1'b0;
reg [95:0] fg_data = 96'd0;
reg [95:0] bg_data = 96'd0;
reg [31:0] fg_alpha = 32'd0;
reg [11:0] fg_mask = 12'd0;
reg in_valid = 1'b0;
wire in_ready;
wire [95:0] blend_data;
wire [11:0] blend_mask;
wire out_valid;
reg out_ready = 1'b0;
wire pipeline_free;

alphaBlending u_dut (
    .clk(clk), .rst_n(rst_n), .line_start(line_start), .line_done(line_done),
    .fg_data(fg_data), .fg_alpha(fg_alpha), .bg_data(bg_data), .fg_mask(fg_mask),
    .in_valid(in_valid), .in_ready(in_ready),
    .blend_data(blend_data), .blend_mask(blend_mask),
    .out_valid(out_valid), .out_ready(out_ready), .pipeline_free(pipeline_free)
);

reg [95:0] expected_data [0:1023];
reg [11:0] expected_mask [0:1023];
integer q_read = 0;
integer q_write = 0;
integer q_count = 0;
integer errors = 0;
integer accepted = 0;
integer compared = 0;
integer cancelled = 0;
integer stall_checks = 0;
integer empty_checks = 0;
integer continuous_outputs = 0;
reg accepted_last = 1'b0;
reg output_last = 1'b0;
reg stalled = 1'b0;
reg [95:0] stalled_data;
reg [11:0] stalled_mask;

function [95:0] reference_data;
    input [95:0] f, b;
    input [31:0] a;
    integer j, fv, bv, av, distance, correction;
    begin
        reference_data = 96'd0;
        for (j = 0; j < 12; j = j + 1) begin
            fv = f[j*8 +: 8];
            bv = b[j*8 +: 8];
            av = a[(j/3)*8 +: 8];
            distance = fv >= bv ? fv-bv : bv-fv;
            // True integer division, independent from the RTL's shift/add /255.
            correction = distance * av / 255;
            reference_data[j*8 +: 8] = fv >= bv ? bv+correction : bv-correction;
        end
    end
endfunction

function [11:0] reference_mask;
    input [11:0] m;
    input [31:0] a;
    integer j;
    begin
        for (j = 0; j < 12; j = j + 1)
            reference_mask[j] = m[j] && (a[(j/3)*8 +: 8] != 0);
    end
endfunction

task fail;
    input [8*96-1:0] reason;
    begin
        errors = errors + 1;
        if (errors <= 12)
            $display("FAIL: %0s t=%0t queued=%0d", reason, $time, q_count);
    end
endtask

always @(posedge clk) begin
    accepted_last = 1'b0;
    output_last = 1'b0;
    // Clear takes precedence over input/output handshakes. A ready input during
    // this edge is an unwanted row tail and is deliberately discarded.
    if (!rst_n || line_start || line_done) begin
        cancelled = cancelled + q_count;
        q_read = 0;
        q_write = 0;
        q_count = 0;
        stalled = 1'b0;
        #0.010;
        if (out_valid !== 1'b0) fail("clear failed to remove output valid");
        if (pipeline_free !== 1'b1) fail("clear did not empty all pipeline stages");
    end
    else begin
        if (stalled) begin
            stall_checks = stall_checks + 1;
            if (out_valid !== 1'b1 || blend_data !== stalled_data || blend_mask !== stalled_mask)
                fail("output changed under backpressure");
        end
        if (out_valid && out_ready) begin
            output_last = 1'b1;
            if (q_count == 0) fail("unexpected or duplicated output");
            else begin
                if (blend_data !== expected_data[q_read] || blend_mask !== expected_mask[q_read]) begin
                    fail("RGB or mask differs from independent integer reference");
                    if (errors <= 12)
                        $display(" got=%h/%h expected=%h/%h", blend_data, blend_mask,
                                 expected_data[q_read], expected_mask[q_read]);
                end
                compared = compared + 1;
                q_read = (q_read + 1) % 1024;
                q_count = q_count - 1;
            end
        end
        if (in_valid && in_ready) begin
            accepted_last = 1'b1;
            expected_data[q_write] = reference_data(fg_data, bg_data, fg_alpha);
            expected_mask[q_write] = reference_mask(fg_mask, fg_alpha);
            q_write = (q_write + 1) % 1024;
            q_count = q_count + 1;
            accepted = accepted + 1;
            if (q_count >= 1024) fail("test scoreboard overflow");
        end
        stalled = out_valid && !out_ready;
        stalled_data = blend_data;
        stalled_mask = blend_mask;
        #0.010;
        empty_checks = empty_checks + 1;
        if (pipeline_free !== (q_count == 0)) fail("pipeline_free excludes pending data");
    end
end

task cycle;
    input v;
    input [95:0] f, b;
    input [31:0] a;
    input [11:0] m;
    input ready;
    begin
        @(negedge clk);
        in_valid = v; fg_data = f; bg_data = b; fg_alpha = a; fg_mask = m;
        out_ready = ready;
        @(posedge clk); #0.050;
    end
endtask

task drain;
    integer timeout;
    begin
        timeout = 0;
        while (q_count != 0 && timeout < 100) begin
            cycle(0, 0, 0, 0, 0, 1);
            timeout = timeout + 1;
        end
        if (q_count != 0) fail("pipeline failed to drain");
        repeat (8) cycle(0, 0, 0, 0, 0, 1);
    end
endtask

task send;
    input [95:0] f, b;
    input [31:0] a;
    input [11:0] m;
    integer timeout;
    begin
        cycle(1, f, b, a, m, 1);
        timeout = 0;
        while (!accepted_last && timeout < 100) begin
            cycle(1, f, b, a, m, 1);
            timeout = timeout + 1;
        end
        if (!accepted_last) fail("source remained blocked with ready consumer");
    end
endtask

// Cancel at different occupancies so data can reside in every arithmetic stage
// and in both full and partially occupied output FIFOs.
task cancellation_case;
    input integer which, depth;
    integer n;
    begin
        drain;
        for (n = 0; n < depth; n = n + 1)
            cycle(1, {3{32'hc37e4100+n}}, {3{32'h17ab8f52-n}},
                  32'hff800100, 12'hb6d, 0);
        if (q_count == 0) fail("cancellation case did not queue test data");
        case (which)
            0: line_start = 1'b1;
            1: line_done = 1'b1;
            2: rst_n = 1'b0;
        endcase
        cycle(1, {12{8'hfe}}, {12{8'h01}}, 32'hff7f0100, 12'hfff, 0);
        line_start = 1'b0; line_done = 1'b0; rst_n = 1'b1;
        repeat (12) cycle(0, 0, 0, 0, 0, 1);
        if (q_count != 0 || pipeline_free !== 1'b1) fail("cancelled tail reappeared");
        for (n = 0; n < 13; n = n + 1)
            send({12{8'h19+n}}, {12{8'he7-n}}, 32'h00ff7f01, 12'hd73);
        drain;
    end
endtask

integer n, j, a, distance, which, depth, seed;
integer before_accepted, before_compared;
reg [95:0] f, b;
reg [31:0] al;
reg [11:0] mask;
reg pending, ready;
initial begin
    seed = 32'h5a19c3e7;
    repeat (3) cycle(0, 0, 0, 0, 0, 0);
    rst_n = 1'b1;
    cycle(0, 0, 0, 0, 0, 1);

    // All 256 distances x all 256 alpha values; positive, negative and translated
    // channel differences are checked simultaneously in the twelve RGB bytes.
    for (a = 0; a < 256; a = a + 1) begin
        for (distance = 0; distance < 256; distance = distance + 1) begin
            for (j = 0; j < 12; j = j + 1) begin
                case (j % 3)
                    0: begin f[j*8 +: 8] = distance; b[j*8 +: 8] = 0; end
                    1: begin f[j*8 +: 8] = 0; b[j*8 +: 8] = distance; end
                    2: begin f[j*8 +: 8] = 255-distance; b[j*8 +: 8] = 255; end
                endcase
                mask[j] = (a ^ distance ^ j) & 1;
            end
            al = {4{a[7:0]}};
            send(f, b, al, mask);
        end
    end
    drain;
    $display("PASS: all 65536 distance/alpha combinations, both signs and per-byte masks");

    before_accepted = accepted;
    before_compared = compared;
    for (n = 0; n < 1024; n = n + 1) begin
        cycle(1, {3{32'h31a2c45f+n}}, {3{32'hf9c43102-n}}, 32'hfffe807f, 12'hfff, 1);
        if (!accepted_last) fail("continuous traffic did not accept one beat per clock");
        if (n >= 16) begin
            if (!output_last) fail("full-rate output developed a bubble");
            else continuous_outputs = continuous_outputs + 1;
        end
    end
    drain;
    if (accepted-before_accepted != 1024 || compared-before_compared != 1024)
        fail("continuous traffic count mismatch");
    $display("PASS: 1024 continuous beats, one output beat per clock after fill");

    // A compliant source holds its pending word through stalls. Force repeated
    // long stalls as well as irregular producer/consumer gaps.
    pending = 0;
    for (n = 0; n < 16000; n = n + 1) begin
        if (!pending && (($random(seed) & 7) != 0)) begin
            pending = 1;
            f = {$random(seed), $random(seed), $random(seed)};
            b = {$random(seed), $random(seed), $random(seed)};
            al = $random(seed); mask = $random(seed);
        end
        ready = ((n % 257) >= 37) && (($random(seed) & 3) != 0);
        cycle(pending, f, b, al, mask, ready);
        if (accepted_last) pending = 0;
    end
    if (pending) send(f, b, al, mask);
    drain;
    if (stall_checks < 1000) fail("insufficient blocked-output coverage");
    $display("PASS: random producer gaps, masks, alpha and consumer stalls; stable output");

    for (which = 0; which < 3; which = which + 1) begin
        cancellation_case(which, 1);
        cancellation_case(which, 3);
        cancellation_case(which, 8);
    end
    if (cancelled < 30) fail("insufficient cancellation occupancy coverage");
    if (accepted != compared + cancelled) fail("accepted/compared/cancelled accounting mismatch");
    if (errors == 0)
        $display("SUMMARY: tb_alpha_pipeline ALL-PASS accepted=%0d compared=%0d cancelled=%0d stall_checks=%0d full_rate=%0d free_checks=%0d",
                 accepted, compared, cancelled, stall_checks, continuous_outputs, empty_checks);
    else
        $display("SUMMARY: tb_alpha_pipeline FAIL errors=%0d", errors);
    $finish;
end

initial begin
    #10000000;
    $display("SUMMARY: tb_alpha_pipeline FAIL timeout");
    $finish;
end
endmodule
