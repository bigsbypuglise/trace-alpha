# Restart Trace on a clip, at frame 0.
#
# Always restart before locating the timeline groove: scrub.ps1 finds it by the
# unfilled track colour, which does not exist once the playhead reaches the end.
# Restarting is also what makes a measurement run reproducible -- an empty frame
# cache and a cold decoder, rather than whatever the previous gesture left.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$Exe,
    [int]$SettleSeconds = 4
)

if (-not $Exe) {
    $repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $Exe = Join-Path $repo "build\app\Release\Trace.exe"
}
if (-not (Test-Path $Exe)) { Write-Output "no exe at $Exe"; exit 1 }
if (-not (Test-Path $Clip)) { Write-Output "no clip at $Clip"; exit 1 }

Get-Process -Name Trace -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
Start-Sleep -Milliseconds 800
Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400

$p = Start-Process -FilePath $Exe -ArgumentList "`"$Clip`"" -PassThru
Start-Sleep -Seconds $SettleSeconds
if ($p.HasExited) { Write-Output "EXITED EARLY code $($p.ExitCode)"; exit 1 }
Write-Output "restarted at frame 0, pid $($p.Id)"
