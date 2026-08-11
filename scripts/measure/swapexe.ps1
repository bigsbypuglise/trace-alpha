# Swap the binary the harness runs, and PROVE the swap happened.
#
# None of the measurement scripts take an -Exe, so an A/B is done by putting
# each build at build\app\Release\Trace.exe in turn. The failure mode that makes
# that dangerous is silent: a Copy-Item that fails -- because Trace.exe is still
# running and holds a lock -- prints an error into a scrollback nobody reads,
# leaves the previous binary in place, and turns the A/B into two runs of the
# same build with a table full of plausible numbers.
#
# So this stops the app first, copies, and then reports the hash of what is
# actually there. Read the hash on every swap.

param(
    [Parameter(Mandatory = $true)][ValidateSet('control', 'head')][string]$Which,
    [string]$ControlExe = "$env:TEMP\trace_ctl11\build\app\Release\Trace.exe"
)

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$live = Join-Path $repo "build\app\Release\Trace.exe"
$stash = Join-Path $repo "build\app\Release\Trace.head.exe"

Get-Process -Name Trace -ErrorAction SilentlyContinue | ForEach-Object { $_.CloseMainWindow() | Out-Null }
Start-Sleep -Milliseconds 700
Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400

if ($Which -eq 'control') {
    if (-not (Test-Path $ControlExe)) { Write-Output "no control exe at $ControlExe"; exit 1 }
    # Keep HEAD's binary rather than rebuilding it back: a rebuild between legs
    # is another thing that can differ.
    if (-not (Test-Path $stash)) { Copy-Item $live $stash -Force }
    Copy-Item $ControlExe $live -Force
} else {
    if (-not (Test-Path $stash)) { Write-Output "no stashed HEAD exe; build it"; exit 1 }
    Copy-Item $stash $live -Force
}

$h = (Get-FileHash $live).Hash
Write-Output ("live binary is now {0}  sha256 {1}" -f $Which, $h.Substring(0, 8))
