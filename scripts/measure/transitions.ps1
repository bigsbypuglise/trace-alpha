# Every boundary of a shuttle RUN, exercised one at a time.
#
# Supersedes revtransitions.ps1, which enumerated six ways OUT of a reverse run.
# That framing was re-derived at spec phase 4 rather than extended, because the
# thing it enumerated stopped being the thing that matters:
#
#   - it was reverse-only, and the forward shuttle has been a queued, strided
#     run with a decoder lease since dd21fe9 -- the same shape as reverse, and
#     the same failure modes;
#   - every one of its six exits was a KEY or the slider, which is exactly how
#     the frame-step button went unlisted and held a real bug (phase 3 record);
#   - and at phase 4 the forward button became a shuttle ENTRY, so "exits" is no
#     longer the right axis at all. A press that starts a run ends the previous
#     one in the same call.
#
# So the axis is a RUN BOUNDARY: any command that starts a run, ends one, or
# changes its direction or speed, from each state a run can be in. That is what
# the lease, the queue and shuttleLastPresented_ all change hands on, and it is
# where a fault is invisible rather than loud -- the phase 3 bug neither hung
# nor froze, and was only reachable by the command that ends a run AFTERWARDS.
#
# States: R reverse run (J), F forward run (the Fast-forward button at 2x),
#         P paused, N playing normally at 1x.
#
# Commands not on this list are not boundaries and are covered elsewhere:
# fullscreen, the HUD toggle, the readout keys, mute.
#
#   -FromReverse   R x { Space K Right prevBtn ffBtn L J scrub quit }
#   -FromForward   F x { Space K Right prevBtn ffBtn J scrub quit }
#   -Entries       P and N x { ffBtn }, plus the ladder P -> ff x5
#   -Delayed       the phase 3 gesture and its untested forward mirror: step
#                  with the BUTTON during a run, then press K
#   -All           all of the above
#
# Each case reports whether the picture is moving or still when it should be,
# and whether a step still works afterwards. A stranded lease shows up as the
# second; a stranded queue as the first.
#
# CHOOSE THE CLIP FOR THE MEASUREMENT, not for coverage. Two properties matter
# and neither is obvious:
#
#   16:9, because the picture signature samples a grid across the window and a
#   9:16 clip pillarboxes to `display 202x360` inside a 1280-wide viewer -- so
#   four fifths of the grid lands on black and never changes. Run on the 412
#   frame 9x16 clip the moving cases read 13-15% and one step read 0.0%; the
#   same 21 cases on a 16:9 clip of similar length read 48-49% moving and every
#   step between 1.5 and 50%. Both runs PASSED. The first one was nearly unable
#   to tell the two states apart and said so nowhere.
#
#   Long enough, because a forward case has to still be running when it is
#   sampled. 121 frames is not: at +2x the run reaches the tail inside the
#   observation window and reports `moved 0%`, which is a FAIL for a transition
#   that worked. Around 250 frames upward.
#
# M&M_TopGun_1080.mp4 satisfies both and is what the phase 4 record used.

param(
    [switch]$FromReverse, [switch]$FromForward, [switch]$Entries, [switch]$Delayed,
    [switch]$All,
    [Parameter(Mandatory = $true)][string]$Clip,
    # Where to dump the ladder captures. The rung a press reaches cannot be
    # asserted from pixels; it is read off the HUD by eye.
    [string]$LadderOut
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class TR {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
  public const uint DOWN = 0x0002, UP = 0x0004;
}
"@

$restart = Join-Path $PSScriptRoot "restart.ps1"

function Handle {
    return Get-Process -Name Trace -ErrorAction SilentlyContinue |
           Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
}

function Shot($h) {
    $r = New-Object TR+RECT
    [TR]::GetWindowRect($h, [ref]$r) | Out-Null
    $b = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $b.Size)
    $g.Dispose()
    return @{ bmp = $b; rect = $r }
}

