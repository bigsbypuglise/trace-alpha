# A playback run with the WHOLE lower HUD captured, not just the cadence band.
#
# cadence.ps1 crops six lines because that is all a cadence question needs. The
# audio non-regression check (plan section 24.9 criterion 4) needs `rep` and
# `skip` too, and those sit on the audio line above that crop -- so a run
# captured with cadence.ps1 cannot answer it and would silently look like it had.
#
# Same restart/play/capture sequence, taller band, one image per repeat.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$Label = "",
    [int]$Seconds = 11,
    [int]$Repeats = 2,
    [string[]]$Env = @(),
    [string]$OutDir = "$env:TEMP\trace_playhud",
    # Height of the band above the bottom of the window to keep. The HUD grows
    # with the media (audio adds a line), so this is deliberately generous.
    [int]$FromBottom = 470,
    [int]$BandH = 230
)

Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $OutDir | Out-Null
if (-not $Label) { $Label = [System.IO.Path]::GetFileNameWithoutExtension($Clip) }
if ($Label.Length -gt 26) { $Label = $Label.Substring(0, 26) }

$shots = @()
for ($r = 1; $r -le $Repeats; $r++) {
    & "$PSScriptRoot\restart.ps1" -Clip $Clip -Env $Env -SettleSeconds 5 | Out-Null
    & "$PSScriptRoot\play.ps1" -Seconds $Seconds | Out-Null
    $png = Join-Path $OutDir ("{0}_r{1}.png" -f ($Label -replace '[^\w\-]', '_'), $r)
    & "$PSScriptRoot\capture.ps1" -Out $png | Out-Null
    $tag = if ($Env.Count) { ($Env -join ' ') } else { "(defaults)" }
    $shots += @{ Png = $png; Label = ("{0}  rep{1}  {2}" -f $Label, $r, $tag) }
    Write-Output ("captured {0} rep {1}" -f $Label, $r)
}
Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

$probe = [System.Drawing.Bitmap]::FromFile($shots[0].Png)
$w0 = [int]($probe.Width * 0.86)
$probe.Dispose()

$lbl = 20
$dst = New-Object System.Drawing.Bitmap ($w0 * 2), ($shots.Count * ($BandH * 2 + $lbl))
$g = [System.Drawing.Graphics]::FromImage($dst)
$g.InterpolationMode = 'NearestNeighbor'
$g.Clear([System.Drawing.Color]::FromArgb(30, 0, 50))
$f = New-Object System.Drawing.Font('Consolas', 13, [System.Drawing.FontStyle]::Bold)
$i = 0
foreach ($s in $shots) {
    $src = [System.Drawing.Bitmap]::FromFile($s.Png)
    $top = [Math]::Max(0, $src.Height - $FromBottom)
    $ty = $i * ($BandH * 2 + $lbl)
    $g.DrawString($s.Label, $f, [System.Drawing.Brushes]::Yellow, 4, ($ty + 2))
    $g.DrawImage($src,
        (New-Object System.Drawing.Rectangle 0, ($ty + $lbl), ($w0 * 2), ($BandH * 2)),
        (New-Object System.Drawing.Rectangle 0, $top, $w0, $BandH), 'Pixel')
    $src.Dispose(); $i++
}
$out = Join-Path $OutDir ("summary_{0}.png" -f ($Label -replace '[^\w\-]', '_'))
$dst.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $dst.Dispose()
Write-Output $out
