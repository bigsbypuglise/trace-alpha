# Which of two unrelated faults is a file's playback stutter?
#
# The presented rate reads 98-99% under both and therefore cannot say, which is
# how ProRes 4444 can measure 99.6% of real time and still visibly stutter.
#
#   CAUSE A -- the integer tick beat. floor(1000/fps) is 41ms against a 41.667ms
#   frame, so the wall-clock accumulator falls 0.667ms short per frame and every
#   ~62nd frame needs two ticks: one doubled (~83ms) frame every ~2.6s, REGULARLY
#   spaced, on every file. Presentation-clock problem; GATE E owns it.
#
#   CAUSE B -- per-frame cost overrun. Playback decodes synchronously with no
#   read-ahead, so a frame over budget is late immediately. Ragged, irregular,
#   specific to expensive files. GATE E does NOT fix it -- a presentation clock
#   supplies phase, not headroom.
#
# The HUD `cadence` line separates them: A piles up in the 1.5-2.5x bucket with a
# TIGHT long-gap min/med/max, and leaves `handler>budget` at 0. B smears across
# buckets, scatters the spacing, and drives handler>budget above 0.
#
# THE CONTROL NEEDS TRACE_NO_AUDIO=1 AND THIS IS NOT OPTIONAL. When a file has an
# audio track the audio clock becomes the only scheduler and the accumulator is
# bypassed (cd79d49), so cause A cannot occur. ProRes 4444 has no audio track;
# 4K ProRes 422 HQ and the 1080p clip both do. Comparing them as shipped compares
# two different schedulers and would "prove" that 4444 is uniquely bad when the
# only difference is which clock is running. TRACE_NO_AUDIO puts all three on the
# wall clock, which is the only way the comparison means anything.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$Label = "",
    [int]$Seconds = 11,
    [int]$Repeats = 2,
    # NAME=VALUE pairs passed to restart.ps1, e.g. TRACE_RENDERER=d3d11
    [string[]]$Env = @(),
    [string]$OutDir = "$env:TEMP\trace_cadence"
)

Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $OutDir | Out-Null
if (-not $Label) { $Label = [System.IO.Path]::GetFileNameWithoutExtension($Clip) }
if ($Label.Length -gt 26) { $Label = $Label.Substring(0, 26) }

# A SCRATCH SETTINGS FILE, BECAUSE A PERSISTED PREFERENCE CAN SILENTLY REWRITE
# WHAT THIS MEASURES. Spec phase 14 added Loop, persisted through
# trace::app::settings() -- and a looping run RE-ESTABLISHES the playback
# timeline at every wrap, which zeroes `frames`, `presented`, the cadence
# histogram and `handler>budget` along with it. A cadence run on a machine where
# Loop had been left on therefore reports the LAST LAP: measured, before this
# existed, as `frames 30 | elapsed 1.25s` on a clip whose recorded baseline is
# 119 frames, with every rate figure reading a healthy 100%.
#
# Nothing about that is visible in the figures being compared -- which is the
# whole shape of the stalls-versus-refresh mistake -- so the fix is to stop the
# run inheriting the machine at all, the way recentfiles.ps1 already does. The
# defaults a fresh INI produces (Loop off, aspect lock on) are the ones every
# recorded baseline was taken under.
#
# The HUD's `loop ON wraps N` field is the check on this: if a cadence figure is
# ever questioned, read it before anything else.
$scratchIni = Join-Path $OutDir "cadence-scratch.ini"
Remove-Item $scratchIni -ErrorAction SilentlyContinue
$Env = @($Env) + @("TRACE_SETTINGS_FILE=$scratchIni")

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

# The presented/sched/cadence block, anchored from the bottom because the HUD
# sits under a video area whose height varies with the window and the media.
$probe = [System.Drawing.Bitmap]::FromFile($shots[0].Png)
$fromBottom = 330
$bandH = 86
$w0 = [int]($probe.Width * 0.86)
$probe.Dispose()

$lbl = 20
$dst = New-Object System.Drawing.Bitmap ($w0 * 2), ($shots.Count * ($bandH * 2 + $lbl))
$g = [System.Drawing.Graphics]::FromImage($dst)
$g.InterpolationMode = 'NearestNeighbor'
$g.Clear([System.Drawing.Color]::FromArgb(30, 0, 50))
$f = New-Object System.Drawing.Font('Consolas', 13, [System.Drawing.FontStyle]::Bold)
$i = 0
foreach ($s in $shots) {
    $src = [System.Drawing.Bitmap]::FromFile($s.Png)
    $top = [Math]::Max(0, $src.Height - $fromBottom)
    $ty = $i * ($bandH * 2 + $lbl)
    $g.DrawString($s.Label, $f, [System.Drawing.Brushes]::Yellow, 4, ($ty + 2))
    $g.DrawImage($src,
        (New-Object System.Drawing.Rectangle 0, ($ty + $lbl), ($w0 * 2), ($bandH * 2)),
        (New-Object System.Drawing.Rectangle 0, $top, $w0, $bandH), 'Pixel')
    $src.Dispose(); $i++
}
$out = Join-Path $OutDir ("summary_{0}.png" -f ($Label -replace '[^\w\-]', '_'))
$dst.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $dst.Dispose()
Write-Output $out
