# CADENCE FOR THE IMAGE-SEQUENCE PATH -- the instrument this project did not have.
#
# Until 2026-08-24 no EXR playback rate had ever been measured here. The counters
# were being accumulated (the sequence path runs the SAME playback tick and the
# SAME GATE E deadline scheduler as video) but refreshHud() built the three
# cadence lines inside its VideoFile branch only, so they were never displayed.
# The consequence is the one this project keeps paying for: "EXR playback is
# fine" and "EXR playback has never been measured" were indistinguishable, the
# same shape as `stalls 0 of 0` hiding the title-bar stall for a dozen runs.
#
# READ THE PRESENTED LINE AND THE MEDIA FIGURE TOGETHER, ALWAYS:
#
#   `presented N / 24.00 fps nominal (P% real time)`  is how much PICTURE arrived
#   `skip N (ticks M max R, media Q%)`                is whether the PLAYHEAD held
#
# They are different questions and on this path they give very different answers.
# A previous session read the playhead reaching frame 61 in 2.5s and recorded
# that sequence playback "keeps real time"; the playhead advances by SKIPPING, so
# that figure was `media`, not `presented`. Quoting one for the other is the
# specific mistake this harness exists to make impossible.
#
# `fps nominal` is not decoration. An image sequence has no container rate --
# ImageSequenceFrameSource::fps() returns Trace's own 24.0 and fpsRational()
# returns false -- so the denominator of "% of real time" is Trace's assumption.
#
# WHY NOT cadence.ps1: it crops a FIXED 86px band 330px up from the bottom,
# which is tuned to the video HUD's line count. A sequence HUD has a different
# one, so that band lands on the picture. This anchors at the bottom of the
# window and takes a generous slice instead of predicting an offset -- the same
# lesson passkeys.ps1 records for its own HUD band.
#
# The verdict is the owner's eye on the stitched PNG, deliberately: these are
# figures to read, not a threshold to pass. There is no recorded baseline to
# compare against yet, which is the whole reason for running it.

param(
    # One or more sequences. Pass ANY frame of each; Trace detects the run.
    [Parameter(Mandatory = $true)][string[]]$Clip,
    [int]$Seconds = 14,
    [int]$Repeats = 2,
    # NAME=VALUE pairs forwarded to restart.ps1. TRACE_HUD=1 and the scratch INI
    # are added here and must not be passed again.
    [string[]]$Env = @(),
    [string]$OutDir = "$env:TEMP\trace_seqcadence",
    [int]$BandHeight = 190,
    # Which binary. Empty = build\app\Release, the shipping one.
    [string]$Exe = ""
)

Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $OutDir | Out-Null

# A SCRATCH SETTINGS FILE, for cadence.ps1's reason and it is not optional:
# Loop is persisted, and a wrap re-establishes the playback timeline, which
# zeroes `presented`, `frames`, the histogram and `handler>budget`. A run on a
# machine where Loop was left on reports the LAST LAP while every rate figure
# reads healthy. The HUD's `loop` field is the check if a figure is questioned.
$scratchIni = Join-Path $OutDir "seqcadence-scratch.ini"
Remove-Item $scratchIni -ErrorAction SilentlyContinue
$envAll = @($Env) + @("TRACE_HUD=1", "TRACE_SETTINGS_FILE=$scratchIni")

$shots = @()
foreach ($c in $Clip) {
    if (-not (Test-Path $c)) { Write-Warning "seqcadence: missing $c"; continue }
    $label = Split-Path (Split-Path $c -Parent) -Leaf
    for ($r = 1; $r -le $Repeats; $r++) {
        $restartArgs = @{ Clip = $c; Env = $envAll; SettleSeconds = 6 }
        if ($Exe) { $restartArgs.Exe = $Exe }
        & "$PSScriptRoot\restart.ps1" @restartArgs | Out-Null
        # play.ps1 asserts the picture actually advanced and reports failure on
        # the WARNING stream; every caller pipes success to Out-Null, so the one
        # message saying the figure is void would otherwise be swallowed. A run
        # that never ticked reports `frames 0` and stitches in looking like data.
        & "$PSScriptRoot\play.ps1" -Seconds $Seconds | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Warning "seqcadence: playback did not start for $label; aborting"; exit 1 }
        $png = Join-Path $OutDir ("{0}_r{1}.png" -f ($label -replace '[^\w\-]', '_'), $r)
        & "$PSScriptRoot\capture.ps1" -Out $png | Out-Null
        $tag = if ($Env.Count) { ($Env -join ' ') } else { "(defaults)" }
        $shots += @{ Png = $png; Label = ("{0}  rep{1}  {2}" -f $label, $r, $tag) }
        Write-Output ("captured {0} rep {1}" -f $label, $r)
    }
}
Get-Process -Name Trace -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

if ($shots.Count -eq 0) { Write-Warning "seqcadence: nothing captured"; exit 1 }

# Anchored at the BOTTOM of the window, not at a predicted offset from it.
$probe = [System.Drawing.Bitmap]::FromFile($shots[0].Png)
$w0 = $probe.Width
$probe.Dispose()

$lbl = 22
$dst = New-Object System.Drawing.Bitmap ($w0 * 2), ($shots.Count * ($BandHeight * 2 + $lbl))
$g = [System.Drawing.Graphics]::FromImage($dst)
$g.InterpolationMode = 'NearestNeighbor'
$g.Clear([System.Drawing.Color]::FromArgb(30, 0, 50))
$f = New-Object System.Drawing.Font('Consolas', 14, [System.Drawing.FontStyle]::Bold)
$i = 0
foreach ($s in $shots) {
    $src = [System.Drawing.Bitmap]::FromFile($s.Png)
    $top = [Math]::Max(0, $src.Height - $BandHeight)
    $ty = $i * ($BandHeight * 2 + $lbl)
    $g.DrawString($s.Label, $f, [System.Drawing.Brushes]::Yellow, 4, ($ty + 2))
    $g.DrawImage($src,
        (New-Object System.Drawing.Rectangle 0, ($ty + $lbl), ($w0 * 2), ($BandHeight * 2)),
        (New-Object System.Drawing.Rectangle 0, $top, $src.Width, $BandHeight), 'Pixel')
    $src.Dispose(); $i++
}
$out = Join-Path $OutDir "summary.png"
$dst.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $dst.Dispose()
Write-Output $out
