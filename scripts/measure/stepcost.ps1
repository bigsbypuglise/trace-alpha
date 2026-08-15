# What does one frame step cost, forward and backward, measured from outside?
#
# Stepping is exact by contract and synchronous on the UI thread, so its cost IS
# the freeze the user feels. A backward step is a random-access request: cache hit
# or seek + walk to the target. On a file whose only keyframe is frame 0 that walk
# is bounded by the clip rather than by a GOP, which is why this needs measuring
# separately from a drag -- the scrub worker is not involved in a step at all.
#
# Timing is mouse-free: SendMessageTimeout with SMTO_BLOCK returns only when the
# UI thread pumps again. The key is POSTED (keybd_event) so it queues behind the
# work, and the probe is sent afterwards, so a sent message cannot overtake it.
param(
    [int]$Steps = 8,
    [ValidateSet('back','forward')][string]$Direction = 'back',
    [string]$ProcName = "Trace"
)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class SC {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr wp, IntPtr lp, uint flags, uint ms, out IntPtr res);
  public const byte LEFT = 0x25, RIGHT = 0x27;
  public const uint KEYUP = 0x0002;
}
"@

$p = Get-Process -Name $ProcName -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
$h = $p.MainWindowHandle
[SC]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 400

$vk = if ($Direction -eq 'back') { [SC]::LEFT } else { [SC]::RIGHT }
$res = [IntPtr]::Zero
$times = @()
for ($i = 0; $i -lt $Steps; $i++) {
    # Drain first: prove the window is idle before each step, or a reading is
    # just the tail of the previous one.
    [SC]::SendMessageTimeout($h, 0, [IntPtr]::Zero, [IntPtr]::Zero, 3, 20000, [ref]$res) | Out-Null
    $sw = [Diagnostics.Stopwatch]::StartNew()
    [SC]::keybd_event($vk, 0, 0, [IntPtr]::Zero)
    [SC]::keybd_event($vk, 0, [SC]::KEYUP, [IntPtr]::Zero)
    [SC]::SendMessageTimeout($h, 0, [IntPtr]::Zero, [IntPtr]::Zero, 3, 20000, [ref]$res) | Out-Null
    $sw.Stop()
    $times += $sw.Elapsed.TotalMilliseconds
    Start-Sleep -Milliseconds 120
}
$fmt = ($times | ForEach-Object { "{0:N0}" -f $_ }) -join ' '
$stats = $times | Measure-Object -Average -Maximum -Minimum
Write-Output ("step $Direction x$Steps : " + $fmt)
Write-Output ("  min {0:N0}  avg {1:N0}  max {2:N0} ms" -f $stats.Minimum, $stats.Average, $stats.Maximum)
