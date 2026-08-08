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

param(
    [switch]$StepCycle,
    [switch]$PlayAfter,
    [switch]$SwitchMedia,
    [switch]$KillMidDrag,
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
  public const uint DOWN = 0x0002, UP = 0x0004;
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

# Drag to roughly the middle of the clip, so stepping has room either side.
$mid = [int](($sxA + $sxB) / 2)
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
Start-Sleep -Milliseconds 900

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
