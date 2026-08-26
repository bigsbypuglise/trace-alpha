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
    # PASS-CYCLING LEG. Press `]` this many times after the file opens and
    # before playback starts, so the run measures a DIFFERENT channel span.
    # Selecting a pass changes which channels loadExr reads, and the standalone
    # read costs are NOT flat across them (exrprobe: root 35.35ms, P 34.96,
    # SpecularLighting 45.41 -- a ~10ms spread from block traversal). Whether
    # that spread reaches playback is what this leg exists to answer.
    [int]$PassAdvance = 0,
    # The key to send, so the recorded SendKeys trap can be reproduced
    # deliberately as a negative control: ']' unescaped is SWALLOWED.
    [string]$PassKey = '{]}',
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

# ---- PASS SELECTION, AND IT MUST BE ABLE TO FAIL ----------------------------
#
# A run whose keypress went nowhere reports every pass with identical figures,
# which reads as "the pass makes no difference" -- the flattering answer, and
# the class of wrong result this project has paid for repeatedly. So the press
# is ASSERTED rather than assumed: the HUD band is captured either side of it
# and the two are compared.
#
# Taken while PAUSED, which is what makes the comparison clean. Nothing ticks,
# so the three cadence lines are frozen and the only things that can move in
# the band are the media line (`pass N/M <name> ... map ...`) and the transport
# line's action label. A zero reading therefore means the key did not land --
# it can never mean "the app was merely idle".
. "$PSScriptRoot\tracewindow.ps1"

if (-not ("SeqPass" -as [type])) {
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class SeqPass {
  [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint x,uint y,uint d,UIntPtr e);
}
"@
}
Add-Type -AssemblyName System.Windows.Forms

function Grab-HudBand([IntPtr]$h, [int]$rows, [string]$png) {
    $r = New-Object TraceWin+RECT
    [TraceWin]::GetWindowRect($h, [ref]$r) | Out-Null
    # 16px in from each side: Windows 11's invisible resize border puts whatever
    # is BEHIND Trace into the first columns of a GetWindowRect capture. The
    # recorded transitions.ps1 trap, re-hit by two later harnesses.
    $w = ($r.R - $r.L) - 32
    if ($w -le 0 -or $rows -le 0) { return $null }
    $bmp = New-Object System.Drawing.Bitmap $w, $rows
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L + 16, $r.B - $rows - 10, 0, 0, $bmp.Size)
    $g.Dispose()
    if ($png) { $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png) }
    return $bmp
}

function Band-Diff($a, $b) {
    if ($null -eq $a -or $null -eq $b) { return -1.0 }
    if ($a.Width -ne $b.Width -or $a.Height -ne $b.Height) { return 100.0 }
    $d = 0; $t = 0
    for ($y = 0; $y -lt $a.Height; $y += 2) {
        for ($x = 0; $x -lt $a.Width; $x += 2) {
            $t++
            $ca = $a.GetPixel($x, $y); $cb = $b.GetPixel($x, $y)
            if ([Math]::Abs($ca.R - $cb.R) -gt 24 -or
                [Math]::Abs($ca.G - $cb.G) -gt 24 -or
                [Math]::Abs($ca.B - $cb.B) -gt 24) { $d++ }
        }
    }
    if ($t -eq 0) { return 0.0 }
    return [Math]::Round(100.0 * $d / $t, 3)
}

