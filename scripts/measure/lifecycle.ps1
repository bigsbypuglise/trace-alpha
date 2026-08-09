# Lifecycle gestures around a scrub: the transitions where the decoder changes
# hands. Throughput harnesses do not reach these, and they are where an
# ownership bug shows up as a hang, a stale frame or a wrong landing rather
# than as a bad number.
#
#   -StepCycle     release, then Right x5 / Left x5 three times. Must return to
#                  exactly the released frame. Stepping is never latest-wins.
#   -PlayAfter     release, then Space. Playback must take the decoder back and
#                  run normally.
#   -SwitchMedia   drag, and open another file WITHOUT releasing. A frame
#                  produced for the outgoing media must never be presented
#                  against the incoming one.
#   -KillMidDrag   close the window with the button still down and a decode in
#                  flight. Must not hang: cancellation is raised before the join.
#   -PlayThroughDrag  play, then drag and release WITHOUT pausing first.
#                  Playback must continue from the released frame: a scrub
#                  interrupts playback, it does not end it (step 5.6).
#   -PausedThroughDrag  the control for it. Same gesture with no play first;
#                  must stay paused. Run BOTH -- a check that can only ever
#                  report "moving" proves nothing on its own, and these two
#                  differ in exactly one thing: whether Play was pressed.
#
# The last two decide by comparing the picture across a second of wall time
# rather than by reading the HUD, because "is it still playing" is a question
# about motion and the frame counter would need OCR to answer.

