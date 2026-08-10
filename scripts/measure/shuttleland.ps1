# The reverse -> forward direction change, with and without the exact landing.
#
# This exists to settle ONE question and deliberately not the others. Spec phase
# 3 left `landPreviousExactly` preserved rather than unified because J passes
# false and L passes true, and the recorded justification -- "a J press always
# produces a run that supersedes the picture immediately" -- turned out to
# describe a mechanism that no longer exists: before dd21fe9 an off-speed
# forward run presented one frame per tick synchronously, so its picture was
# already exact. It is a queued, strided run now, the same shape as reverse.
#
# What is left that reading cannot answer: the landing branch is a synchronous
# Step decode which leaves the DECODER positioned on the frame that is on
# screen, and the forward run that follows decodes forward from wherever the
# decoder is. So the open question is anchoring, and it is measured here as a
# trade between two numbers that live in different places:
#
#   land <ms>   the UI thread blocked inside the landing decode (HUD, phase 4)
#   starve N    slots the new forward run had no frame for (HUD, per run)
#
# The gesture: click mid-clip, press J to establish a reverse run and let its
# queue fill, then press L. Everything after the L press is the forward run's
# own counters, because beginPlaybackTimeline resets them on every press.
#
# Note `land` is reported for the whole session, not per press, so a single-L
# capture reads `land 1`. That is the point: the count says how many times the
# UI thread paid it.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    # Rungs of J before the direction change. 1 = -1x, 2 = -2x.
    [ValidateRange(1, 5)][int]$ReversePresses = 1,
    # Rungs of L after it. 1 = ordinary 1x playback (NOT a shuttle run),
    # 2 = +2x, which is what the Fast-forward button will do on its first click.
    [ValidateRange(1, 5)][int]$ForwardPresses = 2,
    [double]$ReverseHold = 2.5,
    [double]$ForwardHold = 2.5,
    # Where the reverse leg starts, as a fraction of the groove. The default is
    # mid-clip; a high reverse rung needs to start nearer the tail or the run
    # reaches the head and the transition being measured never happens.
    [double]$StartFraction = 0.55,
    # NAME=VALUE, passed through to restart.ps1. TRACE_SHUTTLE_LAND=0 is the
    # control; TRACE_SHUTTLE_ENTRY=2x drives the button convention.
    [string[]]$Env = @(),
    [string]$Out
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class SL {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
  public const uint DOWN = 0x0002, UP = 0x0004;
}
"@

$restart = Join-Path $PSScriptRoot "restart.ps1"
& $restart -Clip $Clip -Env $Env | Write-Output

$p = Get-Process -Name Trace -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
$h = $p.MainWindowHandle
[SL]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 400

$rect = New-Object SL+RECT
[SL]::GetWindowRect($h, [ref]$rect) | Out-Null
$winW = $rect.R - $rect.L; $winH = $rect.B - $rect.T
Write-Output "win ${winW}x${winH}"

# Longest run of the unfilled track colour inside the transport band. A
# first-match scan latches onto window chrome, which carries short runs of
# similar greys.
$bmp = New-Object System.Drawing.Bitmap $winW, $winH
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($rect.L, $rect.T, 0, 0, $bmp.Size)
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
if ($best -lt 300) { Write-Output "groove not found (longest run $best)"; exit 1 }

$sy = $rect.T + $grooveY
$sxA = $rect.L + $x0 + 6
$sxB = $rect.L + $x1 - 6

# Mid-clip on purpose: reverse needs somewhere to run from, and forward needs
# somewhere to run to. Either end would make one of the two legs clip-limited
# and the run would end for a reason unrelated to what is being measured.
$startX = [int]($sxA + ($sxB - $sxA) * $StartFraction)
[SL]::SetCursorPos($startX, $sy) | Out-Null
Start-Sleep -Milliseconds 200
[SL]::mouse_event([SL]::DOWN, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 120
[SL]::mouse_event([SL]::UP, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 900

[SL]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 200

$revSpeeds = @(1, 2, 5, 10, 30)
Write-Output ("J x{0} -> expect -{1}x, hold {2}s" -f $ReversePresses, $revSpeeds[$ReversePresses - 1], $ReverseHold)
for ($i = 0; $i -lt $ReversePresses; $i++) {
    [System.Windows.Forms.SendKeys]::SendWait("j")
    Start-Sleep -Milliseconds 60
}
# Long enough for the queue to reach its depth of 8, which is what puts the
# decoder run-ahead of the picture -- the thing the anchor is about.
Start-Sleep -Seconds $ReverseHold

Write-Output ("L x{0} -> expect +{1}x, hold {2}s" -f $ForwardPresses, $revSpeeds[$ForwardPresses - 1], $ForwardHold)
for ($i = 0; $i -lt $ForwardPresses; $i++) {
    [System.Windows.Forms.SendKeys]::SendWait("l")
    Start-Sleep -Milliseconds 60
}
Start-Sleep -Seconds $ForwardHold

# Captured BEFORE any stop: `speed`, `starve` and the run's cadence figures are
# what this measures, and a stop clears the first of them.
if ($Out) {
    $rr = New-Object SL+RECT
    [SL]::GetWindowRect($h, [ref]$rr) | Out-Null
    $b = New-Object System.Drawing.Bitmap ($rr.R - $rr.L), ($rr.B - $rr.T)
    $gg = [System.Drawing.Graphics]::FromImage($b)
    $gg.CopyFromScreen($rr.L, $rr.T, 0, 0, $b.Size)
    $b.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $gg.Dispose(); $b.Dispose()
    Write-Output "saved $Out"
}
