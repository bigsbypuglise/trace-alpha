# Widen the Trace window without changing the video rect.
#
# For portrait media (9:16, 4:5, 1:1) section 4 gives a narrow window and the dev HUD
# clips -- phase 12's recorded diagnostic limitation. Every harness that reads a
# figure off a capture is then reading a truncated line, and scrub.ps1's groove
# scan fails outright because the slider is under its 300px minimum run.
#
# Widening costs the measurement NOTHING on portrait media, and that is the point:
# the video rect is fitted, so on a clip taller than it is wide the fit is
# HEIGHT-bound. Adding width adds letterbox, not picture. Verify it rather than
# assuming -- the HUD's `display WxH` must be identical before and after, and this
# script prints both so the run that used it carries its own proof.
#
# SetWindowPos is deliberate: it sends no WM_SIZING, so section 4's aspect lock never
# runs and does not fight the resize. That is the same route Windows Snap takes.
param(
    [int]$Width = 1300,
    [int]$Height = 0,
    [string]$ProcName = "Trace"
)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Wide {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int ht, uint flags);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public const uint NOZORDER = 0x0004, NOACTIVATE = 0x0010;
}
"@

$p = Get-Process -Name $ProcName -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "no window"; exit 1 }
$h = $p.MainWindowHandle
$r = New-Object Wide+RECT
[Wide]::GetWindowRect($h, [ref]$r) | Out-Null
$w0 = $r.R - $r.L; $h0 = $r.B - $r.T
if ($Height -le 0) { $Height = $h0 }
Write-Output "before ${w0}x${h0} at ($($r.L),$($r.T))"

[Wide]::SetWindowPos($h, [IntPtr]::Zero, $r.L, $r.T, $Width, $Height, ([Wide]::NOZORDER -bor [Wide]::NOACTIVATE)) | Out-Null
Start-Sleep -Milliseconds 400
[Wide]::GetWindowRect($h, [ref]$r) | Out-Null
Write-Output "after  $($r.R - $r.L)x$($r.B - $r.T)"

# The HUD is not rebuilt on resizeEvent (phase 12), so a paused window redraws the
# new geometry with the OLD string in it. Step one frame to force a refresh, or
# every figure read afterwards is from before the resize.
[Wide]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 250
$sh = New-Object -ComObject WScript.Shell
$sh.SendKeys('{RIGHT}')
Start-Sleep -Milliseconds 500
$sh.SendKeys('{LEFT}')
Start-Sleep -Milliseconds 500
Write-Output "stepped +1/-1 to refresh the HUD"
