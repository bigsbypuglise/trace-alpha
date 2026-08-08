param([double]$Seconds = 1.5, [switch]$Backward, [switch]$Reversals)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W {
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
[W]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 400

$r = New-Object W+RECT
[W]::GetWindowRect($h, [ref]$r) | Out-Null
$winW = $r.R - $r.L; $winH = $r.B - $r.T

# Find the slider groove by its track colour rather than assuming a y offset:
# the transport sits above a HUD whose height depends on the media.
$bmp = New-Object System.Drawing.Bitmap $winW, $winH
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
# Search only the transport band and keep the LONGEST run, not the first: the
# window chrome contains short runs of similar greys that a first-match scan
# latches onto instead.
$grooveY = -1; $x0 = -1; $x1 = -1; $best = 0
$yFrom = [int]($winH * 0.25); $yTo = [int]($winH * 0.85)
for ($y = $yFrom; $y -lt $yTo; $y++) {
  $run = 0; $start = -1
  for ($x = 0; $x -lt $winW; $x++) {
    $c = $bmp.GetPixel($x, $y)
    if ([Math]::Abs($c.R - 55) -le 6 -and [Math]::Abs($c.G - 55) -le 6 -and [Math]::Abs($c.B - 55) -le 6) {
      if ($start -lt 0) { $start = $x }
      $run++
      if ($run -gt $best) { $best = $run; $grooveY = $y; $x0 = $start; $x1 = $x }
    } else {
      $run = 0; $start = -1
    }
  }
}
if ($best -lt 300) { $grooveY = -1 }
$g.Dispose(); $bmp.Dispose()
if ($grooveY -lt 0) { Write-Output "groove not found"; exit 1 }
Write-Output "groove y=$grooveY x=$x0..$x1"

$sy = $r.T + $grooveY
$sxA = $r.L + $x0 + 6
$sxB = $r.L + $x1 - 6

function Sweep([int]$from, [int]$to, [double]$secs) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($true) {
    $t = $sw.Elapsed.TotalSeconds / $secs
    if ($t -ge 1.0) { break }
    $x = [int]($from + ($to - $from) * $t)
    [W]::SetCursorPos($x, $sy) | Out-Null
    # Spin, don't sleep: a synthetic drag that teleports and pauses overstates
    # how well the shuttle keeps up.
    $spin = [Diagnostics.Stopwatch]::StartNew()
    while ($spin.Elapsed.TotalMilliseconds -lt 4) { }
  }
  [W]::SetCursorPos($to, $sy) | Out-Null
}

if ($Backward) { $a = $sxB; $b = $sxA } else { $a = $sxA; $b = $sxB }

[W]::SetCursorPos($a, $sy) | Out-Null
Start-Sleep -Milliseconds 200
[W]::mouse_event([W]::DOWN, 0, 0, 0, [IntPtr]::Zero)

if ($Reversals) {
  # Hard direction reversals under one continuous press, running into both
  # ends: the gesture set that found the decode-error in 2523d77.
  $mid = [int](($sxA + $sxB) / 2)
  Sweep $a $sxB 0.5; Sweep $sxB $sxA 0.5; Sweep $sxA $mid 0.3
  Sweep $mid $sxB 0.4; Sweep $sxB $mid 0.3; Sweep $mid $sxA 0.5
} else {
  Sweep $a $b $Seconds
}

Start-Sleep -Milliseconds 400
[W]::mouse_event([W]::UP, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 600
Write-Output "done"
