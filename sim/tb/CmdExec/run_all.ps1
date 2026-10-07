$ErrorActionPreference = "Continue"
$R  = "E:\project\FPGA_Project\FPGA_Elitestek\FPGADesign_Elitestek\project\verified\06_SoC_2DGPU\RTL"
$MS = "E:\FPGA\ModelSim\modeltech64_10.5\win64"
$env:PATH = "$MS;$env:PATH"
Set-Location "E:\project\FPGA_Project\FPGA_Elitestek\FPGADesign_Elitestek\sim\tb\CmdExec"
if (Test-Path work_cmdexec) { Remove-Item -Recurse -Force work_cmdexec }
& "$MS\vlib.exe" work_cmdexec | Out-Null
$F = @(
  "$R\Peripherial\GPU_2D\cmd\CmdExec.v",
  "tb_CmdExec.v"
)
& "$MS\vlog.exe" -work work_cmdexec -quiet @F
Write-Host "=== VLOG EXIT $LASTEXITCODE ==="
if ($LASTEXITCODE -eq 0) {
  & "$MS\vsim.exe" -c -quiet -do "run -all; quit -f" work_cmdexec.tb_CmdExec -l run_cmdexec.log | Out-Null
  Write-Host "=== VSIM EXIT $LASTEXITCODE ==="
  Get-Content run_cmdexec.log -Tail 20
}