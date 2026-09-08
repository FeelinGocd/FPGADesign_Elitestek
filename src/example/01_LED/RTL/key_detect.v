/*-----------------------------------------------------------------------

CONFIDENTIAL IN CONFIDENCE
This confidential and proprietary software may be only used as authorized
by a licensing agreement from  EdgeWang (Thereturnofbingo).
In the event of publication, the following notice is applicable:
Copyright (C) 2023-20xx  EdgeWang Corporation
The entire notice above must be reproduced on all authorized copies.
Author				:		EdgeWang
Technology blogs 	: 	
Email Address 		: 		
Filename			:		key_detect
Date				:		2023-06-06
Description			:		
Modification History	:
Date			By			Version			Change Description
=========================================================================
23/6/6		EdgeWang	1.0				Original

****************************************************************/


module key_detect #(
    parameter CLK_FREQUENCY_MHz = 50
)(
    input i_key,
    input clk,
    input rst_n,
    output key_sw_en


);
localparam MAX_CNT = CLK_FREQUENCY_MHz * 10000*15 -1 ; //15ms

parameter s0 = 1'b0;
parameter s1 = 1'b1;

reg [31:0] cnt = 'd0;
reg         cnt_flag = 1'b0 ;
reg         state = s0;
always @( posedge clk or negedge rst_n ) 
begin
    if( !rst_n ) begin
        state = s0;
    end else begin
        case( state )
        s0 : begin
            if( ~i_key )
                state <= s1;
        end
        s1 : begin
            if( cnt_flag == 1 )
                state = s0;
        end

        default :;
        endcase
    end     
end 

always @( posedge clk or negedge rst_n ) 
begin
    if( !rst_n ) 
        cnt <= 'd0 ;
    else if( state == s0 )
        cnt <= 'd0;
    else if( state == s1 && i_key == 1'b1 )
        cnt <= cnt + 1'b1;
    else 
        cnt <= 'd0;

end 

always @( posedge clk or negedge rst_n )
begin
    if( !rst_n )
        cnt_flag <= 1'b0;
    else if( cnt == MAX_CNT)
        cnt_flag <= 1'b1;
    else 
        cnt_flag <= 'd0; 
end 

assign key_sw_en = cnt_flag ;


endmodule