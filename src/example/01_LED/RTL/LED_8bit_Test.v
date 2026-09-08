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
Filename			:		LED_8bit_Test
Date				:		2023-06-06
Description			:		
Modification History	:
Date			By			Version			Change Description
=========================================================================
23/6/6		EdgeWang	1.0				Original

****************************************************************/

`timescale 1ns/1ns
module LED_8bit_Test
(
	//led interface
  input [3:0] key_i,
  input sys_pll_LOCKED,
  input clk_sel,
  input clk_50m,
  input clk_54m,
  input clk_25m_x2,
  (* syn_peri_port = 0 *) input osc_clk,
  (* syn_peri_port = 0 *) output osc_inst1_ENA,
  output [7:0] led_data,
  output sys_pll_RSTN
	);
  //============================================================
  // signal
  //============================================================
  wire       led_data1;
  wire       led_data2;
  wire       led_data3;
  wire       led_data4;
  wire       sys_rst_n;
  wire       key_sw_en1;
  wire       key_sw_en2;
  wire       key_sw_en3;
  wire       key_sw_en4;
  //============================================================
  //
  //============================================================

assign sys_pll_RSTN = key_i[0];
assign sys_rst_n = sys_pll_LOCKED;
assign osc_inst1_ENA = 1'b1;
// wire led_clk;
wire led_clk = clk_sel? clk_50m :clk_54m;

//----------------------------
key_detect # (
    .CLK_FREQUENCY_MHz(54)
  )
  key1_detect_inst (
    .i_key(key_i[0]),
    .clk(led_clk),
    .rst_n(sys_rst_n),
    .key_sw_en(key_sw_en1)
  );


led_ctrl led_ctrl_inst (
    .clk(clk_54m),
    .rst_n(sys_rst_n),
    .en(key_sw_en1),
    .led(led_data1)
  );
//=============================================== 
//
//===============================================
  key_detect # (
    .CLK_FREQUENCY_MHz(54)
  )
  key2_detect_inst (
    .i_key(key_i[1]),
    .clk(clk_50m),
    .rst_n(sys_rst_n),
    .key_sw_en(key_sw_en2)
  );
  led_ctrl clk_50m_led_ctrl_inst (
    .clk(clk_50m),
    .rst_n(sys_rst_n),
    .en(key_sw_en2),
    .led(led_data2)
  );
//=============================================== 
//
//===============================================
  key_detect # (
    .CLK_FREQUENCY_MHz(54)
  )
  key3_detect_inst (
    .i_key(key_i[2]),
    .clk(clk_25m_x2),
    .rst_n(sys_rst_n),
    .key_sw_en(key_sw_en3)
  );

  led_ctrl clk_25m_led_ctrl_inst (
    .clk(clk_25m_x2),
    .rst_n(sys_rst_n),
    .en(key_sw_en3),
    .led(led_data3)
  );

//=============================================== 
//
//===============================================
  key_detect # (
    .CLK_FREQUENCY_MHz(54)
  )
  key4_detect_inst (
    .i_key(key_i[3]),
    .clk(osc_clk),
    .rst_n(sys_rst_n),
    .key_sw_en(key_sw_en4)
  );

  led_ctrl clk_80m_led_ctrl_inst (
    .clk(osc_clk),
    .rst_n(sys_rst_n),
    .en(key_sw_en4),
    .led(led_data4)
  );

//=============================================== 
//
//===============================================

assign led_data = {{3{led_data4}},{2{led_data3}},led_data2,led_data1};

endmodule


