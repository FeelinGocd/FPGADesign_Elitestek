# Main project: CPU/GPU 300 MHz; memory/AXI 100 MHz.
# CPU/GPU-to-memory CDC bounded by the faster clock period.
source outflow/GamingPlayUnit.pt.sdc
create_clock -name jtag_inst1_TCK -period 1250.000 [get_ports {jtag_inst1_TCK}]
set_clock_groups -asynchronous -group [get_clocks {jtag_inst1_TCK}]
set_max_delay -from [get_clocks core_clk] -to [get_clocks core_clk_feedback] 3.333
set_max_delay -from [get_clocks core_clk_feedback] -to [get_clocks core_clk] 3.333
# Same PLL outputs: retain normal hold checks.
set_max_delay -from [get_clocks core_clk_feedback] -to [get_clocks clk_HDMI_x1] 10.000
set_max_delay -from [get_clocks clk_HDMI_x1] -to [get_clocks core_clk_feedback] 10.000
set_false_path -hold -from [get_clocks core_clk_feedback] -to [get_clocks clk_HDMI_x1]
set_false_path -hold -from [get_clocks clk_HDMI_x1] -to [get_clocks core_clk_feedback]
