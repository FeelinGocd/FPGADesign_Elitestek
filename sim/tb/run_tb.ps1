
param([string]$TbDir, [string]$TbTop)
$ErrorActionPreference = 'Continue'
$R = 'E:\project\FPGA_Project\FPGA_Elitestek\FPGADesign_Elitestek\project\verified\06_SoC_2DGPU\RTL'
$MS = 'E:\FPGA\ModelSim\modeltech64_10.5\win64'
$env:PATH = "$MS;$env:PATH"
$P = "$R\Peripherial"
Set-Location "E:\project\FPGA_Project\FPGA_Elitestek\FPGADesign_Elitestek\sim\tb\$TbDir"
if (Test-Path work) { Remove-Item -Recurse -Force work }
& "$MS\vlib.exe" work | Out-Null
$F = @(
  "$P\GPU_2D\GPU_2D.v",
  "$P\GPU_2D\interface\GPU2D_APBHandler.v", "$P\GPU_2D\interface\AXIMastersHandler.v",
  "$P\GPU_2D\factory\Factory.v", "$P\GPU_2D\factory\WriteController.v",
  "$P\GPU_2D\factory\DDRPacker.v", "$P\GPU_2D\factory\Compositor.v",
  "$P\GPU_2D\pixel\palette\PaletteRAM.v", "$P\GPU_2D\pixel\palette\PaletteLUT.v",
  "$P\GPU_2D\pixel\palette\PaletteDecoder.v", "$P\GPU_2D\pixel\palette\PaletteSource.v",
  "$P\GPU_2D\pixel\DDR\DDRSource.v", "$P\GPU_2D\pixel\DDR\DDRUnpacker.v",
  "$P\GPU_2D\process\colorKey.v", "$P\GPU_2D\process\alphaBlending.v",
  "$P\GPU_2D\cmd\CmdExec.v", "$P\GPU_2D\cmd\CmdChannel.v", "$P\GPU_2D\cmd\CmdBuffer.v", "$P\GPU_2D\cmd\CIUnpack.v",
  "$P\APB0\GPU_2D\APB0_GPU_2D.v",  "$P\GPU_2D\lineBuffer\lineBuffer.v", "$P\GPU_2D\lineBuffer\bufferControler.v", "$P\GPU_2D\lineBuffer\bufferUnpacker.v",
  "$P\GPU_2D\lineBuffer\HDMI\HDMI_Top.v", "$P\GPU_2D\lineBuffer\HDMI\asyn_rst_syn.v",
  "$P\GPU_2D\lineBuffer\HDMI\TMDS\dvi_encoder_Top.v", "$P\GPU_2D\lineBuffer\HDMI\TMDS\tmds_encoder.v",
  "$P\GPU_2D\lineBuffer\HDMI\Pixel_Timing\pixel_timing.v",
  "$P\GPU_2D\lineBuffer\PingPongBuffer\async_PingPongBuffer.v", "$P\GPU_2D\lineBuffer\PingPongBuffer\async_fifo.v",
  "$P\GPU_2D\lineBuffer\PingPongBuffer\BlockRAM.v",
  "$P\AXI4\Master\AXI4_Master.v", "$P\AXI4\Master\AXI4_Master_Read.v", "$P\AXI4\Master\AXI4_Master_Write.v",
  "$TbTop.v"
)
& "$MS\vlog.exe" -work work -quiet @F
Write-Host "=== $TbTop VLOG EXIT $LASTEXITCODE ==="
if ($LASTEXITCODE -eq 0) {
  & "$MS\vsim.exe" -c -quiet -do "run -all; quit -f" $TbTop 2>&1 | Tee-Object -FilePath "run_$TbTop.log"
  Write-Host "=== $TbTop VSIM EXIT $LASTEXITCODE ==="
}