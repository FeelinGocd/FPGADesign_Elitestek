
$ErrorActionPreference = 'Continue'
$R = 'E:\project\FPGA_Project\FPGA_Elitestek\FPGADesign_Elitestek\project\verified\06_SoC_2DGPU\RTL'
$L = "$R\Peripherial\GPU_2D\lineBuffer"
$MS = 'E:\FPGA\ModelSim\modeltech64_10.5\win64'
$env:PATH = "$MS;$env:PATH"
Set-Location 'E:\project\FPGA_Project\FPGA_Elitestek\FPGADesign_Elitestek\sim\tb\lineBuffer'
if (Test-Path work) { Remove-Item -Recurse -Force work }
& "$MS\vlib.exe" work | Out-Null
$F = @(
  "$L\lineBuffer.v", "$L\bufferControler.v", "$L\bufferUnpacker.v",
  "$L\HDMI\asyn_rst_syn.v", "$L\HDMI\HDMI_Top.v", "$L\HDMI\Pixel_Timing\pixel_timing.v",
  "$L\HDMI\TMDS\dvi_encoder_Top.v", "$L\HDMI\TMDS\tmds_encoder.v",
  "$L\PingPongBuffer\async_fifo.v", "$L\PingPongBuffer\async_PingPongBuffer.v", "$L\PingPongBuffer\BlockRAM.v",
  "$R\Peripherial\AXI4\Master\AXI4_Master.v", "$R\Peripherial\AXI4\Master\AXI4_Master_Read.v", "$R\Peripherial\AXI4\Master\AXI4_Master_Write.v",
  "$R\Peripherial\GPU_2D\interface\GPU2D_APBHandler.v",
  "tb_lineBuffer.v"
)
& "$MS\vlog.exe" -work work -quiet @F
Write-Host "=== VLOG EXIT $LASTEXITCODE ==="