# Launch the same build and the same clip on both renderers, land on the SAME
# known frame, and capture each -- so a CPU/GPU comparison is a comparison of
# two pictures of one frame rather than of whatever each run happened to open on.
#
# -Frame is mandatory in spirit: the default of 0 is exactly the case that
# produced a wrong conclusion at GATE B, because the 1080p clip opens on black.
# visual.ps1 refuses to call a uniform dark rect a pass, and reports
# INDETERMINATE instead.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [Parameter(Mandatory = $true)][string]$Name,
    [int]$Frame = 30,
    [string]$OutDir = "$env:TEMP\trace_ab"
)

New-Item -ItemType Directory -Force $OutDir | Out-Null

foreach ($r in @("cpu", "d3d11")) {
    & "$PSScriptRoot\restart.ps1" -Clip $Clip -Env "TRACE_RENDERER=$r" | Out-Null
    $png = Join-Path $OutDir "$Name`_$r.png"
    $res = & "$PSScriptRoot\visual.ps1" -Out $png -Frame $Frame
    $code = $LASTEXITCODE
    $res | ForEach-Object { Write-Output ("[{0}/{1}] {2}" -f $Name, $r, $_) }
    if ($code -ne 0) { Write-Output ("[{0}/{1}] exit {2}" -f $Name, $r, $code) }
}

Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Output ("[{0}] both captures in {1}" -f $Name, $OutDir)
