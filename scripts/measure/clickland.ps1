# How long does ONE timeline click take to land, and how long is the window dead
# while it lands?
#
# A click is a jump: press+release on the same value, landed exactly through
# RequestMode::Step, SYNCHRONOUSLY on the UI thread (scrubJumpPending_, c3335ec).
# So its whole cost is seek + walk to the target, and on a file whose only
# keyframe is frame 0 that walk is unbounded by the GOP -- it is bounded by the
# clip. That is a different failure from a drag, and no drag harness measures it:
# scrub.ps1 presses and immediately sweeps, so the press cost is folded into the
# gesture.
#
# The number this prints is wall time from mouse-up to the window answering a
# WM_NULL again (SendMessageTimeout), i.e. how long the application is frozen.
# Read it beside the HUD's `ui gap max`, which measures the same block from the
# inside and should agree.
param(
    [double]$At = 0.85,          # fraction along the groove to click
    [string]$ProcName = "Trace"
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class CL {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr wp, IntPtr lp, uint flags, uint ms, out IntPtr res);
  public const uint DOWN = 0x0002, UP = 0x0004;
}
"@

$p = Get-Process -Name $ProcName -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
$h = $p.MainWindowHandle
[CL]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 400

$r = New-Object CL+RECT
[CL]::GetWindowRect($h, [ref]$r) | Out-Null
$winW = $r.R - $r.L; $winH = $r.B - $r.T

# Same groove scan as scrub.ps1: longest run of the unfilled track colour.
$bmp = New-Object System.Drawing.Bitmap $winW, $winH
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
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
if ($best -lt 300) { Write-Output "groove not found (best run $best)"; exit 1 }
Write-Output "groove y=$grooveY x=$x0..$x1"

$sy = $r.T + $grooveY
$x = $r.L + $x0 + [int](($x1 - $x0) * $At)
[CL]::SetCursorPos($x, $sy) | Out-Null
Start-Sleep -Milliseconds 250

# Prove the window is answering BEFORE the click, or a zero reading means
# nothing: a window that was already busy would look instant.
$res = [IntPtr]::Zero
[CL]::SendMessageTimeout($h, 0, [IntPtr]::Zero, [IntPtr]::Zero, 0, 2000, [ref]$res) | Out-Null

# A SINGLE probe after the click is NOT a measurement of the freeze, and the
# first version of this script was one. A sent message is serviced ahead of the
# posted mouse input, so the probe can be answered before the click is even
# dispatched and report 3ms for a landing that goes on to block for 400. Poll
# instead: probe with a 5ms timeout for up to 5s and report the longest
# contiguous stretch of timeouts, which is the window actually being dead.
[CL]::mouse_event([CL]::DOWN, 0, 0, 0, [IntPtr]::Zero)
[CL]::mouse_event([CL]::UP, 0, 0, 0, [IntPtr]::Zero)
$sw = [Diagnostics.Stopwatch]::StartNew()
$deadStart = -1.0; $worst = 0.0; $lastOk = 0.0
while ($sw.Elapsed.TotalMilliseconds -lt 5000) {
    $t = $sw.Elapsed.TotalMilliseconds
    $ok = [CL]::SendMessageTimeout($h, 0, [IntPtr]::Zero, [IntPtr]::Zero, 3, 5, [ref]$res)
    if ($ok -eq [IntPtr]::Zero) {
        if ($deadStart -lt 0) { $deadStart = $lastOk }
    } else {
        if ($deadStart -ge 0) {
            $span = $sw.Elapsed.TotalMilliseconds - $deadStart
            if ($span -gt $worst) { $worst = $span }
            $deadStart = -1.0
        }
        $lastOk = $sw.Elapsed.TotalMilliseconds
        # Stop once the window has been continuously alive for 250ms.
        if ($worst -gt 0 -and ($sw.Elapsed.TotalMilliseconds - $lastOk) -ge 0) {
            if ($sw.Elapsed.TotalMilliseconds -gt $worst + 250) { break }
        } elseif ($sw.Elapsed.TotalMilliseconds -gt 400) { break }
    }
}
$sw.Stop()
Write-Output ("click at {0} : window frozen {1:N0} ms" -f $At, $worst)
Start-Sleep -Milliseconds 700
