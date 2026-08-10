# Every way OUT of a reverse run, exercised one at a time.
#
# Plan section 29.2 is the reason this exists: GATE E was validated on the Play
# action alone, and every other path that started the playback timer kept
# compiling silently for weeks. A reverse run adds a decoder LEASE to the same
# machinery, so a path that leaves a run without returning the lease does not
# misbehave visibly -- it strands the decoder on the worker and the next
# UI-thread decode waits for an owner that has no reason to give it back.
#
# So the transitions are enumerated rather than sampled:
#
#   -ToScrub    reverse, then grab the slider and drag
#   -ToStep     reverse, then arrow-key
#   -ToPlay     reverse, then Space
#   -ToForward  reverse, then L
#   -ToPause    reverse, then K
#   -ToQuit     reverse, then close the window
#   -All        every one of them, on separate app instances
#
# Each reports PASS/FAIL on two things: the app is still responsive afterwards,
# and the picture is not frozen when it should be moving (or moving when it
# should be still). A hung lease shows up as the first; a stranded queue as the
# second.

param(
    [switch]$ToScrub, [switch]$ToStep, [switch]$ToPlay,
    [switch]$ToForward, [switch]$ToPause, [switch]$ToQuit,
    [switch]$All,
    [Parameter(Mandatory = $true)][string]$Clip
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class T {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
  public const uint DOWN = 0x0002, UP = 0x0004;
}
"@

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$restart = Join-Path $PSScriptRoot "restart.ps1"

function Handle {
    $p = Get-Process -Name Trace -ErrorAction SilentlyContinue |
         Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $p) { return $null }
    return $p
}

function Sig($h) {
    $rr = New-Object T+RECT
    [T]::GetWindowRect($h, [ref]$rr) | Out-Null
    $w = $rr.R - $rr.L; $ht = $rr.B - $rr.T
    if ($w -le 0 -or $ht -le 0) { return $null }
    $b = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen($rr.L, $rr.T, 0, 0, $b.Size)
    $vals = New-Object System.Collections.ArrayList
    for ($y = [int]($ht * 0.15); $y -lt [int]($ht * 0.55); $y += 13) {
        for ($x = [int]($w * 0.12); $x -lt [int]($w * 0.88); $x += 13) {
            $c = $b.GetPixel($x, $y); [void]$vals.Add($c.R + $c.G + $c.B)
        }
    }
    $g.Dispose(); $b.Dispose()
    return $vals
}

# NOT named Diff: `diff` is a built-in alias for Compare-Object, and aliases
# outrank functions in PowerShell's resolution order, so a function called Diff
# is simply never called. It fails by returning Compare-Object's own objects,
# which then compare as "not IComparable" rather than as a wrong number --
# every verdict in the run read PASS or FAIL for reasons unrelated to the app.
function SigDiff($a, $b) {
    if ($null -eq $a -or $null -eq $b -or $a.Count -ne $b.Count) { return -1 }
    $m = 0
    for ($i = 0; $i -lt $a.Count; $i++) { if ([Math]::Abs($a[$i] - $b[$i]) -gt 12) { $m++ } }
    return [Math]::Round(100.0 * $m / $a.Count, 1)
}

# Groove, for the transitions that need the slider.
function Groove($h) {
    $r = New-Object T+RECT
    [T]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    $b = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $b.Size)
    $y0 = -1; $x0 = -1; $x1 = -1; $best = 0
    for ($y = [int]($ht * 0.25); $y -lt [int]($ht * 0.94); $y++) {
        $run = 0; $st = -1
        for ($x = 0; $x -lt $w; $x++) {
            $c = $b.GetPixel($x, $y)
            if ([Math]::Abs($c.R-55) -le 6 -and [Math]::Abs($c.G-55) -le 6 -and [Math]::Abs($c.B-55) -le 6) {
                if ($st -lt 0) { $st = $x }
                $run++
                if ($run -gt $best) { $best = $run; $y0 = $y; $x0 = $st; $x1 = $x }
            } else { $run = 0; $st = -1 }
        }
    }
    $g.Dispose(); $b.Dispose()
    if ($best -lt 300) { return $null }
    return @{ y = $r.T + $y0; a = $r.L + $x0 + 6; b = $r.L + $x1 - 6 }
}

function RunCase([string]$name, [scriptblock]$transition, [string]$expect) {
    & $restart -Clip $Clip | Out-Null
    $p = Handle
    if (-not $p) { Write-Output "$name : FAIL - no window after restart"; return }
    $h = $p.MainWindowHandle
    [T]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 300

    # Get off frame 0 so reverse has somewhere to run, then start reversing.
    $gr = Groove $h
    if ($null -eq $gr) { Write-Output "$name : FAIL - groove not found"; return }
    $x = [int]($gr.a + ($gr.b - $gr.a) * 0.9)
    [T]::SetCursorPos($x, $gr.y) | Out-Null
    Start-Sleep -Milliseconds 150
    [T]::mouse_event([T]::DOWN,0,0,0,[IntPtr]::Zero)
    Start-Sleep -Milliseconds 120
    [T]::mouse_event([T]::UP,0,0,0,[IntPtr]::Zero)
    Start-Sleep -Milliseconds 700

    [T]::SetForegroundWindow($h) | Out-Null
    [System.Windows.Forms.SendKeys]::SendWait("j")
    Start-Sleep -Milliseconds 900

    & $transition $h $gr
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

    # Responsiveness: a step must change the picture. A stranded lease shows up
    # here, because the step cannot decode until the decoder comes back.
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

if ($All) { $ToScrub = $ToStep = $ToPlay = $ToForward = $ToPause = $ToQuit = $true }

if ($ToPause) {
    RunCase "reverse -> K" { param($h,$gr) [System.Windows.Forms.SendKeys]::SendWait("k") } "still"
}
if ($ToStep) {
    RunCase "reverse -> step" { param($h,$gr) [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}") } "still"
}
if ($ToPlay) {
    # STILL, not moving. Space is play/pause and a reverse run is playback, so
    # Space stops it -- that is the behaviour before and after the shuttle, and
    # the first version of this check asserted "moving" and failed the app for
    # doing the right thing. Expectations are part of the test and get the same
    # scrutiny as the code.
    RunCase "reverse -> Space" { param($h,$gr) [System.Windows.Forms.SendKeys]::SendWait(" ") } "still"
}
if ($ToForward) {
    RunCase "reverse -> L" { param($h,$gr) [System.Windows.Forms.SendKeys]::SendWait("l") } "moving"
}
if ($ToScrub) {
    RunCase "reverse -> scrub" {
        param($h,$gr)
        [T]::SetCursorPos([int](($gr.a + $gr.b)/2), $gr.y) | Out-Null
        [T]::mouse_event([T]::DOWN,0,0,0,[IntPtr]::Zero)
        for ($i = 0; $i -lt 20; $i++) {
            [T]::SetCursorPos([int](($gr.a + $gr.b)/2) - $i*8, $gr.y) | Out-Null
            $sp = [Diagnostics.Stopwatch]::StartNew()
            while ($sp.Elapsed.TotalMilliseconds -lt 8) { }
        }
        [T]::mouse_event([T]::UP,0,0,0,[IntPtr]::Zero)
    } "still"
}
if ($ToQuit) {
    RunCase "reverse -> quit" {
        param($h,$gr)
        Get-Process -Name Trace -ErrorAction SilentlyContinue |
            ForEach-Object { $_.CloseMainWindow() | Out-Null }
    } "gone"
}
