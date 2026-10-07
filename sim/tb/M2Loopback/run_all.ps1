$ErrorActionPreference = "Continue"
$R  = "E:\project\FPGA_Project\FPGA_Elitestek\FPGADesign_Elitestek\project\verified\06_SoC_2DGPU\RTL"
$MS = "E:\FPGA\ModelSim\modeltech64_10.5\win64"
$env:PATH = "$MS;$env:PATH"
Set-Location "E:\project\FPGA_Project\FPGA_Elitestek\FPGADesign_Elitestek\sim\tb\M2Loopback"
$F  = @(Get-ChildItem "$R\Peripherial\GPU_2D" -Recurse -Filter *.v | Select-Object -ExpandProperty FullName)
$F += @(Get-ChildItem "$R\Peripherial\AXI4" -Recurse -Filter *.v | Select-Object -ExpandProperty FullName)
if (Test-Path work_m2) { Remove-Item -Recurse -Force work_m2 }
& "$MS\vlib.exe" work_m2 | Out-Null
& "$MS\vlog.exe" -work work_m2 -quiet @F "tb_m2loopback.v" 2>&1 | Select-String -Pattern "rror" | Select-Object -First 6
if ($LASTEXITCODE -eq 0) {
  & "$MS\vsim.exe" -c -quiet -do "run -all; quit -f" work_m2.tb_m2loopback -l run_m2.log | Out-Null
  Write-Host "=== VSIM EXIT $LASTEXITCODE ==="
  Get-Content run_m2.log -Tail 20
} else { Write-Host "=== VLOG EXIT $LASTEXITCODE ===" }