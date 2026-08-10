# Continuous REVERSE playback (J), driven and measured.
#
# There was no reverse harness in this repo until now: every reverse figure
# before 2026-08-10 was taken by hand, and the ones taken between GATE E and
# that date were measuring the J-K-L scheduler fault (plan section 29.2) rather
# than reverse playback. This script exists so a reverse number can be produced
# the same way twice.
#
# What it does, in order:
#   1. finds the timeline groove by its unfilled track colour, as scrub.ps1 does
#   2. CLICKS near the end of the groove, so reverse has somewhere to run from
#      (a click is a jump and lands exactly; a drag would shuttle there)
#   3. presses J (or L with -Forward) -Presses times, walking the 1x/2x/5x/10x/30x
#      ladder
#   4. either holds for -HoldSeconds, or with -Traverse waits until the picture
#      stops changing, which is reverse arriving at frame 0
#   5. presses K to stop
#   6. with -StepCheck, steps Right then Left and compares the picture with the
#      frame reverse stopped on -- the landing-exactness gesture, run without
#      needing to read a frame number off the HUD
#
# -Traverse is the speed measurement. Elapsed time for a known number of frames
# is an ACHIEVED reverse rate, which is the thing the owner's 2x/5x/10x/30x
# ladder is a request for; the presented-rate field on the HUD cannot answer it
# because presenting every frame slowly and sampling frames quickly read the
# same there.
#
# Notes that cost time if rediscovered:
#   - restart.ps1 FIRST. The groove scan looks for the *unfilled* track, which
#     does not exist once the playhead is at the end -- and this script then
#     clicks near the end, so the second run in a row would fail to find it.
#   - J is silent by design (audio is 1x forward only), so nothing here needs
#     TRACE_NO_AUDIO to be comparable across files.
#   - the ladder is 1x/2x/5x/10x/30x and caps at 30x, in both directions.
#   - FORWARD AT 1x IS NOT A SHUTTLE RUN. It is ordinary audio-mastered
#     playback, deliberately untouched, so `shuttle idle` on a -Forward -Presses 1
#     capture is correct rather than a failure to engage.

