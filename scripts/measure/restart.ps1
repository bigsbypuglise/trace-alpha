# Restart Trace on a clip, at frame 0.
#
# Always restart before locating the timeline groove: scrub.ps1 finds it by the
# unfilled track colour, which does not exist once the playhead reaches the end.
# Restarting is also what makes a measurement run reproducible -- an empty frame
# cache and a cold decoder, rather than whatever the previous gesture left.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$Exe,
    [int]$SettleSeconds = 4,
    # Tuning knobs for the run, as NAME=VALUE. Set on this process before the
    # child is started, and cleared afterwards, so an A/B is a parameter of the
    # command rather than of the shell it was typed into -- a leaked env var is
    # otherwise invisible and silently applies to every later measurement.
    [string[]]$Env = @()
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

# The dev HUD ships hidden (UI roadmap step 2, 2026-08-17) and every harness
# reads its figures off the HUD in a pixel capture -- so the launcher forces it
# on unless the caller says otherwise. Pass TRACE_HUD=0 in -Env to measure the
# shipping (HUD-hidden) configuration; any explicit TRACE_HUD entry wins.
$hasHudEntry = $false
foreach ($pair in $Env) { if ($pair -like 'TRACE_HUD=*') { $hasHudEntry = $true } }
if (-not $hasHudEntry) { $Env = @($Env) + @('TRACE_HUD=1') }

$applied = @()
foreach ($pair in $Env) {
    $i = $pair.IndexOf('=')
    if ($i -lt 1) { Write-Output "bad -Env entry '$pair', expected NAME=VALUE"; exit 1 }
    $name = $pair.Substring(0, $i)
    $value = $pair.Substring($i + 1)
    Set-Item -Path "env:$name" -Value $value
    $applied += "$name=$value"
}

$p = Start-Process -FilePath $Exe -ArgumentList "`"$Clip`"" -PassThru

foreach ($pair in $Env) {
    $name = $pair.Substring(0, $pair.IndexOf('='))
    Remove-Item -Path "env:$name" -ErrorAction SilentlyContinue
}

Start-Sleep -Seconds $SettleSeconds
if ($p.HasExited) { Write-Output "EXITED EARLY code $($p.ExitCode)"; exit 1 }
$envNote = if ($applied.Count -gt 0) { " env: " + ($applied -join ' ') } else { "" }
Write-Output "restarted at frame 0, pid $($p.Id)$envNote"
