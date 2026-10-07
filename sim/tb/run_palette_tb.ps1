
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
  "$P\GPU_2D\pixel\palette\PaletteRAM.v", "$P\GPU_2D\pixel\palette\PaletteLUT.v",
  "$P\GPU_2D\pixel\palette\PaletteDecoder.v", "$P\GPU_2D\pixel\palette\PaletteSource.v",
  "$P\GPU_2D\lineBuffer\PingPongBuffer\BlockRAM.v",
  "$TbTop.v"
)
& "$MS\vlog.exe" -work work -quiet @F
Write-Host "=== $TbTop VLOG EXIT $LASTEXITCODE ==="
if ($LASTEXITCODE -eq 0) {
  & "$MS\vsim.exe" -c -quiet -do "run -all; quit -f" $TbTop 2>&1 | Tee-Object -FilePath "run_$TbTop.log" | Out-Null
  Write-Host "=== $TbTop VSIM EXIT $LASTEXITCODE ==="
  Get-Content "run_$TbTop.log" -Tail 12
}