function Sig($h) {
    $s = Shot $h
    $b = $s.bmp; $w = $b.Width; $ht = $b.Height
    if ($w -le 0 -or $ht -le 0) { $b.Dispose(); return $null }
    $vals = New-Object System.Collections.ArrayList
    for ($y = [int]($ht * 0.15); $y -lt [int]($ht * 0.55); $y += 13) {
        for ($x = [int]($w * 0.12); $x -lt [int]($w * 0.88); $x += 13) {
            $c = $b.GetPixel($x, $y); [void]$vals.Add($c.R + $c.G + $c.B)
        }
    }
    $b.Dispose()
    return $vals
}

# NOT named Diff: `diff` is a built-in alias for Compare-Object, and aliases
# outrank functions in PowerShell's resolution order, so a function called Diff
# is never called at all. Every check in one phase-3 smoke run reported FAIL for
# that reason and each one looked like an app fault.
function SigDiff($a, $b) {
    if ($null -eq $a -or $null -eq $b -or $a.Count -ne $b.Count) { return -1 }
    $m = 0
    for ($i = 0; $i -lt $a.Count; $i++) { if ([Math]::Abs($a[$i] - $b[$i]) -gt 12) { $m++ } }
    return [Math]::Round(100.0 * $m / $a.Count, 1)
}

# The groove AND the three transport buttons, in one screen grab.
#
# The buttons are found by their own pixels rather than by arithmetic off the
# groove. The arithmetic is derivable -- 16px margin, 44/60/44 controls, 8px
# spacing -- and it was wrong by ten pixels when checked against a capture,
# because QSlider insets its groove by the handle radius. A cluster scan can
# ASSERT that it found exactly three controls; a formula cannot notice that it
# has drifted, and a click ten pixels off the edge of a button is a silent pass.
function Geometry($h) {
    $s = Shot $h
    $b = $s.bmp; $r = $s.rect
    $w = $b.Width; $ht = $b.Height

    $grooveY = -1; $x0 = -1; $x1 = -1; $best = 0
    for ($y = [int]($ht * 0.25); $y -lt [int]($ht * 0.94); $y++) {
        $run = 0; $st = -1
        for ($x = 0; $x -lt $w; $x++) {
            $c = $b.GetPixel($x, $y)
            if ([Math]::Abs($c.R-55) -le 6 -and [Math]::Abs($c.G-55) -le 6 -and [Math]::Abs($c.B-55) -le 6) {
                if ($st -lt 0) { $st = $x }
                $run++
                if ($run -gt $best) { $best = $run; $grooveY = $y; $x0 = $st; $x1 = $x }
            } else { $run = 0; $st = -1 }
        }
    }
    if ($best -lt 300) { $b.Dispose(); return $null }

    # Icon pixels on the groove's own row, left of the groove. The bar is
    # RGB(18,18,18) and the play control's resting surface reaches only ~41, so
    # a threshold of 120 sees glyphs and nothing else.
    #
    # Stopping 24px short of the groove is not slack, it is a fourth cluster.
    # The slider HANDLE is a white circle on this exact row, and at frame 0 --
    # which is where restart.ps1 leaves the app, and therefore always where this
    # runs -- it hangs 12px past the left end of the groove. Measured: controls
    # at 40..50, 100..114, 157..173, handle at 204..215 with the groove starting
    # at 216. Without the margin the scan finds four controls and refuses.
    $scanTo = $x0 - 24
    $marks = New-Object System.Collections.ArrayList
    for ($x = 0; $x -lt $scanTo; $x++) {
        $c = $b.GetPixel($x, $grooveY)
        if (($c.R + $c.G + $c.B) / 3 -gt 120) { [void]$marks.Add($x) }
    }
    $b.Dispose()
    if ($marks.Count -lt 3) { return $null }

    # Merge within 30px: the control pitch is 60px, and a glyph made of two
    # chevrons has a gap of only a few.
    $clusters = New-Object System.Collections.ArrayList
    $cs = $marks[0]; $ce = $marks[0]
    for ($i = 1; $i -lt $marks.Count; $i++) {
        if ($marks[$i] - $ce -le 30) { $ce = $marks[$i] }
        else { [void]$clusters.Add([int](($cs + $ce) / 2)); $cs = $marks[$i]; $ce = $marks[$i] }
    }
    [void]$clusters.Add([int](($cs + $ce) / 2))
    if ($clusters.Count -ne 3) { return $null }

    return @{
        y      = $r.T + $grooveY
        a      = $r.L + $x0 + 6
        b      = $r.L + $x1 - 6
        prev   = $r.L + $clusters[0]
        play   = $r.L + $clusters[1]
        ffwd   = $r.L + $clusters[2]
        found  = $clusters.Count
    }
}