param(
    # Drive L (fast-forward) instead of J (rewind). Same gesture, mirrored: it
    # starts near the HEAD of the clip so forward has somewhere to run.
    [switch]$Forward,
    [ValidateRange(1, 8)][int]$Presses = 1,
    [double]$HoldSeconds = 8.0,
    # Wait for reverse to arrive at frame 0 instead of holding a fixed time.
    [switch]$Traverse,
    [double]$TraverseTimeout = 30.0,
    # Where on the groove to start the run, as a fraction of its length.
    # Defaults to 0.95 for rewind and 0.05 for fast-forward.
    [double]$StartFraction = -1,
    # Step +1 then -1 after stopping and report how far the picture moved. A
    # landing that is exact returns to the same picture; anything else does not.
    [switch]$StepCheck,
    [string]$Out
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class R {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
  public const uint DOWN = 0x0002, UP = 0x0004;
}
"@

$p = Get-Process -Name Trace -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
$h = $p.MainWindowHandle
[R]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 400

$rect = New-Object R+RECT
[R]::GetWindowRect($h, [ref]$rect) | Out-Null
$winW = $rect.R - $rect.L; $winH = $rect.B - $rect.T
Write-Output "win ${winW}x${winH}"

# Groove scan: transport band only, longest run wins. Window chrome carries
# short runs of similar greys and a first-match scan latches onto the title bar.
$bmp = New-Object System.Drawing.Bitmap $winW, $winH
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($rect.L, $rect.T, 0, 0, $bmp.Size)
$grooveY = -1; $x0 = -1; $x1 = -1; $best = 0
$yFrom = [int]($winH * 0.25); $yTo = [int]($winH * 0.94)
for ($y = $yFrom; $y -lt $yTo; $y++) {
  $run = 0; $start = -1
  for ($x = 0; $x -lt $winW; $x++) {
    $c = $bmp.GetPixel($x, $y)
    if ([Math]::Abs($c.R - 55) -le 6 -and [Math]::Abs($c.G - 55) -le 6 -and [Math]::Abs($c.B - 55) -le 6) {
      if ($start -lt 0) { $start = $x }
      $run++
      if ($run -gt $best) { $best = $run; $grooveY = $y; $x0 = $start; $x1 = $x }
    } else { $run = 0; $start = -1 }
  }
}
$g.Dispose(); $bmp.Dispose()
if ($best -lt 300) { Write-Output "groove not found (longest run $best) - restart.ps1 first"; exit 1 }
Write-Output "groove y=$grooveY x=$x0..$x1"

$sy = $rect.T + $grooveY
$sxA = $rect.L + $x0 + 6
$sxB = $rect.L + $x1 - 6
if ($StartFraction -lt 0) { $StartFraction = if ($Forward) { 0.05 } else { 0.95 } }
$startX = [int]($sxA + ($sxB - $sxA) * $StartFraction)

function PictureSig {
    $rr = New-Object R+RECT
    [R]::GetWindowRect($h, [ref]$rr) | Out-Null
    $w = $rr.R - $rr.L; $ht = $rr.B - $rr.T
    $b = New-Object System.Drawing.Bitmap $w, $ht
    $gg = [System.Drawing.Graphics]::FromImage($b)
    $gg.CopyFromScreen($rr.L, $rr.T, 0, 0, $b.Size)
    $vals = New-Object System.Collections.ArrayList
    for ($y = [int]($ht * 0.15); $y -lt [int]($ht * 0.60); $y += 11) {
        for ($x = [int]($w * 0.10); $x -lt [int]($w * 0.90); $x += 11) {
            $c = $b.GetPixel($x, $y)
            [void]$vals.Add($c.R + $c.G + $c.B)
        }
    }
    $gg.Dispose(); $b.Dispose()
    return $vals
}

function SigDiff($a, $b) {
    if ($a.Count -ne $b.Count -or $a.Count -eq 0) { return -1 }
    $moved = 0
    for ($i = 0; $i -lt $a.Count; $i++) {
        if ([Math]::Abs($a[$i] - $b[$i]) -gt 12) { $moved++ }
    }
    return [Math]::Round(100.0 * $moved / $a.Count, 1)
}

# Position the playhead with a click, not a drag: a click is a jump and lands
# exactly, a drag would shuttle every frame on the way there and leave the
# cache full of the frames the run is about to ask for.
[R]::SetCursorPos($startX, $sy) | Out-Null
Start-Sleep -Milliseconds 200
[R]::mouse_event([R]::DOWN, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 120
[R]::mouse_event([R]::UP, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 900
Write-Output "positioned at fraction $StartFraction"

[R]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 200

# The ladder is 1x/2x/5x/10x/30x in both directions, and it is a ladder of
# SAMPLING STRIDES rather than of tick rates -- one frame is presented per
# source-frame period at every speed and the speed decides which frames.
$speeds = @(1, 2, 5, 10, 30, 30, 30, 30)
$key = if ($Forward) { "l" } else { "j" }
$sign = if ($Forward) { "+" } else { "-" }
Write-Output ("pressing {0} x{1} -> expect {2}{3}x" -f $key.ToUpper(), $Presses, $sign, $speeds[$Presses - 1])

$sw = [Diagnostics.Stopwatch]::StartNew()
for ($i = 0; $i -lt $Presses; $i++) {
    [System.Windows.Forms.SendKeys]::SendWait($key)
    Start-Sleep -Milliseconds 60
}

if ($Traverse) {
    # Reverse stops when it reaches frame 0. Detect it as the picture going
    # still: three consecutive quiet samples, so a single slow frame during a
    # GOP walk is not mistaken for the end.
    $prev = PictureSig
    $quiet = 0
    $stoppedAt = -1.0
    while ($sw.Elapsed.TotalSeconds -lt $TraverseTimeout) {
        Start-Sleep -Milliseconds 120
        $now = PictureSig
        $d = SigDiff $prev $now
        $prev = $now
        if ($d -lt 1.0) { $quiet++ } else { $quiet = 0 }
        if ($quiet -ge 3) { $stoppedAt = $sw.Elapsed.TotalSeconds - 0.36; break }
    }
    if ($stoppedAt -lt 0) {
        Write-Output ("traverse DID NOT FINISH within {0}s" -f $TraverseTimeout)
    } else {
        Write-Output ("traverse reached the head in {0:N2}s" -f $stoppedAt)
    }
} else {
    Start-Sleep -Seconds $HoldSeconds
    Write-Output ("held {0}s" -f $HoldSeconds)
}

function Shot([string]$path) {
    if (-not $path) { return }
    $rr = New-Object R+RECT
    [R]::GetWindowRect($h, [ref]$rr) | Out-Null
    $b = New-Object System.Drawing.Bitmap ($rr.R - $rr.L), ($rr.B - $rr.T)
    $gg = [System.Drawing.Graphics]::FromImage($b)
    $gg.CopyFromScreen($rr.L, $rr.T, 0, 0, $b.Size)
    $b.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $gg.Dispose(); $b.Dispose()
    Write-Output "saved $path"
}

# Captured BEFORE K. The cadence and handler counters are cumulative and
# survive the stop, but `speed` does not -- and a run whose speed cannot be
# confirmed from its own capture is a run that cannot be quoted.
Shot $Out

[System.Windows.Forms.SendKeys]::SendWait("k")
Start-Sleep -Milliseconds 700
Write-Output "stopped (K)"

if ($StepCheck) {
    $landed = PictureSig
    [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}")
    Start-Sleep -Milliseconds 500
    $fwd = PictureSig
    [System.Windows.Forms.SendKeys]::SendWait("{LEFT}")
    Start-Sleep -Milliseconds 500
    $back = PictureSig
    $moveOut = SigDiff $landed $fwd
    $moveBack = SigDiff $landed $back
    Write-Output ("stepcheck: +1 moved {0}%, -1 returned to {1}% difference" -f $moveOut, $moveBack)
    if ($moveBack -lt 1.0) { Write-Output "stepcheck PASS - returned to the landed frame" }
    else { Write-Output "stepcheck FAIL - did not return to the landed frame" }
    if ($moveOut -lt 1.0) { Write-Output "stepcheck INCONCLUSIVE - +1 did not move the picture (static shot?)" }
}
