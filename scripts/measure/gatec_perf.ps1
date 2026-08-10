# GATE C: does moving the colour conversion to the GPU pay, and does it cost
# anything on the scrub path?
#
# Three configurations per clip, because two would not separate the two things
# that changed:
#   cpu          - swscale to BGRA, QPainter blit           (the shipping default)
#   d3d11-bgra   - swscale to BGRA, GPU present             (GATE B)
#   d3d11-planar - plane copy, GPU converts                 (GATE C)
#
# The middle row is the control that matters. Comparing GATE C against `cpu`
# alone would credit the planar path with the presentation win GATE B already
# booked (plan section 17.4 records paint 0.54ms -> 0.14ms and explicitly says
# not to book it twice).
#
# SCRUB IS MEASURED SEPARATELY and that is the point of section 20.7: previews
# are converted to display size by swscale and deliberately stay that way, so
# the scrub rows should read UNCHANGED. A scrub row that improves means previews
# started taking the planar path, which would be uploading full-resolution
# planes to draw a 640x360 picture; a scrub row that regresses means the same
# thing more expensively. Either way the number to check is "no change".

param(
    [Parameter(Mandatory = $true)][string[]]$Clips,
    [int]$PlaySeconds = 9,
    [string]$OutDir = "$env:TEMP\trace_gatec_perf",
    [switch]$SkipScrub
)

Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $OutDir | Out-Null

$configs = @(
    @{ Name = "cpu";          Env = @("TRACE_RENDERER=cpu") },
    @{ Name = "d3d11-bgra";   Env = @("TRACE_RENDERER=d3d11", "TRACE_PLANAR_UPLOAD=0") },
    @{ Name = "d3d11-planar"; Env = @("TRACE_RENDERER=d3d11") }
)

# Stack the HUD lines that answer this question from every run into one image,
# so the whole matrix is read in one look instead of one capture at a time.
$strips = @()

foreach ($clip in $Clips) {
    $short = [System.IO.Path]::GetFileNameWithoutExtension($clip)
    if ($short.Length -gt 22) { $short = $short.Substring(0, 22) }
    foreach ($cfg in $configs) {
        # Twice, discarding the first: the first d3d11 run of a session carries a
        # warm-up cost large enough to read as a regression (plan section 21.4).
        foreach ($pass in @(1, 2)) {
            & "$PSScriptRoot\restart.ps1" -Clip $clip -Env $cfg.Env -SettleSeconds 5 | Out-Null
            & "$PSScriptRoot\play.ps1" -Seconds $PlaySeconds | Out-Null
            if ($pass -eq 2) {
                $png = Join-Path $OutDir ("play_{0}_{1}.png" -f $short, $cfg.Name)
                & "$PSScriptRoot\capture.ps1" -Out $png | Out-Null
                $strips += @{ Png = $png; Label = ("PLAY  {0}  {1}" -f $short, $cfg.Name) }
            }
        }

        if (-not $SkipScrub) {
            foreach ($pass in @(1, 2)) {
                & "$PSScriptRoot\restart.ps1" -Clip $clip -Env $cfg.Env -SettleSeconds 5 | Out-Null
                & "$PSScriptRoot\scrub.ps1" -Reversals | Out-Null
                if ($pass -eq 2) {
                    $png = Join-Path $OutDir ("scrub_{0}_{1}.png" -f $short, $cfg.Name)
                    & "$PSScriptRoot\capture.ps1" -Out $png | Out-Null
                    $strips += @{ Png = $png; Label = ("SCRUB {0}  {1}" -f $short, $cfg.Name) }
                }
            }
        }
        Write-Output ("done {0} / {1}" -f $short, $cfg.Name)
    }
}

Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

if ($strips.Count -eq 0) { Write-Output "no captures"; exit 1 }

# Lines 1-10 of the HUD: source/dst format, the dec/sws/paint costs, the
# presented rate, and the scrub and smooth lines.
$probe = [System.Drawing.Bitmap]::FromFile($strips[0].Png)
$top = [int]($probe.Height * 0.60)
$bandH = 175
$w = [int]($probe.Width * 0.70)
$probe.Dispose()

$lbl = 22
$dst = New-Object System.Drawing.Bitmap ($w * 2), ($strips.Count * ($bandH * 2 + $lbl))
$g = [System.Drawing.Graphics]::FromImage($dst)
$g.InterpolationMode = 'NearestNeighbor'
$g.Clear([System.Drawing.Color]::FromArgb(30, 0, 50))
$f = New-Object System.Drawing.Font('Consolas', 13, [System.Drawing.FontStyle]::Bold)
$i = 0
foreach ($s in $strips) {
    $src = [System.Drawing.Bitmap]::FromFile($s.Png)
    $ty = $i * ($bandH * 2 + $lbl)
    $g.DrawString($s.Label, $f, [System.Drawing.Brushes]::Yellow, 4, ($ty + 2))
    $g.DrawImage($src,
        (New-Object System.Drawing.Rectangle 0, ($ty + $lbl), ($w * 2), ($bandH * 2)),
        (New-Object System.Drawing.Rectangle 0, $top, $w, $bandH), 'Pixel')
    $src.Dispose(); $i++
}
$summary = Join-Path $OutDir "summary.png"
$dst.Save($summary, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $dst.Dispose()
Write-Output $summary
