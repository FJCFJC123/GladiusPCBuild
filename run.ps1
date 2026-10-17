# Runs build\gladius.exe with a timeout and prints the port log.
param([int]$Seconds = 20)
Set-Location $PSScriptRoot\build
$p = Start-Process -FilePath .\gladius.exe -PassThru -NoNewWindow -RedirectStandardOutput out.txt -RedirectStandardError err.txt
if (-not $p.WaitForExit($Seconds * 1000)) { $p.Kill(); "TIMEOUT after $Seconds s" }
"exit code: 0x{0:X8}" -f $p.ExitCode
Get-Content port.log -ErrorAction SilentlyContinue | Select-Object -Last 40