function Click($x, $y) {
    [TR]::SetCursorPos($x, $y) | Out-Null
    Start-Sleep -Milliseconds 120
    [TR]::mouse_event([TR]::DOWN, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 90
    [TR]::mouse_event([TR]::UP, 0, 0, 0, [IntPtr]::Zero)
}

# $enter is what puts the app into the state under test; $transition is the
# command being enumerated. Splitting them is what makes the matrix readable:
# the same nine commands run against a reverse run and a forward one.
function RunCase([string]$name, [scriptblock]$enter, [scriptblock]$transition,
                 [string]$expect, [double]$startFrac = 0.55) {
    & $restart -Clip $Clip | Out-Null
    $p = Handle
    if (-not $p) { Write-Output "$name : FAIL - no window after restart"; return }
    $h = $p.MainWindowHandle
    [TR]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 300

    $geo = Geometry $h
    if ($null -eq $geo) { Write-Output "$name : FAIL - groove or controls not located"; return }

    # Off frame 0, and far enough from the end the case runs TOWARD that the run
    # cannot finish for a reason unrelated to the case. This is not slack: at
    # the default 0.55 on a 121-frame clip, "N -> ffBtn" reached the tail during
    # the observation window and reported `moved 0%` -- a FAIL for a transition
    # that had worked perfectly. Same shape as the -StepCheck trap in revplay
    # and the frame-0 static-picture trap, and it is why forward cases start low.
    Click ([int]($geo.a + ($geo.b - $geo.a) * $startFrac)) $geo.y
    Start-Sleep -Milliseconds 700
    [TR]::SetForegroundWindow($h) | Out-Null

    & $enter $h $geo
    Start-Sleep -Milliseconds 900

    & $transition $h $geo
    Start-Sleep -Milliseconds 900

    $p2 = Handle
    if ($expect -eq "gone") {
        if ($null -eq $p2) { Write-Output "$name : PASS - exited cleanly" }
        else { Write-Output "$name : FAIL - still running after quit" }
        return
    }
    if ($null -eq $p2) { Write-Output "$name : FAIL - process gone"; return }

    $s1 = Sig $h
    Start-Sleep -Milliseconds 700
    $s2 = Sig $h
    $moved = SigDiff $s1 $s2

    # A step must change the picture. A stranded lease shows up here and only
    # here: the step cannot decode until the decoder comes back.
    [TR]::SetForegroundWindow($h) | Out-Null
    [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}")
    Start-Sleep -Milliseconds 600
    $s3 = Sig $h
    $stepped = SigDiff $s2 $s3

    $verdict = "PASS"
    if ($expect -eq "still" -and $moved -ge 3.0) { $verdict = "FAIL (moving, expected still)" }
    if ($expect -eq "moving" -and $moved -lt 1.0) { $verdict = "FAIL (frozen, expected moving)" }
    if ($stepped -lt 0) { $verdict = "FAIL (unresponsive)" }
    Write-Output ("{0} : {1} - moved {2}%, step moved {3}%" -f $name, $verdict, $moved, $stepped)
}

if ($All) { $FromReverse = $FromForward = $Entries = $Delayed = $true }

$enterReverse = { param($h,$g) [System.Windows.Forms.SendKeys]::SendWait("j") }
$enterForward = { param($h,$g) Click $g.ffwd $g.y; [TR]::SetForegroundWindow($h) | Out-Null }
$enterNormal  = { param($h,$g) [System.Windows.Forms.SendKeys]::SendWait(" ") }
$enterNone    = { param($h,$g) }

$cSpace   = { param($h,$g) [System.Windows.Forms.SendKeys]::SendWait(" ") }
$cK       = { param($h,$g) [System.Windows.Forms.SendKeys]::SendWait("k") }
$cRight   = { param($h,$g) [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}") }
$cJ       = { param($h,$g) [System.Windows.Forms.SendKeys]::SendWait("j") }
$cL       = { param($h,$g) [System.Windows.Forms.SendKeys]::SendWait("l") }
$cPrevBtn = { param($h,$g) Click $g.prev $g.y; [TR]::SetForegroundWindow($h) | Out-Null }
$cFfBtn   = { param($h,$g) Click $g.ffwd $g.y; [TR]::SetForegroundWindow($h) | Out-Null }
$cScrub   = {
    param($h,$g)
    $mid = [int](($g.a + $g.b) / 2)
    [TR]::SetCursorPos($mid, $g.y) | Out-Null
    [TR]::mouse_event([TR]::DOWN,0,0,0,[IntPtr]::Zero)
    for ($i = 0; $i -lt 20; $i++) {
        [TR]::SetCursorPos($mid - $i*8, $g.y) | Out-Null
        $sp = [Diagnostics.Stopwatch]::StartNew()
        while ($sp.Elapsed.TotalMilliseconds -lt 8) { }
    }
    [TR]::mouse_event([TR]::UP,0,0,0,[IntPtr]::Zero)
}
$cQuit = {
    param($h,$g)
    Get-Process -Name Trace -ErrorAction SilentlyContinue |
        ForEach-Object { $_.CloseMainWindow() | Out-Null }
}

if ($FromReverse) {
    # Space is STILL, not moving. It is play/pause and a reverse run is
    # playback, so it stops. The first version of this expectation asserted
    # "moving" and failed the app for doing the right thing -- an expectation is
    # part of the test and gets the same scrutiny as the code.
    RunCase "R -> Space"   $enterReverse $cSpace   "still"  0.85
    RunCase "R -> K"       $enterReverse $cK       "still"  0.85
    RunCase "R -> Right"   $enterReverse $cRight   "still"  0.85
    RunCase "R -> prevBtn" $enterReverse $cPrevBtn "still"  0.85
    # The two that end reverse by starting a FORWARD run start mid-clip: they
    # need room in both directions.
    RunCase "R -> ffBtn"   $enterReverse $cFfBtn   "moving" 0.45
    RunCase "R -> L"       $enterReverse $cL       "moving" 0.45
    # Same direction, next rung. Never enumerated before phase 4, and it is a
    # full run boundary: the lease is revoked and the queue dropped exactly as
    # on a direction change.
    RunCase "R -> J"       $enterReverse $cJ       "moving" 0.85
    RunCase "R -> scrub"   $enterReverse $cScrub   "still"  0.85
    RunCase "R -> quit"    $enterReverse $cQuit    "gone"   0.85
}

if ($FromForward) {
    RunCase "F -> Space"   $enterForward $cSpace   "still"  0.15
    RunCase "F -> K"       $enterForward $cK       "still"  0.15
    RunCase "F -> Right"   $enterForward $cRight   "still"  0.15
    RunCase "F -> prevBtn" $enterForward $cPrevBtn "still"  0.15
    RunCase "F -> ffBtn"   $enterForward $cFfBtn   "moving" 0.15
    RunCase "F -> J"       $enterForward $cJ       "moving" 0.45
    RunCase "F -> scrub"   $enterForward $cScrub   "still"  0.15
    RunCase "F -> quit"    $enterForward $cQuit    "gone"   0.15
}

if ($Entries) {
    RunCase "P -> ffBtn"   $enterNone   $cFfBtn "moving" 0.15
    RunCase "N -> ffBtn"   $enterNormal $cFfBtn "moving" 0.15
}

if ($Delayed) {
    # The gesture that found the phase 3 bug, and the mirror of it that has
    # never been run. Stepping with a BUTTON during a run left the run's own
    # frame in shuttleLastPresented_, and the fault only appeared on the NEXT
    # command that ends a run and takes its landing branch. Reverse then click
    # then arrow-key passes on a broken build; it takes the K.
    RunCase "R -> prevBtn -> K" $enterReverse {
        param($h,$g) Click $g.prev $g.y; [TR]::SetForegroundWindow($h) | Out-Null
        Start-Sleep -Milliseconds 700
        [System.Windows.Forms.SendKeys]::SendWait("k")
    } "still" 0.85
    RunCase "F -> prevBtn -> K" $enterForward {
        param($h,$g) Click $g.prev $g.y; [TR]::SetForegroundWindow($h) | Out-Null
        Start-Sleep -Milliseconds 700
        [System.Windows.Forms.SendKeys]::SendWait("k")
    } "still" 0.15
}

if ($LadderOut) {
    # The rung a press reaches is not visible in the picture, so this leg
    # captures rather than asserts: five clicks from paused should read
    # +2, +5, +10, +30, +30 on the HUD's `speed` and `shuttle stride`.
    & $restart -Clip $Clip | Out-Null
    $p = Handle
    $h = $p.MainWindowHandle
    [TR]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 300
    $geo = Geometry $h
    if ($null -eq $geo) { Write-Output "ladder : FAIL - controls not located" }
    else {
        Click ([int]($geo.a + ($geo.b - $geo.a) * 0.02)) $geo.y
        Start-Sleep -Milliseconds 700
        New-Item -ItemType Directory -Force $LadderOut | Out-Null
        # Six presses, close together, from the head. The settle has to be short
        # or the run outlives the ladder: at 30x a 412-frame clip is traversed in
        # 0.57s, so a half-second pause between presses ends the run before the
        # rung above it is ever pressed. Six because the spec's "further presses
        # at +30x remain at +30x" needs a second press at the top.
        for ($i = 1; $i -le 6; $i++) {
            Click $geo.ffwd $geo.y
            Start-Sleep -Milliseconds 180
            $s = Shot $h
            $s.bmp.Save((Join-Path $LadderOut "ff$i.png"), [System.Drawing.Imaging.ImageFormat]::Png)
            $s.bmp.Dispose()
        }
        Write-Output "ladder : captured 6 presses to $LadderOut"

        # The cap needs its own leg, because capturing between presses is what
        # makes the ladder too slow to reach it: at 30x the run traverses the
        # whole clip in 0.57s, so by the fifth press the run has ended and the
        # press reads +2x from PAUSED -- which is correct behaviour that looks
        # exactly like a ladder that wrapped. Press without capturing, then
        # capture once: six presses must read 30x, not 2x.
        & $restart -Clip $Clip | Out-Null
        $p2 = Handle
        if ($null -ne $p2) {
            $h2 = $p2.MainWindowHandle
            [TR]::SetForegroundWindow($h2) | Out-Null
            Start-Sleep -Milliseconds 300
            $geo2 = Geometry $h2
            if ($null -ne $geo2) {
                Click ([int]($geo2.a + ($geo2.b - $geo2.a) * 0.02)) $geo2.y
                Start-Sleep -Milliseconds 700
                for ($i = 1; $i -le 6; $i++) { Click $geo2.ffwd $geo2.y; Start-Sleep -Milliseconds 60 }
                $s = Shot $h2
                $s.bmp.Save((Join-Path $LadderOut "cap.png"), [System.Drawing.Imaging.ImageFormat]::Png)
                $s.bmp.Dispose()
                Write-Output "ladder : cap leg captured (6 rapid presses) to $LadderOut\cap.png"
            }
        }
    }
}
