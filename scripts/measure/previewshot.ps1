# Capture the DRAG PREVIEW, not the landing.
#
# The two are scaled by completely different machinery and that is the whole
# point of this script. A preview converts in swscale straight to the size the
# viewer will draw it at (`b5a56af`) and is then drawn 1:1. A landing converts at
# full resolution and lets the renderer's sampler reduce it. So the same frame,
# at the same size, a fraction of a second apart, goes through a real multi-tap
# reduction one moment and a 2x2 tap the next -- and nothing in the repo had ever
# compared them with an instrument that could see the difference.
#
# Getting a preview on screen requires the button to stay DOWN: the release is
# what lands a full-resolution frame. So this presses, nudges, and holds while the
# capture is taken, then releases.
#
# It does not choose which frame it lands on, and cannot: the shuttle walks toward
# the pointer at whatever rate the media allows. The frame index is therefore READ
# BACK from the HUD and printed, and the caller generates its ffmpeg references for
# that index. Asserting a target frame here would fail on heavy media for reasons
# that have nothing to do with the question being asked.
#
# Groove location is scrub.ps1's, including both of its recorded traps: take the
# longest run in the transport band rather than the first match anywhere, and
# restart first so the unfilled RGB(55,55,55) track still exists.

param(
    [Parameter(Mandatory = $true)][string]$Out,
    # Where along the groove to drag to, 0..1.
    [double]$At = 0.5,
    # How long to hold before capturing. The shuttle needs to converge, or the
    # preview on screen is mid-walk and its index is still moving.
    [int]$HoldMs = 1200
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class PW {
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
[PW]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 400

$r = New-Object PW+RECT
[PW]::GetWindowRect($h, [ref]$r) | Out-Null
$winW = $r.R - $r.L; $winH = $r.B - $r.T

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
if ($best -lt 300) { Write-Output "groove not found (longest run $best)"; exit 1 }

$sy = $r.T + $grooveY
$sxA = $r.L + $x0 + 6
$sxB = $r.L + $x1 - 6
$target = [int]($sxA + ($sxB - $sxA) * $At)

# Press a little before the target and move onto it, so this is a DRAG. A press
# alone is a click, and `c3335ec` makes the first flush after a press land exactly
# through Step -- which is a full-resolution landing, i.e. the wrong path.
$from = [Math]::Max($sxA, $target - 60)
[PW]::SetCursorPos($from, $sy) | Out-Null
Start-Sleep -Milliseconds 120
[PW]::mouse_event([PW]::DOWN, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 120
$steps = 20
for ($i = 1; $i -le $steps; $i++) {
    [PW]::SetCursorPos([int]($from + ($target - $from) * $i / $steps), $sy) | Out-Null
    Start-Sleep -Milliseconds 25
}
Start-Sleep -Milliseconds $HoldMs

# Captured with the button still down.
$shot = New-Object System.Drawing.Bitmap $winW, $winH
$g2 = [System.Drawing.Graphics]::FromImage($shot)
$g2.CopyFromScreen($r.L, $r.T, 0, 0, $shot.Size)
$g2.Dispose()
$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$shot.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$shot.Dispose()

[PW]::mouse_event([PW]::UP, 0, 0, 0, [IntPtr]::Zero)

Write-Output ("saved {0} ({1}x{2}) with the button held" -f $Out, $winW, $winH)
Write-Output "READ THE HUD in that capture: `dst` must say BGRA at the display size"
Write-Output "for this to be a preview, and `shown N` is the frame to build"
Write-Output "references for. A `dst ... planar` line means the landing was caught"
Write-Output "and the capture is not a preview."
