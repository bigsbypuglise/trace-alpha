# Drives the overlay spike: hover then click where the spike button is, and
# report what the app said it received. Used to answer "can a Qt overlay take
# input above the native video surface", which is a different question from
# "can it be seen" -- an invisible widget may still eat clicks, and a native
# child window may swallow them before Qt ever hears.

param(
    [Parameter(Mandatory = $true)][string]$Renderer,
    [Parameter(Mandatory = $true)][int]$Variant,
    [string]$Clip = "C:\Users\andre\Documents\Claude_Cowork\Trace_Testing_Assets\4_4K_H264_MP4\Splash_1.mp4"
)

Add-Type @"
using System;using System.Runtime.InteropServices;
public class S {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(System.Drawing.Point p);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
  public const uint DOWN = 0x0002, UP = 0x0004, WHEEL = 0x0800;
}
"@ -ReferencedAssemblies System.Drawing, System.Windows.Forms

Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
$log = "$env:TEMP\spike_input_$Renderer`_$Variant.txt"
$env:TRACE_RENDERER = $Renderer
$env:TRACE_OVERLAY_SPIKE = "$Variant"
$p = Start-Process -FilePath "build\app\Release\Trace.exe" -ArgumentList "`"$Clip`"" -PassThru -RedirectStandardError $log
Start-Sleep -Seconds 5
$env:TRACE_RENDERER = ""; $env:TRACE_OVERLAY_SPIKE = ""

$proc = Get-Process -Id $p.Id
$h = $proc.MainWindowHandle
[S]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 500

$r = New-Object S+RECT
[S]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L

# The viewer occupies the top of the client area; the spike button sits at
# ((viewerW-120)/2, viewerH-78). Derived from the captured layout rather than
# guessed: the video rect ends just above the transport, around 0.47 of window
# height in these runs.
$bx = $r.L + [int]($w / 2)
$by = $r.T + 352

$cls = New-Object System.Text.StringBuilder 256
[S]::WindowFromPoint((New-Object System.Drawing.Point($bx, $by))) | ForEach-Object {
    [S]::GetClassName($_, $cls, 256) | Out-Null
}
Write-Output ("SPIKE-INPUT window under button point: '{0}'" -f $cls.ToString())

# Hover, then click.
[S]::SetCursorPos($bx, $by) | Out-Null
Start-Sleep -Milliseconds 700
[S]::mouse_event([S]::DOWN, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 90
[S]::mouse_event([S]::UP, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 700
# Wheel over the video area, to see whether the surface traps it.
[S]::mouse_event([S]::WHEEL, 0, 0, 120, [IntPtr]::Zero)
Start-Sleep -Milliseconds 500

Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
Write-Output "--- reported by app ($Renderer, variant $Variant) ---"
$lines = Get-Content $log -ErrorAction SilentlyContinue | Select-String "OVERLAY-SPIKE"
if ($lines) { $lines | ForEach-Object { $_.Line } } else { Write-Output "(nothing)" }