# Returns the pre/post band difference, or -1 if the window could not be driven.
function Select-Pass([int]$advance, [string]$tagBase) {
    $h = Resolve-TraceWindow
    if ($h -eq [IntPtr]::Zero) {
        Write-Warning "seqcadence: no Trace window to select a pass in"; return -1.0
    }
    if (-not (Focus-TraceWindow -Handle $h)) {
        Write-Warning "seqcadence: foreground DENIED -- the keys would go elsewhere"; return -1.0
    }

    # Click-activate on the picture (feedback item 13), aimed a third of the way
    # down so it lands on picture and never on the transport strip.
    $r = New-Object TraceWin+RECT
    [TraceWin]::GetWindowRect($h, [ref]$r) | Out-Null
    [TraceWin]::SetCursorPos([int](($r.L + $r.R) / 2), [int]($r.T + ($r.B - $r.T) / 3)) | Out-Null
    Start-Sleep -Milliseconds 200
    [SeqPass]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)
    [SeqPass]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 400

    $pre = Grab-HudBand $h $BandHeight (Join-Path $OutDir ($tagBase + "_pre.png"))
    for ($k = 1; $k -le $advance; $k++) {
        [System.Windows.Forms.SendKeys]::SendWait($PassKey)
        Start-Sleep -Milliseconds 700
    }
    $post = Grab-HudBand $h $BandHeight (Join-Path $OutDir ($tagBase + "_post.png"))
    $pct = Band-Diff $pre $post
    if ($pre)  { $pre.Dispose() }
    if ($post) { $post.Dispose() }
    return $pct
}

# TWO THRESHOLDS WITH A GAP, never one cutoff -- passkeys.ps1's recorded lesson.
# A reading BETWEEN them means the instrument cannot tell, and the run says so
# rather than rounding toward the answer it expected.
$kPassSame  = 0.50
$kPassMoved = 2.00

$shots = @()
foreach ($c in $Clip) {
    if (-not (Test-Path $c)) { Write-Warning "seqcadence: missing $c"; continue }
    $label = Split-Path (Split-Path $c -Parent) -Leaf
    for ($r = 1; $r -le $Repeats; $r++) {
        $restartArgs = @{ Clip = $c; Env = $envAll; SettleSeconds = 6 }
        if ($Exe) { $restartArgs.Exe = $Exe }
        & "$PSScriptRoot\restart.ps1" @restartArgs | Out-Null

        # SELECT THE PASS BEFORE PLAYBACK STARTS, never during it: applyExrPass()
        # clears the frame cache, so cycling mid-run would fold a cache refill
        # into the cadence figure and measure the wrong thing.
        $passNote = ""
        if ($PassAdvance -gt 0) {
            $pd = Select-Pass $PassAdvance ("{0}_r{1}_pass" -f ($label -replace '[^A-Za-z0-9_\-]', '_'), $r)
            $passNote = ("pass+{0} (band {1}%)" -f $PassAdvance, $pd)
            Write-Output ("  pass advance +{0}  band moved {1}%" -f $PassAdvance, $pd)
            if ($pd -lt 0) {
                Write-Warning "seqcadence: could not drive the window; the figure would be void"
                exit 1
            }
            if ($pd -le $kPassSame) {
                Write-Warning ("seqcadence: THE PASS DID NOT CHANGE (band {0}% <= {1}%). Either the key was swallowed or the action is disabled on this media. Whatever follows is the OPENING pass under a different label." -f $pd, $kPassSame)
                $passNote += " NO-CHANGE"
            } elseif ($pd -lt $kPassMoved) {
                Write-Warning ("seqcadence: band moved {0}%, between the thresholds ({1} / {2}). The instrument cannot tell; this run is inconclusive." -f $pd, $kPassSame, $kPassMoved)
                $passNote += " AMBIGUOUS"
            }
        }
        # play.ps1 asserts the picture actually advanced and reports failure on
        # the WARNING stream; every caller pipes success to Out-Null, so the one
        # message saying the figure is void would otherwise be swallowed. A run
        # that never ticked reports `frames 0` and stitches in looking like data.
        & "$PSScriptRoot\play.ps1" -Seconds $Seconds | Out-Null
        if ($LASTEXITCODE -ne 0) { Write-Warning "seqcadence: playback did not start for $label; aborting"; exit 1 }
        $png = Join-Path $OutDir ("{0}_r{1}.png" -f ($label -replace '[^\w\-]', '_'), $r)
        & "$PSScriptRoot\capture.ps1" -Out $png | Out-Null
        $tag = if ($Env.Count) { ($Env -join ' ') } else { "(defaults)" }
        if ($passNote) { $tag = "$tag  $passNote" }
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
