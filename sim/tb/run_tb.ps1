
param([string]$TbDir, [string]$TbTop)
$ErrorActionPreference = 'Continue'
$ROOT = 'E:\project\FPGA_Project\FPGA_Elitestek\FPGADesign_Elitestek'
#  NOTE (keep this file ASCII-only; PS 5.1 reads BOM-less .ps1 as ANSI):
#  Compile target = the RELEASE RTL (repo\RTL) -- the same files Efinity synthesizes.
#  The frozen milestone copy under project\verified is no longer simulated.
$R = Join-Path $ROOT 'RTL'
$MS = 'E:\FPGA\ModelSim\modeltech64_10.5\win64'
$env:PATH = "$MS;$env:PATH"
$P = "$R\Peripherial"
$RUNDIR = Join-Path $ROOT "sim\tb\$TbDir"
Set-Location $RUNDIR
#  The release PaletteSource init paths are "hex/palette.hex", so mirror the hex files
#  into <rundir>\hex\ as well (a testbench's own $readmemh keeps using the run directory).
New-Item -ItemType Directory -Force -Path (Join-Path $RUNDIR 'hex') | Out-Null
foreach ($h in @('palette.hex','assets.hex')) {
  $src = Join-Path $RUNDIR $h
  if (!(Test-Path $src)) { $src = Join-Path $ROOT "sim\tb\$h" }
  if (Test-Path $src) { Copy-Item -LiteralPath $src -Destination (Join-Path $RUNDIR "hex\$h") -Force }
}
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
  "$P\GPU_2D\process\imageCombine.v",
  "$P\GPU_2D\cmd\CmdExec.v", "$P\GPU_2D\cmd\CmdChannel.v", "$P\GPU_2D\cmd\CmdBuffer.v", "$P\GPU_2D\cmd\CIUnpack.v",
  "$P\APB0\GPU_2D\APB0_GPU_2D.v",  "$P\GPU_2D\lineBuffer\lineBuffer.v", "$P\GPU_2D\lineBuffer\bufferControler.v", "$P\GPU_2D\lineBuffer\lineAligner.v", "$P\GPU_2D\lineBuffer\bufferUnpacker.v",
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