param(
    [switch]$StepCycle,
    [switch]$PlayAfter,
    [switch]$SwitchMedia,
    [switch]$KillMidDrag,
    [switch]$PlayThroughDrag,
    [switch]$PausedThroughDrag,
    [switch]$KeyAtRelease,
    [switch]$WheelFirst,
    # Drag to the END of the clip rather than the middle. Releasing on the last
    # frame with playback intent set must leave it there: restarting the file
    # is the Play button's job, not the release's.
    [switch]$ToEnd,
    [string]$OtherClip,
    [int]$Cycles = 3,
    # -StepCycle captures the frame it landed on and the frame it ends on, in
    # one run. Comparing two separate runs does not work: the landing depends
    # on drag timing, so "before" and "after" would be different gestures.
    [string]$Out
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class L {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
  public const uint DOWN = 0x0002, UP = 0x0004, WHEEL = 0x0800;
}
"@

$p = Get-Process -Name Trace -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
$h = $p.MainWindowHandle
[L]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 400

$r = New-Object L+RECT
[L]::GetWindowRect($h, [ref]$r) | Out-Null
$winW = $r.R - $r.L; $winH = $r.B - $r.T

# Same groove scan as scrub.ps1: longest run of the UNFILLED track colour
# within the transport band. See that script for why both qualifiers matter.
$bmp = New-Object System.Drawing.Bitmap $winW, $winH
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$grooveY = -1; $x0 = -1; $x1 = -1; $best = 0
for ($y = [int]($winH * 0.25); $y -lt [int]($winH * 0.94); $y++) {
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
if ($best -lt 300) { Write-Output "groove not found"; exit 1 }

$sy = $r.T + $grooveY
$sxA = $r.L + $x0 + 6
$sxB = $r.L + $x1 - 6

function Sweep([int]$from, [int]$to, [double]$secs) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($true) {
    $t = $sw.Elapsed.TotalSeconds / $secs
    if ($t -ge 1.0) { break }
    [L]::SetCursorPos([int]($from + ($to - $from) * $t), $sy) | Out-Null
    $spin = [Diagnostics.Stopwatch]::StartNew()
    while ($spin.Elapsed.TotalMilliseconds -lt 4) { }
  }
  [L]::SetCursorPos($to, $sy) | Out-Null
}

# Samples the picture area on a sparse grid. Deliberately well above the
# transport and below the title bar, so neither the HUD text nor the slider
# handle can make a paused window look like a moving one.
function PictureSig {
    $rr = New-Object L+RECT
    [L]::GetWindowRect($h, [ref]$rr) | Out-Null
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

function Shot([string]$path) {
    if (-not $path) { return }
    $rr = New-Object L+RECT
    [L]::GetWindowRect($h, [ref]$rr) | Out-Null
    $b = New-Object System.Drawing.Bitmap ($rr.R - $rr.L), ($rr.B - $rr.T)
    $gg = [System.Drawing.Graphics]::FromImage($b)
    $gg.CopyFromScreen($rr.L, $rr.T, 0, 0, $b.Size)
    $b.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $gg.Dispose(); $b.Dispose()
    Write-Output "saved $path"
}

function SigDiff($a, $b) {
    if ($a.Count -ne $b.Count -or $a.Count -eq 0) { return -1 }
    $moved = 0
    for ($i = 0; $i -lt $a.Count; $i++) {
        if ([Math]::Abs($a[$i] - $b[$i]) -gt 12) { $moved++ }
    }
    return [Math]::Round(100.0 * $moved / $a.Count, 1)
}

Add-Type -AssemblyName System.Windows.Forms
if ($PlayThroughDrag -or $PausedThroughDrag) {
    # PRECONDITION: the app must be paused. Both gestures start by toggling or
    # not toggling play, so inheriting a playing app inverts the expected
    # outcome -- and it inverts BOTH gestures, which reads exactly like a
    # product regression. Running the pair against one instance without a
    # restart in between did precisely that once, and passed by luck the time
    # before, so this is checked rather than assumed.
    $a = PictureSig
    Start-Sleep -Milliseconds 600
    $b = PictureSig
    if ((SigDiff $a $b) -ge 5.0) {
        Write-Output "PRECONDITION FAIL: the app is already playing. Run restart.ps1 before each through-drag gesture; they are not composable on one instance."
        exit 1
    }
}

if ($PlayThroughDrag) {
    # Playing when the drag begins. This is the whole point of the gesture:
    # before step 5.6 the press ended the run and the release never restored it.
    [System.Windows.Forms.SendKeys]::SendWait(" ")
    Start-Sleep -Milliseconds 1500
}

if ($WheelFirst) {
    # A wheel notch over the groove steps the slider with no press and no
    # release, so it is the one route into the scrub lambdas that is not part
    # of a drag. It is a stepping gesture and must end the playback intent --
    # otherwise the intent outlives it and some later, unrelated drag resurrects
    # playback. Combined with -PlayThroughDrag the expectation therefore
    # inverts: playing, wheel, drag, release => paused.
    [L]::SetCursorPos([int](($sxA + $sxB) / 2), $sy) | Out-Null
    Start-Sleep -Milliseconds 150
    [L]::mouse_event([L]::WHEEL, 0, 0, 120, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 600
}

# Drag to roughly the middle of the clip, so stepping has room either side.
$mid = if ($ToEnd) { $sxB } else { [int](($sxA + $sxB) / 2) }
[L]::SetCursorPos($sxA, $sy) | Out-Null
Start-Sleep -Milliseconds 200
[L]::mouse_event([L]::DOWN, 0, 0, 0, [IntPtr]::Zero)
Sweep $sxA $mid 0.8

if ($KillMidDrag) {
    # Button still down, decode in flight. If cancellation is not raised before
    # the join this hangs here rather than exiting.
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p.CloseMainWindow() | Out-Null
    $exited = $p.WaitForExit(8000)
    [L]::mouse_event([L]::UP, 0, 0, 0, [IntPtr]::Zero)
    if (-not $exited) {
        Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force
        Write-Output "KILL-MID-DRAG: FAIL - did not exit within 8s (HUNG)"
        exit 1
    }
    Write-Output ("KILL-MID-DRAG: PASS - exited in {0} ms" -f [int]$sw.Elapsed.TotalMilliseconds)
    exit 0
}

if ($SwitchMedia) {
    if (-not $OtherClip) { Write-Output "need -OtherClip"; exit 1 }
    # Open another file with the button STILL DOWN, so the outgoing media has a
    # request in flight when the decoder changes underneath it.
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.SendKeys]::SendWait("^o")
    Start-Sleep -Milliseconds 1200
    [System.Windows.Forms.SendKeys]::SendWait($OtherClip.Replace("(", "{(}").Replace(")", "{)}"))
    Start-Sleep -Milliseconds 400
    [System.Windows.Forms.SendKeys]::SendWait("{ENTER}")
    Start-Sleep -Milliseconds 2500
    [L]::mouse_event([L]::UP, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 800
    $alive = -not $p.HasExited
    Write-Output ("SWITCH-MEDIA: {0}" -f $(if ($alive) { "PASS - still running, capture to check which file is loaded" } else { "FAIL - exited" }))
    exit 0
}

[L]::mouse_event([L]::UP, 0, 0, 0, [IntPtr]::Zero)

if ($PlayThroughDrag -or $PausedThroughDrag) {
    if ($KeyAtRelease) {
        # Space with no settle at all, so it is delivered while the release is
        # still resolving its exact frame -- on 4K H.264 that landing blocks the
        # UI thread for 90-125ms, which is the window this is aiming at. The
        # expected outcome is INVERTED from the plain gesture: whichever
        # transport command came last must win.
        [System.Windows.Forms.SendKeys]::SendWait(" ")
    }
    # Long enough for the release to land its exact frame -- on 4K H.264 that
    # is a seek plus a GOP walk, 90-125ms -- and short enough that the first
    # sample is still near the landing.
    Start-Sleep -Milliseconds 350
    $a = PictureSig
    # Captured WHILE the resumed run is going, which is the only moment the HUD
    # can be asked whether the first frames after the landing are full-res and
    # where the audio clock restarted. A shot taken after pausing cannot.
    if ($Out) { Shot ($Out -replace '\.png$', '_resumed.png') }
    Start-Sleep -Milliseconds 1100
    $b = PictureSig
    $moved = SigDiff $a $b
    $label = if ($PlayThroughDrag) { "PLAY-THROUGH-DRAG" } else { "PAUSED-THROUGH-DRAG" }
    if ($KeyAtRelease) { $label += " +KEY" }
    if ($WheelFirst) { $label += " +WHEEL" }
    if ($ToEnd) { $label += " +TOEND" }
    # Space at the release flips which outcome is correct: playing then paused
    # must end frozen, paused then played must end moving. A wheel notch before
    # the drag flips it the same way, by clearing the intent.
    $wantMoving = [bool]$PlayThroughDrag
    if ($KeyAtRelease) { $wantMoving = -not $wantMoving }
    if ($WheelFirst) { $wantMoving = $false }
    # Released on the last frame: there is nowhere to advance to, so the picture
    # must be still whatever the intent said. A moving picture here means the
    # file silently restarted.
    if ($ToEnd) { $wantMoving = $false }
    # A still frame samples identically; anything actually advancing moves a
    # large fraction of the grid. The two outcomes are far apart, so the
    # threshold is not a tuned number.
    $verdict = if ($wantMoving) {
        if ($moved -ge 5.0) { "PASS - picture advancing ({0}% of samples moved)" -f $moved }
        else { "FAIL - picture frozen ({0}%), expected it to be playing" -f $moved }
    } else {
        if ($moved -lt 1.0) { "PASS - stayed still ({0}% moved)" -f $moved }
        else { "FAIL - playing ({0}% moved), expected it to be paused" -f $moved }
    }
    Write-Output "${label}: $verdict"
    # Leave the app paused so a follow-up capture reads a settled HUD.
    if ($wantMoving) { [System.Windows.Forms.SendKeys]::SendWait(" ") ; Start-Sleep -Milliseconds 400 }
    exit 0
}

Start-Sleep -Milliseconds 900

Add-Type -AssemblyName System.Windows.Forms
if ($StepCycle) {
    if ($Out) { Shot ($Out -replace '\.png$', '_landed.png') }
    # Right x5 then Left x5 must be a no-op. Repeated, because a single cycle
    # can pass on a decoder that is only accidentally in the right place.
    for ($i = 0; $i -lt $Cycles; $i++) {
        for ($k = 0; $k -lt 5; $k++) { [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}"); Start-Sleep -Milliseconds 120 }
        for ($k = 0; $k -lt 5; $k++) { [System.Windows.Forms.SendKeys]::SendWait("{LEFT}");  Start-Sleep -Milliseconds 120 }
    }
    Write-Output "STEP-CYCLE: $Cycles x (Right x5 / Left x5) sent"
    if ($Out) { Shot ($Out -replace '\.png$', '_stepped.png') }
}

if ($PlayAfter) {
    [System.Windows.Forms.SendKeys]::SendWait(" ")
    Start-Sleep -Seconds 4
    [System.Windows.Forms.SendKeys]::SendWait(" ")
    Start-Sleep -Milliseconds 400
    Write-Output "PLAY-AFTER: played 4s from the released frame"
}

Write-Output "done"
