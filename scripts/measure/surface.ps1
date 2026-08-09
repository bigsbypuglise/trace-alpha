# Window and surface lifecycle for the native presentation path.
#
# These are the states where a swapchain and a child window can get out of step
# with the widget that owns them: the failure is a black rect, a stale size, or
# an orphaned window, none of which a throughput harness reaches.
#
# Captures at each step; the caller diffs or eyeballs. Also counts the native
# surface windows in the process at the end, which is how a leak shows up.

param(
    [Parameter(Mandatory = $true)][string]$Clip,
    [string]$Renderer = "d3d11",
    [int]$Frame = 40,
    [string]$OutDir = "$env:TEMP\trace_surface"
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;using System.Text;using System.Collections.Generic;using System.Runtime.InteropServices;
public class SF {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int ht,bool r);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int c);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
  public static List<string> classes = new List<string>();
  public static bool Collect(IntPtr h, IntPtr p) {
    var sb = new StringBuilder(256); GetClassName(h, sb, 256); classes.Add(sb.ToString()); return true;
  }
}
"@

New-Item -ItemType Directory -Force $OutDir | Out-Null
& "$PSScriptRoot\restart.ps1" -Clip $Clip -Env "TRACE_RENDERER=$Renderer" | Out-Null

$p = Get-Process -Name Trace | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$h = $p.MainWindowHandle
[SF]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 400
for ($i = 0; $i -lt $Frame; $i++) { [System.Windows.Forms.SendKeys]::SendWait("{RIGHT}"); Start-Sleep -Milliseconds 40 }
Start-Sleep -Milliseconds 600

function Shot([string]$tag) {
    $r = New-Object SF+RECT
    [SF]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $ht = $r.B - $r.T
    if ($w -le 0 -or $ht -le 0) { Write-Output "SURFACE $tag : window has no area"; return }
    $b = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $b.Size)
    $path = Join-Path $OutDir "$tag.png"
    $b.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    # Non-black fraction of the upper band, so "went black" is detected rather
    # than left for someone to notice in a picture.
    $y0 = [int]($ht * 0.10); $y1 = [int]($ht * 0.42); $nb = 0; $n = 0
    for ($y = $y0; $y -lt $y1; $y += 9) {
      for ($x = [int]($w * 0.08); $x -lt [int]($w * 0.92); $x += 9) {
        $c = $b.GetPixel($x, $y)
        if (($c.R + $c.G + $c.B) -gt 60) { $nb++ }
        $n++
      }
    }
    $g.Dispose(); $b.Dispose()
    $frac = [Math]::Round($nb / [double]$n, 3)
    $verdict = if ($frac -gt 0.05) { "content" } else { "BLACK" }
    Write-Output ("SURFACE {0,-22} {1}x{2} nonblack {3} -> {4}" -f $tag, $w, $ht, $frac, $verdict)
}

Shot "01-initial"
[SF]::MoveWindow($h, 200, 150, 700, 560, $true) | Out-Null; Start-Sleep -Milliseconds 900; Shot "02-small"
[SF]::MoveWindow($h, 100, 80, 1800, 950, $true) | Out-Null; Start-Sleep -Milliseconds 900; Shot "03-large"
# Rapid resize: the swapchain must not be left at a stale size by a burst.
for ($i = 0; $i -lt 8; $i++) {
    [SF]::MoveWindow($h, 150, 100, 800 + $i * 90, 500 + $i * 40, $true) | Out-Null
    Start-Sleep -Milliseconds 120
}
Start-Sleep -Milliseconds 800; Shot "04-after-rapid-resize"
[SF]::ShowWindow($h, 3) | Out-Null; Start-Sleep -Milliseconds 1000; Shot "05-maximized"
[SF]::ShowWindow($h, 9) | Out-Null; Start-Sleep -Milliseconds 1000; Shot "06-restored"
[SF]::ShowWindow($h, 6) | Out-Null; Start-Sleep -Milliseconds 900
[SF]::ShowWindow($h, 9) | Out-Null; Start-Sleep -Milliseconds 1200; Shot "07-after-minimize-restore"

# Fullscreen is Ctrl+Return in Trace, a Qt window-state change with the layout
# intact -- so the surface has to follow its parent rather than take over.
[SF]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 300
[System.Windows.Forms.SendKeys]::SendWait("^{ENTER}"); Start-Sleep -Milliseconds 1400; Shot "08-fullscreen"
[System.Windows.Forms.SendKeys]::SendWait("^{ENTER}"); Start-Sleep -Milliseconds 1400; Shot "09-after-fullscreen"

# Child window census: exactly one surface should exist, however many resizes
# and state changes happened.
[SF]::classes.Clear()
[SF]::EnumChildWindows($h, [SF+EnumProc]{ param($a,$b) [SF]::Collect($a,$b) }, [IntPtr]::Zero) | Out-Null
$surfaces = @([SF]::classes | Where-Object { $_ -eq "TraceD3D11Surface" })
Write-Output ("SURFACE child TraceD3D11Surface windows: {0}" -f $surfaces.Count)

$r = New-Object SF+RECT
[SF]::GetWindowRect($h, [ref]$r) | Out-Null
Write-Output ("SURFACE final geometry {0},{1} {2}x{3}" -f $r.L, $r.T, ($r.R-$r.L), ($r.B-$r.T))

# Shutdown with the app live. A hang or a crash here is the failure this exists
# to catch.
$sw = [Diagnostics.Stopwatch]::StartNew()
$p.CloseMainWindow() | Out-Null
$exited = $p.WaitForExit(8000)
if (-not $exited) {
    Get-Process -Name Trace -ErrorAction SilentlyContinue | Stop-Process -Force
    Write-Output "SURFACE shutdown: FAIL - did not exit within 8s"
    exit 1
}
Write-Output ("SURFACE shutdown: clean, exit code {0} in {1} ms" -f $p.ExitCode, [int]$sw.Elapsed.TotalMilliseconds)
