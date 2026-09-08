module led_ctrl (
input wire  clk,
input wire  rst_n,
input wire  en,

output reg  led = 'd0
);
//===================================================================== 
//localparam
//=====================================================================
// localparam CNT_WIDTH = log2(LED_TIME);
//===================================================================== 
//reg
//=====================================================================
reg [26:0]          cnt = 'd0;
reg                 cnt_end = 'd0;
reg [1:0]           led_sel = 'd0;
//===================================================================== 
//main code
//=====================================================================
always @( posedge clk or negedge rst_n )
begin
    if( !rst_n ) begin
        cnt <= 'd0;
    end else begin
        cnt <= cnt + 1'b1;
    end


end 

always @( posedge clk or negedge rst_n )
begin
    if( !rst_n )
        led_sel <= 'd0;
    else if( en ) 
        led_sel <= led_sel + 1'b1 ;
end

always @( posedge clk or negedge rst_n )
begin
    if( !rst_n )
            led <= 'd0;
    else begin
        case( led_sel )
        2'd0 : led <= cnt[26];
        2'd1 : led <= cnt[25];
        2'd2 : led <= cnt[24];
        2'd3 : led <= cnt[23];
        default:;
        endcase

    end 
end




endmodule